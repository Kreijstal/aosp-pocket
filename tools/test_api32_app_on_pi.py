#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test an unchanged API32 app through Manager, only in the dedicated VM73."""
import argparse
import base64
import functools
import hashlib
import http.server
import importlib.util
import json
from pathlib import Path
import re
import shlex
import threading
import time
import urllib.parse
import uuid
import zipfile
from test_api32_persistence_on_pi import native_fatals

PACKAGE='dev.kreijstal.aosppocket.api32managerimage'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.ManagerAppProbe32'


def audio_subworkloads_verified(app,spec):
    """Separate audio evidence never turns a failed combined AV trial into a pass."""
    if spec.get('package')!='dev.kreijstal.pocketav32test' or not app.get('avWorkloadRepeated'):return False
    grant=app.get('mediaPermissions',{});previous=None
    if grant.get('validation')!='PASS' or grant.get('grantUid')!=1000 or grant.get('via')!='PackageManager.grantRuntimePermission':return False
    for key in ['firstLaunch','reopened']:
        state=app.get(key,{});p=state.get('mediaAvProbe',{});output=p.get('playback',{});mic=p.get('microphone',{})
        if not (p.get('sdk')==32 and p.get('uid')==state.get('uid')==grant.get('appUid') and p.get('uid',0)>=10000
                and p.get('package')==spec['package'] and p.get('microphonePermission')
                and state.get('certificateSha256')==spec.get('certificateSha256')
                and state.get('certificateSha256')!='c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8'
                and output.get('validation')==mic.get('validation')=='PASS'
                and output.get('sampleRate')==mic.get('sampleRate')==48000 and output.get('channels')==2 and mic.get('channels')==1
                and output.get('writtenFrames')==mic.get('samples')==96000 and output.get('playedFrames',0)>=96000
                and mic.get('reads',0)>0 and mic.get('timestampFrame',0)>0 and mic.get('timestampNanos',0)>0
                and p.get('nonce') and p.get('finishedUptimeMillis',0)>=p.get('startedUptimeMillis',1)):return False
        if previous and (p['nonce']==previous['nonce'] or p['startedUptimeMillis']<previous['finishedUptimeMillis']):return False
        previous=p
    audio=app.get('stoppedSession',{}).get('audio',{});both=app.get('console','')+app.get('nativeLog','')
    closures=audio.get('microphonePeerClosures',0);closed=(closures>=2 and closures==audio.get('microphoneStreams')) if spec.get('requireCameraRecorderAudio') else closures==2
    return bool(audio.get('hostPcmBytesWritten',0)>=768000 and audio.get('microphoneStreams',0)>=2 and audio.get('microphoneDenied',0)==0 and closed
        and 'POCKET_AUDIO_HOST_STREAM open=1' in both and 'POCKET_AUDIO_IN_HOST_STREAM open=1' in both
        and 'POCKET_AUDIO_HOST_UNAVAILABLE' not in both and 'POCKET_AUDIO_IN_HOST_UNAVAILABLE' not in both
        and 'Error while writing data to HAL:' not in both and 'read failed: framesRead=' not in both)


def camera_workload_verified(camera):
    jpeg=camera.get('jpeg',{});frames=camera.get('previewFrames',[])
    return bool(camera.get('validation')=='PASS'
        and camera.get('cameraId') in camera.get('advertisedCameras',[]) and camera.get('captureCompletions',0)>0
        and len(frames)==3 and all(f.get('timestampNanos',0)>0 and f.get('maxLuma',0)>f.get('minLuma',255) for f in frames)
        and all(frames[i]['timestampNanos']<frames[i+1]['timestampNanos'] for i in range(2))
        and camera.get('previewWidth',0)>=160 and camera.get('previewHeight',0)>=120
        and jpeg.get('bytes',0)>100 and jpeg.get('width',0)>=160 and jpeg.get('height',0)>=120 and jpeg.get('sampledColors',0)>1
        and jpeg.get('timestampNanos',0)>0 and re.fullmatch('[a-f0-9]{64}',jpeg.get('sha256','')))


