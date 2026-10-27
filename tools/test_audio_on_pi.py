#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate guest audio on the dedicated Pi Android VM.

Output: guest AudioTrack -> AudioFlinger -> Pocket HAL -> host AudioTrack.
Input (--mic-probe): host AudioRecord -> Pocket HAL -> AudioFlinger -> guest AAudio stream.
"""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
import threading
import time

sys.path[:0]=['/home/pi/lineage-cuttlefish/tools','/home/pi']
import cuttlefish_arm64 as c
from pocketctl import request

PACKAGE='dev.kreijstal.aosppocket.runtime'
ROOT='/data/user/0/'+PACKAGE+'/files/api28-native-rootfs'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install',type=Path,help='Update the Manager first; the guest must be stopped')
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--mic-probe',type=Path,help='Native AAudio capture probe (runtime/tests/mic_probe.c)')
    args=parser.parse_args()
    c.require_virtual()
    config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
    def root(command):return c.shell('su -c '+shlex.quote(command))
    def rpc(op,argv=()):
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,op,argv,stdout=out,stderr=err)
        if code:raise RuntimeError((code,out.getvalue().decode(),err.getvalue().decode()))
        return out.getvalue().decode()
    result={'scope':'Pocket Android 9 on dedicated Pi Android VM'}
    def save():(Path.home()/'audio-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    if args.install:
        state=json.loads(rpc(1))
        if state['running']:
            rpc(4);deadline=time.monotonic()+90
            while time.monotonic()<deadline:
                state=json.loads(rpc(1))
                if not state['running'] and not state.get('stopping'):break
                time.sleep(2)
        assert not state['running'] and not state.get('stopping'),'Guest must be stopped before the update'
        installed=c.adb('-s',c.serial(),'install','--streaming','-r',str(args.install),check=False,timeout=360)
        result['install']=installed.stdout.strip();save()
        if installed.returncode:raise RuntimeError('Manager update failed: '+installed.stderr.strip())
        root('am start-foreground-service -n '+PACKAGE+'/.GuestService -a dev.kreijstal.aosppocket.ADMIN')
        c.shell('am start -n '+PACKAGE+'/.MainActivity --ei guest_sdk 28 --ez start_guest true')
    deadline=time.monotonic()+300
    while time.monotonic()<deadline:
        try:
            state=json.loads(rpc(1))
            if state['running'] and state['ready'] and rpc(2,['/system/bin/getprop','sys.boot_completed']).strip()=='1':break
        except (OSError,ValueError,RuntimeError):pass
        time.sleep(3)
    else:raise RuntimeError('Guest did not complete boot')
    result['version']=c.shell('dumpsys package '+PACKAGE+' | grep versionName').strip()
    result['boot_completed']=True
    uid=root('stat -c %u /data/user/0/'+PACKAGE).strip()
    destination='/data/local/tmp/pocket-audio-probe.jar'
    c.adb('-s',c.serial(),'push',str(args.probe),destination,check=True,timeout=60)
    physical=ROOT+'/system-data'+destination[len('/data'):]
    root('cp '+destination+' '+physical+' && chmod 0644 '+physical)
    command='export CLASSPATH='+destination+'; exec /system/bin/pocket-app-process-api28 /system/bin AudioProbe 4'
    probe={}
    def play():
        out,err=io.BytesIO(),io.BytesIO()
        probe['code']=request(config,2,['/system/bin/toybox','timeout','60','/system/bin/sh','-c',command],stdout=out,stderr=err)
        probe['stdout'],probe['stderr']=out.getvalue().decode(),err.getvalue().decode()[-2000:]
    def host_players():return [l.strip() for l in c.shell('dumpsys audio').splitlines() if 'new player' in l and 'uid/pid:'+uid+'/' in l]
    before=set(host_players())
    worker=threading.Thread(target=play);worker.start()
    # Host evidence while the tone plays: the Manager's own AudioTrack, seen by the host's audio services.
    time.sleep(3)
    flinger=c.shell('dumpsys media.audio_flinger')
    worker.join(90)
    result.update(exitCode=probe.get('code'),stdout=probe.get('stdout',''),stderr=probe.get('stderr',''))
    result['host_uid']=uid
    result['host_players']=[l for l in host_players() if l not in before][-6:]
    result['host_flinger_tracks']=[l.strip() for l in flinger.splitlines() if ' '+uid+' ' in l][-6:]
    result['guest_log']=rpc(2,['/system/bin/sh','-c','/system/bin/toybox grep -E "POCKET_AUDIO|audioserver|AudioFlinger|AudioPolicy|DIRECT_HAL_(PASSTHROUGH|LOAD|FETCH).*audio" /logs/manager-init.log | /system/bin/toybox grep -v throttle | /system/bin/toybox tail -n 40'])
    # The HAL reports the host stream once per boot, so read its markers apart from the AudioFlinger noise.
    result['guest_markers']=rpc(2,['/system/bin/toybox','grep','-E','^POCKET_AUDIO','/logs/manager-init.log'])
    ok=result['exitCode']==0 and 'AUDIO_PROBE_PASS' in result['stdout'] and 'POCKET_AUDIO_HOST_STREAM open=1' in result['guest_markers']
    result['validation']='PASS' if ok and (result['host_players'] or result['host_flinger_tracks']) else 'FAIL'
    if args.mic_probe:
        # The dedicated VM's Manager may lack the host permission; the broker refuses capture without it.
        root('pm grant '+PACKAGE+' android.permission.RECORD_AUDIO')
        destination='/data/local/tmp/pocket-mic-probe'
        c.adb('-s',c.serial(),'push',str(args.mic_probe),destination,check=True,timeout=60)
        physical=ROOT+'/system-data'+destination[len('/data'):]
        root('cp '+destination+' '+physical+' && chmod 0755 '+physical)
        manager=c.shell('pidof '+PACKAGE).split()[0]
        since=c.shell('date +%m-%d\\ %H:%M:%S.000').strip()
        mic={}
        # The host AudioFlinger's track history: the Manager's record tracks, by their add event.
        def host_records():return [l.strip() for l in c.shell('dumpsys media.audio_flinger').splitlines() if 'AT::add' in l and ' '+manager+'/' in l]
        before=set(host_records())
        def capture():
            out,err=io.BytesIO(),io.BytesIO()
            mic['code']=request(config,2,['/system/bin/toybox','timeout','30',destination,'3'],stdout=out,stderr=err)
            mic['stdout'],mic['stderr']=out.getvalue().decode(),err.getvalue().decode()[-2000:]
        worker=threading.Thread(target=capture);worker.start()
        worker.join(60)
        records=c.shell('logcat -d -T '+shlex.quote(since)+' --pid='+manager+' -s AudioRecord')
        markers=rpc(2,['/system/bin/toybox','grep','-E','^POCKET_AUDIO_IN','/logs/manager-init.log'])
        result['mic']={'exitCode':mic.get('code'),'stdout':mic.get('stdout',''),'stderr':mic.get('stderr',''),
            'host_manager_pid':manager,'host_record_log':[l.strip() for l in records.splitlines() if 'AudioRecord' in l][-8:],
            'host_record_tracks':[l for l in host_records() if l not in before],
            'guest_markers':markers}
        mic_ok=mic.get('code')==0 and 'MIC_PROBE_PASS' in mic.get('stdout','') and \
            'POCKET_AUDIO_IN_HOST_STREAM open=1' in markers and bool(result['mic']['host_record_tracks'])
        result['mic']['validation']='PASS' if mic_ok else 'FAIL'
        if not mic_ok:result['validation']='FAIL'
    save();print(json.dumps(result,indent=2))
    assert result['validation']=='PASS',result['validation']

if __name__=='__main__':main()