def camera_recording_verified(recording):
    frames=recording.get('pixels',[]);width=recording.get('width',0);height=recording.get('height',0)
    return bool(recording.get('validation')=='PASS' and recording.get('cameraId') in recording.get('advertisedCameras',[])
        and recording.get('container')=='mp4' and recording.get('mime')=='video/avc' and width>=160 and height>=120
        and 100<recording.get('bytes',0)<8*1024*1024 and re.fullmatch('[a-f0-9]{64}',recording.get('sha256',''))
        and 1000000<=recording.get('durationUs',0)<=20000000 and 12<=recording.get('packets',0)<=600
        and recording.get('initialSync') is True and recording.get('endOfStream') is True and recording.get('decodedFrames')==12
        and recording.get('captureCompletions',0)>=12 and recording.get('requestedFrameRate')==30 and recording.get('decoder')
        and len(frames)==12 and all(f.get('presentationTimeUs',-1)>=0 and f.get('maxLuma',0)-f.get('minLuma',255)>5
            and f['minLuma']<=f.get('meanLuma',-1)<=f['maxLuma'] and f.get('sampledPixels')==((width+3)//4)*((height+3)//4) for f in frames)
        and all(frames[i]['presentationTimeUs']<frames[i+1]['presentationTimeUs'] for i in range(11)))


def av_workload_verified(app,spec):
    if spec.get('package')!='dev.kreijstal.pocketav32test' or not app.get('avWorkloadRepeated'):return False
    if spec.get('requireCameraRecorderAudio') and not spec.get('requireCameraRecorder'):return False
    grant=app.get('mediaPermissions',{})
    permissions={'android.permission.CAMERA','android.permission.RECORD_AUDIO'}
    if not (grant.get('validation')=='PASS' and grant.get('grantUid')==1000 and grant.get('package')==spec['package']
            and grant.get('via')=='PackageManager.grantRuntimePermission' and set(grant.get('permissions',[]))==permissions):return False
    previous=None
    for key in ['firstLaunch','reopened']:
        state=app.get(key,{});p=state.get('mediaAvProbe',{})
        if not (p.get('validation')=='PASS' and p.get('stage')=='complete' and p.get('sdk')==32
                and p.get('uid')==state.get('uid')==grant.get('appUid') and p.get('uid',0)>=10000
                and p.get('package')==spec['package'] and set(state.get('avRequestedPermissions',[]))==permissions
                and state.get('certificateSha256')==spec.get('certificateSha256')
                and state.get('certificateSha256')!='c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8'
                and p.get('cameraPermission') and p.get('microphonePermission')):return False
        playback=p.get('playback',{});mic=p.get('microphone',{});camera=p.get('camera',{})
        if not (all(q.get('validation')=='PASS' for q in [playback,mic,camera])
                and playback.get('sampleRate')==48000 and playback.get('channels')==2 and playback.get('writtenFrames')==96000 and playback.get('playedFrames',0)>=96000
                and mic.get('sampleRate')==48000 and mic.get('channels')==1 and mic.get('samples')==96000 and mic.get('reads',0)>0
                and mic.get('timestampFrame',0)>0 and mic.get('timestampNanos',0)>0
                and camera_workload_verified(camera)):return False
        if spec.get('requireAllCameras'):
            cameras=p.get('cameras',[]);ids=camera.get('advertisedCameras',[])
            if not (ids and len(cameras)==len(ids) and len({q.get('cameraId') for q in cameras})==len(ids)
                    and {q.get('cameraId') for q in cameras}==set(ids)
                    and all(q.get('advertisedCameras')==ids and camera_workload_verified(q) for q in cameras)):return False
        if spec.get('requireCameraRecorder'):
            recordings=p.get('recordings',[]);ids=camera.get('advertisedCameras',[])
            if not (ids and len(recordings)==len(ids) and {q.get('cameraId') for q in recordings}==set(ids)
                    and all(q.get('advertisedCameras')==ids and camera_recording_verified(q) for q in recordings)):return False
        if spec.get('requireCameraRecorderAudio'):
            for recording in p.get('recordings',[]):
                audio=recording.get('audio',{});times=audio.get('decodedPresentationTimesUs',[])
                if not (audio.get('mime')=='audio/mp4a-latm' and audio.get('sampleRate')==48000 and audio.get('channels')==1
                        and 1000000<=audio.get('durationUs',0)<=20000000 and 12<=audio.get('packets',0)<=1200
                        and 0<audio.get('encodedBytes',0)<2*1024*1024 and audio.get('endOfStream') is True and audio.get('decoder')
                        and audio.get('firstPresentationTimeUs',-1)>=0 and audio.get('lastPresentationTimeUs',0)>audio.get('firstPresentationTimeUs',0)
                        and recording.get('firstPresentationTimeUs',-1)>=0 and recording.get('lastPresentationTimeUs',0)>recording.get('firstPresentationTimeUs',0)
                        and audio.get('videoOverlapUs',0)>=500000
                        and audio['videoOverlapUs']==min(recording['lastPresentationTimeUs'],audio['lastPresentationTimeUs'])-max(recording['firstPresentationTimeUs'],audio['firstPresentationTimeUs'])
                        and 8192<=audio.get('decodedSamples',0)<=16384
                        and 8<=audio.get('decoderOutputs',0)<=16 and len(times)==audio['decoderOutputs']
                        and all(t>=0 for t in times) and all(times[i]<times[i+1] for i in range(len(times)-1))
                        and audio.get('minPcm',0)<0<audio.get('maxPcm',0) and audio['maxPcm']-audio['minPcm']>100
                        and 50<audio.get('rms',0)<32767):return False
        if not p.get('nonce') or p.get('finishedUptimeMillis',0)<p.get('startedUptimeMillis',1):return False
        if previous and (p['nonce']==previous['nonce'] or p['startedUptimeMillis']<previous['finishedUptimeMillis']):return False
        previous=p
    return True


def codec_workload_verified(app,spec):
    if spec.get('package')!='dev.kreijstal.pocketcodec32test' or not app.get('codecWorkloadRepeated'):return False
    previous=None
    for key in ['firstLaunch','reopened']:
        state=app.get(key,{});p=state.get('mediaCodecProbe',{})
        if not (isinstance(p,dict) and p.get('validation')=='PASS' and p.get('stage')=='complete'
                and p.get('sdk')==32 and p.get('uid')==state.get('uid') and p.get('uid',0)>=10000
                and p.get('package')==spec['package'] and state.get('codecRequestedPermissions')==[]
                and state.get('certificateSha256')==spec.get('certificateSha256')
                and state.get('certificateSha256')!='c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8'):return False
        audio=p.get('aac',{});video=p.get('avc',{});pixels=video.get('pixels',[])
        if not (audio.get('sampleRate')==48000 and audio.get('channels')==1 and audio.get('inputSamples')==10240
                and 8192<=audio.get('decodedSamples',0)<=14336 and 0<audio.get('encodedBytes',0)<10240
                and 3000<audio.get('rms',0)<9000 and abs(audio.get('frequencyHz',0)-440)<50
                and audio.get('packets',0)>0 and audio.get('decoder',{}).get('outputs',0)>0
                and video.get('width')==video.get('height')==64 and video.get('inputFrames')==video.get('decodedFrames')==12
                and 0<video.get('encodedBytes',0)<36864 and video.get('decoder',{}).get('outputs')==12
                and len(pixels)==12 and {v.get('frame') for v in pixels}==set(range(12))
                and all(abs(v.get('y',-1000)-(32+v['frame']*5))<=12 and abs(v.get('u',-1000)-96)<=12 and abs(v.get('v',-1000)-160)<=12 for v in pixels)):return False
        codecs=p.get('codecs',[])
        for data,mime in [(audio,'audio/mp4a-latm'),(video,'video/avc')]:
            for name,encoder in [(data.get('encoder'),True),(data.get('decoder',{}).get('name'),False)]:
                if not name or not any(c.get('name')==name and c.get('encoder') is encoder and mime in c.get('types',[]) for c in codecs):return False
        if spec.get('requireSurfaceVideo'):
            surface=p.get('surfaceAvc',{});samples=surface.get('pixels',[])
            if not (surface.get('inputMode')=='egl-surface' and surface.get('eglVendor') and surface.get('glRenderer')
                    and surface.get('endOfStream') is True and surface.get('width')==surface.get('height')==64
                    and surface.get('inputFrames')==surface.get('decodedFrames')==surface.get('decoder',{}).get('outputs')==12
                    and 0<surface.get('encodedBytes',0)<36864 and surface.get('packets',0)>0
                    and len(samples)==12 and {s.get('frame') for s in samples}==set(range(12))
                    and all(s.get('presentationTimeUs')==(s['frame']+1)*100000
                        and abs(s.get('y',-1000)-(16+219*(64+s['frame']*8)/255.0))<=12
                        and abs(s.get('u',-1000)-128)<=12 and abs(s.get('v',-1000)-128)<=12 for s in samples)):return False
            for name,encoder in [(surface.get('encoder'),True),(surface.get('decoder',{}).get('name'),False)]:
                if not name or not any(c.get('name')==name and c.get('encoder') is encoder and 'video/avc' in c.get('types',[]) for c in codecs):return False
        if spec.get('requireDecoderSurface'):
            output=p.get('surfaceAvc',{}).get('outputSurface',{});samples=output.get('pixels',[])
            if not (output.get('outputMode')=='surface-texture' and output.get('eglVendor') and output.get('glRenderer')
                    and output.get('endOfStream') is True and output.get('width')==output.get('height')==64
                    and output.get('outputs')==12 and len(samples)==12 and {s.get('frame') for s in samples}==set(range(12))
                    and all(s.get('presentationTimeUs')==(s['frame']+1)*100000
                        and s.get('surfaceTimestampNanos')==(s['frame']+1)*100000000
                        and s.get('expectedGray')==64+s['frame']*8 and s.get('sampledPixels')==2304
                        and all(abs(s.get(channel,-1000)-(64+s['frame']*8))<=16 for channel in ['red','green','blue','minChannel','maxChannel'])
                        and s['minChannel']<=min(s['red'],s['green'],s['blue'])<=max(s['red'],s['green'],s['blue'])<=s['maxChannel'] for s in samples)):
                return False
            if not output.get('name') or not any(c.get('name')==output['name'] and c.get('encoder') is False and 'video/avc' in c.get('types',[]) for c in codecs):return False
        if not p.get('nonce') or p.get('finishedUptimeMillis',0)<p.get('startedUptimeMillis',1):return False
        if previous and (p['nonce']==previous['nonce'] or p['startedUptimeMillis']<previous['finishedUptimeMillis']):return False
        previous=p
    return True


def device_profile_verified(app,image):
    entries=image.get('manifest',{}).get('entries',{})
    policy=entries.get('system/etc/pocket-device-profile.json')
    if not policy:return False
    dynamic=entries.get('system/priv-app/DynamicSystemInstallationService/DynamicSystemInstallationService.apk',{})
    for key in ['firstLaunch','reopened']:
        p=app.get(key,{}).get('deviceProfile',{})
        if not (p.get('profile')=='rootless-v1' and p.get('policySha256')==policy.get('sha256')
                and p.get('multidisplayInstalled') is False and p.get('dynamicSystemPackagePresent') is True
                and p.get('dynamicSystemUid')==1000 and p.get('dynamicSystemApkSha256')==dynamic.get('sha256')
                and p.get('dynamicSystemBootReceiverEnabled') is False and p.get('runningUnsupportedPackages')==[]):return False
    return True


def embedded_manager_verified(app,require_network=False):
    checks=app.get('managerControlChecks',{})
    if not all(checks.get(key) is True for key in ['guestPeerDenied','unknownOperationRejected','badInstallArgumentsRejected']):return False
    owners=re.findall(r'API32_INPUT_CONTROL_READY[^\n]* owner=system_server pid=(\d+)',app.get('console',''))
    completed=re.findall(r'API32_MANAGER_CONTROL_COMPLETE operation=(\w+) pid=(\d+)',app.get('nativeLog',''))
    required={'install','open'}|({'network'} if require_network else set())
    if app.get('managerCrashResume') is True:
        if not (app.get('managerCrashRecoveryVerified') is True and app.get('noInstallAfterCrash') is True):return False
        before=app.get('managerCrashCheckpoint',{}).get('body',{});old_checks=before.get('managerControlChecks',{})
        if not all(old_checks.get(k) is True for k in ['guestPeerDenied','unknownOperationRejected','badInstallArgumentsRejected']):return False
        old_owners=re.findall(r'API32_INPUT_CONTROL_READY[^\n]* owner=system_server pid=(\d+)',before.get('preCrashConsole',''))
        old_completed=re.findall(r'API32_MANAGER_CONTROL_COMPLETE operation=(\w+) pid=(\d+)',before.get('preCrashNativeLog',''))
        if not (len(old_owners)==1 and required.issubset({operation for operation,pid in old_completed})
                and all(pid==old_owners[0] for operation,pid in old_completed)
                and 'API32_INPUT_PEER_DENIED pid='+str(old_checks.get('guestPeerPid')) in before.get('preCrashNativeLog','')
                and old_owners!=owners and not any(operation=='install' for operation,pid in completed)):return False
        required.discard('install')
    return bool(len(owners)==1 and required.issubset({operation for operation,pid in completed})
        and all(pid==owners[0] for operation,pid in completed)
        and checks.get('guestPeerPid',0)>0
        and 'API32_INPUT_PEER_DENIED pid='+str(checks['guestPeerPid']) in app.get('nativeLog',''))


def cross_uid_private_verified(app,spec,nonce):
    expected=spec['storageOwner'];uid=None;seeds=None
    initial='remounted' if spec.get('managerCrashRecovery') is True and app.get('managerCrashResume') is True else 'seed'
    for key,action in [('storageOwnerBefore',initial),('storageOwnerAfter','verify'),('storageOwnerCleanup','cleanup')]:
        state=app.get(key,{});proof=state.get('externalVolumeOwner',{})
        if not (state.get('package')==expected['package'] and state.get('uid',0)>=10000
                and state.get('sdk')==32 and state.get('minSdk')==32 and state.get('targetSdk')==32
                and state.get('versionCode')==expected['versionCode'] and state.get('splitCount')==0
                and state.get('certificateSha256')==expected['certificateSha256']
                and proof.get('validation')=='PASS' and proof.get('uid')==state['uid']
                and proof.get('nonce')==nonce and proof.get('action')==action and proof.get('package')==expected['package']):return False
        if uid is None:uid=state['uid']
        if uid!=state['uid']:return False
        files=proof.get('files',[])
        if len(files)!=2:return False
        for i,area in enumerate(['data','obb']):
            path='/storage/emulated/0/Android/'+area+'/'+expected['package']+('/files' if i==0 else '')+'/pocket-canary-'+nonce
            if files[i].get('area')!=area or files[i].get('path')!=path:return False
            if action=='cleanup':
                if files[i].get('removed') is not True:return False
            else:
                if not (files[i].get('ownReadVerified') is True and files[i].get('bytes',0)>0 and files[i].get('inode',0)>0):return False
                if action=='verify' and any(files[i].get(k)!=seeds[i].get(k) for k in ['bytes','inode','device']):return False
        if key=='storageOwnerBefore':seeds=files
    matrix=set()
    for area in ['data','obb']:
        relative='/Android/'+area+'/'+expected['package']+('/files' if area=='data' else '')
        for prefix in ['/storage/emulated/0','/sdcard','/data/media/0','/system-data/media/0']:
            directory=prefix+relative;path=directory+'/pocket-canary-'+nonce
            matrix.update([(path,'open',0),(path,'open',0x200000),
                (directory,'open',0x10000),(directory,'open',0x210000),(path,'stat',None)])
    for key in ['firstLaunch','reopened']:
        state=app.get(key,{});proof=state.get('externalVolumeApp',{});denials=proof.get('privateDenials',[])
        if not (state.get('uid',0)>=10000 and state['uid']!=uid and proof.get('uid')==state['uid']
                and proof.get('privateOwnerUid')==uid and proof.get('nonce')==nonce and proof.get('validation')=='PASS'
                and 'crossUidPrivateDenied' in proof.get('passed',[]) and len(denials)==len(matrix)
                and all(d.get('errno') in [1,2,13] for d in denials)
                and {(d.get('path'),d.get('operation'),d.get('flags')) for d in denials}==matrix):return False
    return not any(k in app for k in ['storageOwnerCleanupError','storageOwnerRequestCleanupError','storageRequestCleanupError'])


def namespace_syscalls_verified(app,nonce):
    required={'relativeDotdot','symlinkBeforeDotdot','symlinkPrivateDenied','trailingSlash',
        'pollAndFstatfs','renamedDirectoryAndProcFd','cwdRelative','unlinkedHeldFile','unlinkedHeldDirectory'}
    for key in ['firstLaunch','reopened']:
        state=app.get(key,{});proof=state.get('externalVolumeApp',{});namespace=proof.get('namespace',{})
        if not (proof.get('validation')=='PASS' and proof.get('nonce')==nonce
                and proof.get('cleanupComplete') is True and 'namespaceSyscalls' in proof.get('passed',[])
                and namespace.get('validation')=='PASS' and namespace.get('cleanupComplete') is True
                and namespace.get('directInProcessSyscalls') is True and namespace.get('uid',0)>=10000
                and namespace.get('uid')==proof.get('uid')==state.get('uid')
                and set(namespace.get('passed',[]))==required):return False
    return True


def manager_crash_recovery_verified(report):
    app=report.get('app',{});checkpoint=app.get('managerCrashCheckpoint',{});before=checkpoint.get('body',{})
    pre=app.get('managerCrashPreStart',{});request=pre.get('signalRequest',{});spec=report.get('appSpec',{})
    if not (spec.get('managerCrashRecovery') is True and spec.get('requireKeyCanary') is True
            and app.get('managerCrashRecoveryVerified') is True and app.get('managerCrashResume') is True
            and app.get('noInstallAfterCrash') is True and report.get('managerCrashRequested') is True
            and report.get('managerCrashOldPidAbsent') is True and report.get('testServerOfflineBeforeCrash') is True):return False
    old,new=report.get('crashedHostPid'),report.get('resumedHostPid')
    attempts=report.get('managerCrashResumeAttempts',[])
    if not (attempts and attempts[-1].get('event')=='new-recovery-activity-observed'
            and attempts[-1].get('hostPid')==new):return False
    if not (isinstance(old,int) and old>0 and isinstance(new,int) and new>0 and old!=new
            and checkpoint.get('hostPid')==old and pre.get('hostPid')==app.get('managerHostPid')==report.get('hostPid')==new
            and request.get('pid')==old and request.get('signal')==9 and checkpoint.get('runId')==request.get('runId')==report.get('runId')
            and request.get('checkpointSha256')==pre.get('checkpointSha256')==report.get('managerCrashCheckpointSha256')
            and checkpoint.get('managerSha256')==report.get('apkSha256')):return False
    if not (report.get('requestsBeforeCrash')==report.get('requests') and before.get('installed') is True and before.get('homeVerified') is True
            and 'API32_INIT_STOPPED' not in before.get('preCrashConsole','') and before.get('preCrashSession',{}).get('running') is True):return False
    native=before.get('preCrashNativeLog','')
    if ('ANR in ' in native or 'FATAL EXCEPTION' in native or native_fatals(native)
            or len(re.findall(r'Start proc (\d+):com\.android\.systemui/',native))!=1):return False
    processes=checkpoint.get('processes',[]);gone=app.get('managerCrashOldProcessesGone',[])
    if not (len(processes)>=10 and processes==gone and all(isinstance(p.get('pid'),int) and p.get('startTicks') for p in processes)):return False
    endpoints=pre.get('endpoints',[])
    if not any(p.get('path')=='dev/socket/pocket_volume32' and p.get('present') is True and p.get('mode')==0o140000 for p in endpoints):return False
    old_volume=before.get('preCrashSession',{}).get('externalStorage',{});new_volume=app.get('managerCrashNewSession',{}).get('externalStorage',{})
    if not (old_volume.get('attached') is True and new_volume.get('attached') is True and old_volume.get('epoch',0)!=0
            and new_volume.get('epoch',0)!=0 and old_volume['epoch']!=new_volume['epoch']):return False
    held=before.get('crashHeldOwner',{}).get('externalVolumeOwner',{});restored=app.get('crashRemountedOwner',{}).get('externalVolumeOwner',{})
    if not (held.get('heldFdOpen') is True and restored.get('newProcessNoHeldDescriptor') is True
            and held.get('pid')!=restored.get('pid') and held.get('uid')==restored.get('uid') and held.get('nonce')==restored.get('nonce')==report['runId']):return False
    if len(held.get('files',[]))!=2 or len(restored.get('files',[]))!=2:return False
    for old_file,new_file in zip(held['files'],restored['files']):
        if not (new_file.get('ownReadVerified') is True and all(old_file.get(k)==new_file.get(k) for k in ['area','path','bytes','inode'])):return False
    original=held.get('keyCanary',{});recovered=restored.get('keyCanary',{})
    if not (original.get('cryptoValid') is True and recovered.get('cryptoValid') is True and original.get('nonExportable') is True
            and recovered.get('nonExportable') is True and original.get('alias')==recovered.get('alias')=='pocket-volume-crash-'+report['runId']
            and re.fullmatch('[a-f0-9]{64}',original.get('cipherSha256','')) and original['cipherSha256']==recovered.get('cipherSha256')):return False
    cleanup=app.get('storageOwnerCleanup',{}).get('externalVolumeOwner',{}).get('keyCanary',{})
    return bool(cleanup.get('removed') is True and cleanup.get('cryptoValid') is True and cleanup.get('cipherSha256')==original['cipherSha256'])


def resume_crashed_manager(adb,activity,run_id,extra,old_pid,record):
    """Wait out the dead singleTask record; never kill/reinstall a process."""
    until=time.monotonic()+60;last_launch=0;attempts=[]
    while time.monotonic()<until:
        try:output=adb('shell','dumpsys activity '+activity+' pocket-app32',timeout=20)
        except Exception as error:
            attempts.append({'event':'dump-transient-error','error':str(error)});record(attempts);time.sleep(.25);continue
        rows=[line.strip().split('=',1)[1] for line in output.splitlines() if line.strip().startswith('POCKET_SESSION32=')]
        state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
        if state:
            if state.get('runId')!=run_id:raise RuntimeError('A different validation owns the recovery activity')
            if state.get('validation')=='FAIL':raise RuntimeError('Recovery activity failed: '+str(state.get('error')))
            if state.get('managerCrashResume') is True and state.get('managerHostPid')!=old_pid:
                attempts.append({'event':'new-recovery-activity-observed','hostPid':state['managerHostPid'],'stage':state.get('stage')})
                record(attempts);return attempts
        if 'no activities match:' in output and time.monotonic()-last_launch>=2:
            response=adb('shell','am start -n '+activity+' --es probe_run_id '+run_id+extra)
            attempts.append({'event':'launch-after-component-absent','response':response})
            record(attempts);last_launch=time.monotonic()
        time.sleep(.25)
    raise TimeoutError('Recovery activity was not acknowledged; inspect the live process before any restart')


def volume_reset_verified(value,nonce):
    if not (value.get('validation')=='PASS' and value.get('sdk')==32 and value.get('uid')==1000
            and value.get('nonce')==nonce and value.get('operation')=='reset' and value.get('via')=='IStorageManager'):return False
    if any(value.get(side)!={'id':'emulated;0','type':2,'state':2,'user':0} for side in ['before','after']):return False
    proof=value.get('reset',{});before=proof.get('propertyBefore')
    if not (proof.get('via')=='IStorageManager.setDebugFlags' and proof.get('property')=='persist.sys.adoptable'
            and before in ['', 'force_on', 'force_off'] and proof.get('propertyAfter')==before
            and proof.get('debugFlags')=={'':0,'force_on':1,'force_off':2}[before] and proof.get('debugMask')==3):return False
    began=proof.get('requestElapsedMillis');events=proof.get('events')
    if not (isinstance(began,int) and began>0 and isinstance(events,list) and 2<=len(events)<=128):return False
    phase=0
    for event in events:
        if not (event.get('id')=='emulated;0' and event.get('type')==2 and event.get('user')==0
                and isinstance(event.get('elapsedMillis'),int) and event['elapsedMillis']>=began):return False
        old,new=event.get('oldState'),event.get('newState')
        if phase==0 and (old,new)==(0,1):phase=1
        elif phase==1 and (old,new)==(1,2):phase=2
    return phase==2


def volume_lifecycle_verified(app,spec,nonce,provider_sha):
    lifecycle=app.get('volumeLifecycle',{})
    if lifecycle.get('validation')!='PASS':return False
    resetting=spec.get('volumeReset') is True
    if resetting and not volume_reset_verified(lifecycle.get('reset',{}),nonce):return False
    for key,before,after in ([('reset',2,2)] if resetting else [('unmount',2,0),('mount',0,2)]):
        value=lifecycle.get(key,{})
        if not (value.get('validation')=='PASS' and value.get('sdk')==32 and value.get('uid')==1000
                and value.get('nonce')==nonce and value.get('operation')==key and value.get('via')=='IStorageManager'):return False
        for side,state in [('before',before),('after',after)]:
            if value.get(side)!={'id':'emulated;0','type':2,'state':state,'user':0}:return False
    adapters=[('before',True,1,0),('reattached',True,2,1)]
    if not resetting:adapters.insert(1,('detached',False,1,1))
    for key,attached,attaches,revocations in adapters:
        value=lifecycle.get(key,{})
        if not (value.get('attached') is attached and value.get('attaches')==attaches and value.get('revocations')==revocations
                and value.get('workerStopped') is False and 'lastError' not in value):return False
    seed=app.get('storageOwnerBefore',{}).get('externalVolumeOwner',{});uid=seed.get('uid');pid=seed.get('pid');files=seed.get('files',[])
    if len(files)!=2 or not isinstance(pid,int) or pid<=0:return False
    before=lifecycle.get('before',{});reattached=lifecycle.get('reattached',{});detached=reattached if resetting else lifecycle.get('detached',{})
    if not (before.get('epoch',0)!=0 and (resetting or detached.get('epoch')==0) and reattached.get('epoch',0)!=0
            and reattached['epoch']!=before['epoch'] and pid in detached.get('retiredHolders',[])
            and lifecycle.get('oldOwnerAlive') is False):return False
    new_owner_pid=None
    owner_states=[('heldOwner','hold'),('remountedOwner','remounted')]
    if not resetting:owner_states.insert(1,('unmountedOwner','unmounted'))
    for key,action in owner_states:
        state=lifecycle.get(key,{});value=state.get('externalVolumeOwner',{})
        if not (state.get('uid')==uid and state.get('package')==spec['storageOwner']['package']
                and state.get('certificateSha256')==spec['storageOwner']['certificateSha256']
                and value.get('validation')=='PASS' and value.get('nonce')==nonce and value.get('action')==action
                and value.get('uid')==uid and isinstance(value.get('pid'),int) and value['pid']>0):return False
        if action=='unmounted':
            if not (state.get('primaryEmulatedMounted') is False and value.get('storageState')=='unmounted'
                    and value.get('newProcessNoHeldDescriptor') is True and value.get('pid')!=pid
                    and value.get('newOpenErrno') in [2,19,107]):return False
            new_owner_pid=value['pid']
            continue
        current=value.get('files',[])
        if len(current)!=2:return False
        for old,new in zip(files,current):
            if not (new.get('ownReadVerified') is True and all(new.get(k)==old.get(k) for k in ['area','path','bytes','inode'])):return False
            if action=='hold' and new.get('device')!=old.get('device'):return False
            if action=='remounted' and not new.get('device',0)>0:return False
        if action=='hold' and not (value.get('heldFdOpen') is True and value['pid']==pid):return False
        if action=='remounted' and not (value.get('newProcessNoHeldDescriptor') is True
                and (value['pid']!=pid if resetting else value['pid']==new_owner_pid)):return False
    ready=lifecycle.get('remountReady',{});state=lifecycle.get('remountedApp',{});value=state.get('externalVolumeApp',{})
    first=app.get('firstLaunch',{}).get('externalVolumeApp',{})
    if not (first.get('phase')=='initial' and isinstance(first.get('iteration'),int)
            and value.get('phase')=='remounted' and isinstance(value.get('iteration'),int)
            and (value['iteration']>first['iteration'] or value.get('pid')!=first.get('pid'))
            and value.get('validation')=='PASS' and value.get('cleanupComplete') is True
            and state.get('uid')==app.get('firstLaunch',{}).get('uid') and value.get('uid')==state.get('uid')
            and value.get('nonce')==nonce and state.get('package')==spec['package']
            and state.get('certificateSha256')==spec['certificateSha256']
            and {'mounted','registeredMediaStoreQuery','contentToPath','canonicalPathAndFd','providerRenameCoherence','fuseCreateAndMediaStore','fuseUnlinkCoherence','ownExternalFiles','crossUidPrivateDenied'}.issubset(set(value.get('passed',[])))):return False
    for inspected in [ready,state]:
        if not (inspected.get('primaryEmulatedMounted') is True and inspected.get('externalPrimaryQuery') is True
                and inspected.get('externalPrimaryQueryOwner')=='system-external-token' and inspected.get('externalPrimaryQueryRows')==0
                and inspected.get('mediaProvider',{}).get('sha256')==provider_sha):return False
    repeated=dict(app,firstLaunch=state,reopened=state)
    if not cross_uid_private_verified(repeated,spec,nonce):return False
    native=app.get('nativeLog','')
    if resetting and app.get('console','').count('API32_VOLD_RESET physical_disks=0 external_volumes=0 external_backend=selected')!=2:return False
    return bool(native.count('Starting session for id: emulated;0')==2 and 'Exiting session for id: emulated;0' in native
        and not any(k in app for k in ['volumeRecovery','volumeRecoveryError','storagePhaseCleanupError']))


def external_volume_verified(app,native,console,mounts=1):
    # The core's volume lifecycle uses stderr in init's inherited service
    # stream, while framework liblog is captured separately as nativeLog.
    # Require the actual mount record in the service stream and reject a
    # failure in either stream; a mounted Java volume alone is insufficient.
    adapter=app.get('stoppedSession',{}).get('externalStorage',{})
    return bool(console.count('API32_EXTERNAL_VOLUME_MOUNT result=0 ')==mounts
        and 'API32_EXTERNAL_VOLUME_MOUNT result=-' not in console+native
        and not re.search(r'API32_EXTERNAL_VOLUME_(?:STOP|ADAPTER)\b[^\n]*\bresult=-\d+',console+native)
        and all(app.get(key,{}).get('primaryEmulatedMounted') is True
            and app[key].get('externalPrimaryQuery') is True
            and app[key].get('externalPrimaryQueryRows')==0
            and app[key].get('externalPrimaryQueryOwner')=='system-external-token' for key in ['firstLaunch','reopened'])
        and adapter.get('attaches')==mounts and adapter.get('revocations')==mounts-1
        and adapter.get('attached') is False and adapter.get('workerStopped') is True
        and adapter.get('replyFailures',0)==0
        and 'lastError' not in adapter)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    parser.add_argument('--work-dir',type=Path)
    parser.add_argument('--package',choices=(PACKAGE,PACKAGE+'2',PACKAGE+'3'),default=PACKAGE,help='Exact isolated test Manager package')
    parser.add_argument('--require-fresh-install',action='store_true',help='Reject even an uninstalled package with retained data before installing; never reset existing data')
    parser.add_argument('--require-network',action='store_true',help='Also require real guest default-network state and network-bound HTTPS')
    parser.add_argument('--require-rpc-error-recovery',action='store_true',help='Also require fwmark timeout/EOF and subsequent-request recovery')
    parser.add_argument('--require-live-proc-discovery',action='store_true',help='Require live-proc retirement summaries below the native20s RPC deadline')
    parser.add_argument('--require-image-reuse',action='store_true',help='Require verified reuse of unchanged installed image bytes during a retained-profile upgrade')
    parser.add_argument('--allow-media-fixture',action='store_true',help='Grant and restore host camera/microphone permissions only for the exact ordinary AV fixture in VM73')
    parser.add_argument('--require-pending-install',action='store_true',help='Install the complete AV fixture before app readiness; require real wait-to-ready and responsive input')
    parser.add_argument('--require-manager-crash-recovery',action='store_true',help='Require authenticated outer Manager self-SIGKILL and offline recovery without APK reinstall or force-stop')
    args=parser.parse_args()
    package=args.package;activity=package+'/dev.kreijstal.aosppocket.runtime.ManagerAppProbe32'
    if args.require_fresh_install and args.require_image_reuse:parser.error('Fresh installation and retained-profile image reuse require separate runs')
    if args.require_rpc_error_recovery and not args.require_network:parser.error('--require-rpc-error-recovery requires --require-network')
    if args.require_pending_install and not args.allow_media_fixture:parser.error('--require-pending-install requires --allow-media-fixture')
    base=args.vm_root.resolve();work=(args.work_dir or base/'manager-app-validation').resolve()
    assert (base/'.pocket-api32-test-vm').is_file() and work.is_relative_to(base)
    config=base/'runtime/assembly/cuttlefish_config.json'
    assert config.resolve().is_relative_to(base) and set(json.loads(config.read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('app_vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    def adb(*argv,timeout=40):
        result=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if result.returncode:raise RuntimeError((result.stdout+result.stderr)[-2000:])
        return result.stdout.strip()
    metadata=json.loads((work/'build.json').read_text());apk=work/'api32-app-manager-probe.apk'
    assert metadata['probe'] and metadata['guestImagesBundled']==0 and metadata['guestSdks']==[28,29,32]
    assert metadata.get('package',PACKAGE)==package,'Wrong Manager sandbox in build receipt'
    assert hashlib.file_digest(apk.open('rb'),'sha256').hexdigest()==metadata['sha256']
    with zipfile.ZipFile(apk) as archive:
        assert package.encode('utf-16-le')+b'\0\0' in archive.read('AndroidManifest.xml'),'Wrong Manager sandbox in APK manifest'
        if metadata.get('hostNativeSourceBuild'):
            libraries=metadata['hostNativeSourceBuild']['libraries']
            assert set(libraries)=={'libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so'}
            for name,record in libraries.items():assert hashlib.sha256(archive.read('lib/arm64-v8a/'+name)).hexdigest()==record['sha256']
        image=[v for v in json.loads(archive.read('assets/pocket-images.json'))['images'] if v['sdk']==32][0]
        app=json.loads(archive.read('assets/api32-app-spec.json'))
        assert b'Ldev/kreijstal/aosppocket/runtime/ManagerAppProbe32;' in archive.read('classes.dex')
    assert app['minSdk']==32 and app['apkModified'] is False
    assert args.allow_media_fixture==(app.get('proof')=='media-av-v1'),'The AV fixture requires explicit isolated-host media testing'
    if args.allow_media_fixture:assert app['package']=='dev.kreijstal.pocketav32test'
    assert not app.get('requireLateCameraGrant') or args.allow_media_fixture
    assert not app.get('volumeReset') or app.get('volumeLifecycle') is True
    assert args.require_manager_crash_recovery==(app.get('managerCrashRecovery') is True)
    if args.require_manager_crash_recovery:
        assert app.get('requireKeyCanary') is True and app.get('proof')=='external-volume-v1' and not app.get('volumeLifecycle')
    assert not args.require_live_proc_discovery or app.get('volumeLifecycle') is True
    paths=[]
    downloads=[(image,'bytes','sha256'),(app,'archiveBytes','archiveSha256')]
    if 'storageOwner' in app:
        assert app['storageOwner']['package']=='dev.kreijstal.pocketvolumeowner32' and app['storageOwner']['minSdk']==32
        assert app['storageOwner']['apkModified'] is False
        downloads.append((app['storageOwner'],'archiveBytes','archiveSha256'))
    for item,size_key,digest_key in downloads:
        url=urllib.parse.urlsplit(item['url']);filename=url.path.removeprefix('/')
        assert url.scheme=='http' and url.hostname=='127.0.0.1' and url.port==8794 and '/' not in filename
        payload=work/filename
        assert payload.stat().st_size==item[size_key] and hashlib.file_digest(payload.open('rb'),'sha256').hexdigest()==item[digest_key]
        paths.append(url.path)
    report={'runId':uuid.uuid4().hex,'vmInstance':73,'serial':vm.serial(),'managerPackage':package,'apkSha256':metadata['sha256'],'imageSha256':image['sha256'],'appSpec':app,'requireNetwork':args.require_network,'requireFreshInstall':args.require_fresh_install,'requirePendingInstall':args.require_pending_install}
    commands=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),
        ('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs'),
        ('phantomPersistentFlag','getprop persist.sys.fflag.override.settings_enable_monitor_phantom_procs')]
    for name,command in commands:report[name+'Before']=adb('shell',command)
    assert report['selinuxBefore']=='Enforcing'
    if app.get('requireCleanAudioClose') or args.require_manager_crash_recovery:
        rows=re.findall(r'\bmax_phantom_processes=(\d+)\b',adb('shell','dumpsys activity settings'))
        report['hostPhantomMaxBefore']=int(rows[0]) if len(rows)==1 else None
    owners=adb('shell','ps -A -o PID,NAME,ARGS')
    assert not any(any(name in row for name in ['pocket-init-api32','pocket-core-api32','fuse-stock','pocket-media-client']) for row in owners.splitlines()),'An existing guest owns the VM'
    if args.require_fresh_install:
        known=adb('shell','pm list packages -u --user 0').splitlines()
        assert 'package:android' in known,'Package inventory unavailable'
        assert 'package:'+package not in known,'Manager package or retained data already exists; refusing a fresh-sandbox claim'
        report['packageKnownBeforeInstall']=False
    # Keep the launcher in front while replacing this APK so Android cannot
    # recreate a previous CE-probe task that would start another guest.
    adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
    print(adb('install','--no-incremental','-r',str(apk),timeout=180),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+package)
    installed=adb('shell','pm path '+package).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:]),timeout=90).split()[0]==metadata['sha256']
    if args.require_fresh_install:report['freshHostSandboxVerified']=True
    requests=[]
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path not in paths:self.send_error(404);return
            requests.append(self.path);super().do_GET()
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8794),functools.partial(Handler,directory=str(work)))
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();reverse=False
    try:
        if args.allow_media_fixture:
            package_dump=adb('shell','dumpsys package '+package);initial={}
            for permission in ['android.permission.CAMERA','android.permission.RECORD_AUDIO']:
                rows=re.findall(re.escape(permission)+r': granted=(true|false)\b',package_dump);assert len(rows)==1
                initial[permission]=rows[0]=='true'
            report['hostMediaPermissionsBefore']=initial
            report['hostCameraBefore']=adb('shell','dumpsys media.camera')
            report['hostAudioBefore']=adb('shell','dumpsys media.audio_flinger')
            for permission,granted in initial.items():
                if permission=='android.permission.CAMERA' and app.get('requireLateCameraGrant'):
                    assert not granted,'Late-grant fixture requires an initially denied host camera permission'
                    continue
                if not granted:adb('shell','pm grant '+package+' '+permission)
        adb('reverse','tcp:8794','tcp:8794');reverse=True
        adb('shell','am force-stop '+package)
        extra=' --ez require_network true' if args.require_network else ''
        if args.require_pending_install:extra+=' --ez require_pending_install true'
        print(adb('shell','am start -n '+activity+' --es probe_run_id '+report['runId']+extra),flush=True)
        try:
            pid=adb('shell','pidof '+package)
            if pid.isdigit() and int(pid)>0:report['hostPid']=int(pid)
        except Exception as error:report['hostPidLookupError']=str(error)
        last=None;errors=0;deadline=time.monotonic()+1500
        while time.monotonic()<deadline:
            try:output=adb('shell','dumpsys activity '+activity+' pocket-app32')
            except Exception:errors+=1;time.sleep(2);continue
            rows=[line.strip().split('=',1)[1] for line in output.splitlines() if line.strip().startswith('POCKET_SESSION32=')]
            state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if state.get('runId')!=report['runId']:time.sleep(1);continue
            if args.require_manager_crash_recovery and state.get('stage')=='awaiting-manager-crash' and not report.get('managerCrashRequested'):
                old=state['managerHostPid'];assert old==report['hostPid'] and state['managerCrashResume'] is False
                assert re.fullmatch('[a-f0-9]{64}',state['crashCheckpointSha256'])
                first=work/'manager-precrash-report.json';assert not first.exists()
                with first.open('x') as saved:json.dump(state,saved,indent=2)
                report['managerPreCrashReportSha256']=hashlib.file_digest(first.open('rb'),'sha256').hexdigest()
                report['managerCrashCheckpointSha256']=state['crashCheckpointSha256']
                report['managerCrashRequested']=True;report['crashedHostPid']=old
                report['requestsBeforeCrash']=list(requests)
                server.shutdown();server.server_close();thread.join(5);assert not thread.is_alive()
                adb('reverse','--remove','tcp:8794');reverse=False
                report['testServerOfflineBeforeCrash']=True
                try:report['managerCrashSignalAck']=adb('shell','dumpsys activity '+activity+' pocket-crash32 '+report['runId'])
                except Exception as error:report['managerCrashSignalTransportError']=str(error)
                until=time.monotonic()+30
                while True:
                    processes=adb('shell','ps -A -o PID,PPID,UID,NAME:128,ARGS')
                    rows=[line.split(None,4) for line in processes.splitlines()[1:]]
                    if not any(len(row)==5 and row[0]==str(old) for row in rows):break
                    if time.monotonic()>=until:raise TimeoutError('Exact pre-crash Manager PID remains live; do not restart it')
                    time.sleep(.2)
                (work/'post-crash-processes-before-resume.txt').write_text(processes)
                report['managerCrashOldPidAbsent']=True
                def record_resume(attempts):
                    target=work/'manager-crash-resume-attempts.json';temporary=work/'manager-crash-resume-attempts.json.tmp'
                    temporary.write_text(json.dumps(attempts,indent=2)+'\n');temporary.replace(target)
                report['managerCrashResumeAttempts']=resume_crashed_manager(adb,activity,report['runId'],extra,old,record_resume)
                report['hostPid']=None;last=None;continue
            if args.require_manager_crash_recovery and state.get('managerCrashResume') is True and not report.get('resumedHostPid'):
                current=adb('shell','pidof '+package);assert current.isdigit() and int(current)==state['managerHostPid']!=report['crashedHostPid']
                report['resumedHostPid']=report['hostPid']=int(current)
            if app.get('requireLateCameraGrant') and state.get('stage')=='awaiting-host-camera-grant' and not report.get('hostCameraGrantedAfterGuestBoot'):
                assert state.get('cameraHostPermissionDeniedBeforeGrant') and state.get('cameraPermissionGuestRunIdBefore')
                assert state.get('cameraInventoryBeforeHostGrant',{}).get('status')==0 and state['cameraInventoryBeforeHostGrant'].get('cameraCount',0)>0
                assert state.get('cameraStreamBeforeHostGrant',{}).get('status')==403
                rows=re.findall(r'android\.permission\.CAMERA: granted=(true|false)\b',adb('shell','dumpsys package '+package));assert rows==['false']
                adb('shell','pm grant '+package+' android.permission.CAMERA')
                rows=re.findall(r'android\.permission\.CAMERA: granted=(true|false)\b',adb('shell','dumpsys package '+package));assert rows==['true']
                report['hostCameraGrantedAfterGuestBoot']=True
            if not report.get('hostPid'):
                try:
                    pid=adb('shell','pidof '+package)
                    if pid.isdigit() and int(pid)>0:report['hostPid']=int(pid)
                except Exception:pass
            current=(state.get('stage'),state.get('managerStatus'))
            if current!=last:print('APP32_STAGE',*current,flush=True);last=current
            if state.get('validation'):report['app']=state;break
            time.sleep(1)
        else:raise TimeoutError('App validation timed out; inspect the exact live guest before any restart')
        report['pollErrors']=errors
        for name,command in commands:
            report[name+'After']=adb('shell',command);assert report[name+'After']==report[name+'Before']
        if app.get('requireCleanAudioClose') or args.require_manager_crash_recovery:
            rows=re.findall(r'\bmax_phantom_processes=(\d+)\b',adb('shell','dumpsys activity settings'))
            report['hostPhantomMaxAfter']=int(rows[0]) if len(rows)==1 else None
            report['hostDefaultPhantomPolicyVerified']=bool(report['hostPhantomMaxBefore']==report['hostPhantomMaxAfter']==32
                and report['phantomLimitAfter']=='null' and report['phantomMonitoringAfter']=='null'
                and report['phantomPersistentFlagAfter'] in ('','true'))
        native=report['app'].get('nativeLog','');console=report['app'].get('console','')
        report['anrs']=re.findall(r'ANR in ([^\n]+)',native)
        report['javaFatalHeaders']=re.findall(r'FATAL EXCEPTION(?: IN SYSTEM PROCESS)?: ([^\n]+)',native)
        report['nativeFatals']=native_fatals(native)
        report['systemUiStarts']=re.findall(r'Start proc (\d+):com\.android\.systemui/',native)
        report['nativeLogWithinLimit']=len(native.encode('utf-16-le','surrogatepass'))//2<2*1024*1024
        report['initStop']=re.findall(r'API32_INIT_STOPPED reaped=(\d+) remaining=(\d+) failed=(\d+)',console)
        stopped=report['app'].get('stoppedSession',{})
        if args.require_image_reuse:
            reuse=report['app'].get('imageInstallStats',{});entries=image['manifest']['entries']
            report['imageReuseVerified']=bool(reuse.get('imageSha256')==image['sha256'] and reuse.get('sdk')==32
                and reuse.get('reusedFiles',0)>0 and reuse.get('replacedFiles',0)>0
                and reuse.get('reusedFiles',0)+reuse.get('replacedFiles',0)==len(entries)
                and reuse.get('reusedBytes',0)>0 and reuse.get('reusedBytes',0)+reuse.get('writtenBytes',0)==image['unpacked_bytes'])
        if image.get('child_supervision')=='kernel-subreaper':
            descendants=re.findall(r'API32_INIT_DESCENDANTS_STOPPED reaped=(\d+) signalled=(\d+) remaining=(\d+) query_error=(-?\d+)',console)
            report['descendantStop']=descendants
            report['kernelChildOwnershipVerified']=bool(console.count('API32_INIT_SUBREAPER_READY kernel_child_ownership=1')==1
                and len(descendants)==1 and descendants[0][2:]==('0','0')
                and int(descendants[0][0])==len(re.findall(r'(?m)^INIT_DESCENDANT_REAP pid=\d+ code=\d+ status=\d+$',console))
                and 'API32_INIT_DESCENDANT_QUERY_FAILED' not in console)
        report['strictPassed']=bool(report['app'].get('validation')=='PASS' and report['app'].get('installed')
            and (not args.require_pending_install or report['app'].get('pendingAppOperationWaitVerified') is True)
            and (not args.require_fresh_install or report.get('freshHostSandboxVerified') is True)
            and report['app'].get('homeVerified') is True
            and report['app'].get('functionalPassed') and report['app'].get('display',{}).get('frames_presented',0)>0
            and report['nativeLogWithinLimit'] and not report['anrs'] and not report['javaFatalHeaders'] and not report['nativeFatals']
            and len(report['systemUiStarts'])==1 and report['initStop'] and all(row[1:]==('0','0') for row in report['initStop'])
            and stopped.get('ownerExitCode')==0 and stopped.get('ownerAlive') is False and not stopped.get('cleanupFailure')
            and stopped.get('input',{}).get('inputEventsRejected')==0 and paths[1] in requests
            and (not args.require_network or report['app'].get('network',{}).get('guest_network_connected') is True
                and report['app']['network'].get('http_via_guest_network') is True and report['app']['network'].get('http_status')==200
                and report['app']['network'].get('guest_validated') is True
                and report['app']['network'].get('guest_async_dns',{}).get('publicAsyncApi') is True
                and report['app']['network']['guest_async_dns'].get('uid',0)>=10000
                and report['app']['network']['guest_async_dns'].get('negativeRcode')==3
                and 'API32_DNS_PACKET_RESOLVE_PASS' in report['app'].get('rawDnsPermissions','')))
        report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if args.require_manager_crash_recovery:
            report['requests']=list(requests)
            report['managerCrashRecoveryVerified']=manager_crash_recovery_verified(report)
            report['strictPassed']=report['strictPassed'] and report['managerCrashRecoveryVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if app.get('requireLateCameraGrant'):
            report['hostCameraLateGrantVerified']=bool(report.get('hostCameraGrantedAfterGuestBoot') and report['app'].get('cameraHostPermissionDeniedBeforeGrant')
                and report['app'].get('cameraHostPermissionGrantedAfterBoot') and report['app'].get('cameraPermissionGuestRunIdBefore')
                and report['app']['cameraPermissionGuestRunIdBefore']==report['app'].get('cameraPermissionGuestRunIdAfter')
                and report['app'].get('cameraStreamBeforeHostGrant',{}).get('status')==403)
            report['strictPassed']=report['strictPassed'] and report['hostCameraLateGrantVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if app.get('proof')=='media-codec-v1':
            report['ordinaryCodecRoundTripsVerified']=codec_workload_verified(report['app'],app)
            report['strictPassed']=report['strictPassed'] and report['ordinaryCodecRoundTripsVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if app.get('proof')=='media-av-v1':
            report['ordinaryAudioSubworkloadsVerified']=audio_subworkloads_verified(report['app'],app)
            report['ordinaryAudioCameraWorkloadsVerified']=av_workload_verified(report['app'],app)
            audio=stopped.get('audio',{});both=console+native
            report['hostMediaBridgeVerified']=bool(audio.get('hostPcmBytesWritten',0)>0 and audio.get('microphoneStreams',0)>0
                and audio.get('microphoneDenied',0)==0 and 'POCKET_AUDIO_HOST_STREAM open=1' in both
                and 'POCKET_AUDIO_IN_HOST_STREAM open=1' in both and 'POCKET_CAMERA_HOST_STREAM ' in both
                and 'POCKET_CAMERA_HOST_UNAVAILABLE' not in both and 'POCKET_AUDIO_IN_HOST_UNAVAILABLE' not in both)
            report['strictPassed']=report['strictPassed'] and report['ordinaryAudioCameraWorkloadsVerified'] and report['hostMediaBridgeVerified']
            if app.get('requireCleanAudioClose'):
                report['brokerCleanAudioCloseVerified']=bool(audio.get('streamErrors')==0
                    and audio.get('microphonePeerClosures',0)>=2 and audio.get('microphoneStreams',0)>=2
                    and audio.get('completedStreams',0)>=4 and not audio.get('lastStreamErrorClass'))
                report['strictPassed']=report['strictPassed'] and report['brokerCleanAudioCloseVerified'] and report['ordinaryAudioSubworkloadsVerified'] and report['hostDefaultPhantomPolicyVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if args.require_rpc_error_recovery:
            text=report['app'].get('netdRole_stack','')
            report['fwmarkErrorRecoveryVerified']=all(marker in text for marker in ['API32_FWMARK_ERRORS_PASS','idle_timeout=1','eof=1','subsequent_request=1','daemon_alive=1'])
            report['strictPassed']=report['strictPassed'] and report['fwmarkErrorRecoveryVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if args.require_image_reuse:
            report['strictPassed']=report['strictPassed'] and report['imageReuseVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if image.get('child_supervision')=='kernel-subreaper':
            report['strictPassed']=report['strictPassed'] and report['kernelChildOwnershipVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if 'system/etc/pocket-device-profile.json' in image['manifest']['entries']:
            report['rootlessDeviceProfileVerified']=device_profile_verified(report['app'],image)
            report['strictPassed']=report['strictPassed'] and report['rootlessDeviceProfileVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if 'system/etc/pocket-manager-control32' in image['manifest']['entries']:
            report['embeddedManagerControlVerified']=embedded_manager_verified(report['app'],args.require_network)
            report['strictPassed']=report['strictPassed'] and report['embeddedManagerControlVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if image.get('external_volume')=='rootless-v1':
            report['externalVolumeVerified']=external_volume_verified(report['app'],native,console,2 if app.get('volumeLifecycle') is True else 1)
            report['strictPassed']=report['strictPassed'] and report['externalVolumeVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if app.get('proof')=='external-volume-v1':
            required={'mounted','registeredMediaStoreQuery','contentToPath','canonicalPathAndFd','providerRenameCoherence','fuseCreateAndMediaStore','fuseUnlinkCoherence','ownExternalFiles'}
            report['ordinaryExternalVolumeVerified']=all((proof:=report['app'].get(key,{}).get('externalVolumeApp',{})).get('validation')=='PASS'
                and proof.get('cleanupComplete') is True and proof.get('uid')==report['app'][key].get('uid')
                and proof.get('nonce')==report['runId']
                and required.issubset(set(proof.get('passed',[]))) for key in ['firstLaunch','reopened'])
            report['strictPassed']=report['strictPassed'] and report['ordinaryExternalVolumeVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if app.get('requireNamespaceSyscalls') is True:
            report['ordinaryNamespaceSyscallsVerified']=namespace_syscalls_verified(report['app'],report['runId'])
            report['strictPassed']=report['strictPassed'] and report['ordinaryNamespaceSyscallsVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if image.get('private_dir_policy')=='provider-fuse-v1':
            expected=image['manifest']['entries']['apex/com.android.mediaprovider/priv-app/MediaProvider/MediaProvider.apk']['sha256']
            report['sourceBuiltPrivateProviderVerified']=bool('POCKET_MEDIA_PRIVATE_FUSE_POLICY enabled=1 owner_checks=MediaProvider kernel_bind_mounts=none' in console+native
                and all(report['app'].get(key,{}).get('mediaProvider',{}).get('sha256')==expected
                    and report['app'][key]['mediaProvider'].get('package')=='com.android.providers.media.module'
                    and report['app'][key]['mediaProvider'].get('uid',0)>=10000 for key in ['firstLaunch','reopened']))
            report['strictPassed']=report['strictPassed'] and report['sourceBuiltPrivateProviderVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if 'storageOwner' in app:
            report['crossUidPrivateStorageVerified']=cross_uid_private_verified(report['app'],app,report['runId']) and paths[2] in requests
            report['strictPassed']=report['strictPassed'] and report['crossUidPrivateStorageVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        if app.get('volumeLifecycle') is True:
            expected=image['manifest']['entries']['apex/com.android.mediaprovider/priv-app/MediaProvider/MediaProvider.apk']['sha256']
            report['frameworkVolumeLifecycleVerified']=volume_lifecycle_verified(report['app'],app,report['runId'],expected)
            report['strictPassed']=report['strictPassed'] and report['frameworkVolumeLifecycleVerified']
            if app.get('volumeReset') is True:
                report['frameworkVolumeResetVerified']=volume_reset_verified(report['app'].get('volumeLifecycle',{}).get('reset',{}),report['runId'])
                report['strictPassed']=report['strictPassed'] and report['frameworkVolumeResetVerified']
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        server.shutdown();server.server_close();thread.join(5)
        if reverse:adb('reverse','--remove','tcp:8794')
        if args.allow_media_fixture and 'hostMediaPermissionsBefore' in report:
            try:
                report['hostCameraAfter']=adb('shell','dumpsys media.camera')
                report['hostAudioAfter']=adb('shell','dumpsys media.audio_flinger')
                owners=adb('shell','ps -A -o PID,NAME,ARGS')
                assert not any(any(n in row for n in ['pocket-init-api32','pocket-core-api32','fuse-stock']) for row in owners.splitlines()),'Media permissions retained while exact guest is still live'
                for permission,granted in report['hostMediaPermissionsBefore'].items():
                    if not granted:adb('shell','pm revoke '+package+' '+permission)
                package_dump=adb('shell','dumpsys package '+package);after={}
                for permission in report['hostMediaPermissionsBefore']:
                    rows=re.findall(re.escape(permission)+r': granted=(true|false)\b',package_dump);assert len(rows)==1
                    after[permission]=rows[0]=='true'
                report['hostMediaPermissionsAfter']=after
                report['hostMediaPermissionsRestored']=after==report['hostMediaPermissionsBefore']
                assert report['hostMediaPermissionsRestored']
            except Exception as error:
                report['hostMediaCleanupError']=str(error);report['strictPassed']=False;report['validation']='FAIL'
        report['requests']=requests
        if report.get('hostPid'):
            try:report['adapterHostLog']=adb('shell','logcat -d --pid='+str(report['hostPid'])+' -s PocketExternal32:I PocketHolders32:I *:S')
            except Exception as error:report['adapterHostLogError']=str(error)
        if args.require_live_proc_discovery:
            rows=re.findall(r'phase=summary scan=(\w+) entries=(\d+) profile_records=(\d+) ordinary=(\d+) live=(\d+) result=(-?\d+) cached_stat=(\d+) elapsed_ms=(\d+)',report.get('adapterHostLog',''))
            report['holderDiscoverySummaries']=[dict(zip(['scan','entries','profileRecords','ordinary','live','result','cachedStat','elapsedMillis'],[row[0],*map(int,row[1:])])) for row in rows]
            report['liveProcessDiscoveryVerified']=bool(len(rows)==2 and [int(row[5]) for row in rows]==[1,0]
                and all(row[0]=='proc' and int(row[7])<20000 for row in rows))
            report['strictPassed']=bool(report.get('strictPassed') and report['liveProcessDiscoveryVerified'])
            report['validation']='PASS' if report['strictPassed'] else 'FAIL'
        (work/'manager-app-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({key:report.get(key) for key in ('validation','strictPassed','runId','error','anrs','systemUiStarts')}),flush=True)
    if not report['strictPassed']:raise SystemExit(1)


if __name__=='__main__':main()
