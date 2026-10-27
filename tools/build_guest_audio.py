#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build guest audio for the ARM64-only Android 9 guest.

64-bit audioserver (AudioFlinger + AudioPolicyService), the stock passthrough
audio and effects HALs, and audio.primary.default from audio_hal_pocket.c,
which streams the primary output to the host Manager and the host microphone
back as the primary input.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT
from build_boot import reserve_interpreter
from build_guest_media import build as build_media
from prepare_boot_resources import prepare_resources

POLICY='''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<!-- Pocket guest: one stereo 48 kHz output and one mono 48 kHz microphone, both streamed through the host. -->
<audioPolicyConfiguration version="1.0" xmlns:xi="http://www.w3.org/2001/XInclude">
    <globalConfiguration speaker_drc_enabled="false"/>
    <modules>
        <module name="primary" halVersion="2.0">
            <attachedDevices>
                <item>Speaker</item>
                <item>Built-In Mic</item>
            </attachedDevices>
            <defaultOutputDevice>Speaker</defaultOutputDevice>
            <mixPorts>
                <mixPort name="primary output" role="source" flags="AUDIO_OUTPUT_FLAG_PRIMARY">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                             samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
                </mixPort>
                <mixPort name="primary input" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                             samplingRates="48000" channelMasks="AUDIO_CHANNEL_IN_MONO"/>
                </mixPort>
            </mixPorts>
            <devicePorts>
                <devicePort tagName="Speaker" type="AUDIO_DEVICE_OUT_SPEAKER" role="sink"/>
                <devicePort tagName="Built-In Mic" type="AUDIO_DEVICE_IN_BUILTIN_MIC" role="source"/>
            </devicePorts>
            <routes>
                <route type="mix" sink="Speaker" sources="primary output"/>
                <route type="mix" sink="primary input" sources="Built-In Mic"/>
            </routes>
        </module>
    </modules>
    <xi:include href="audio_policy_volumes.xml"/>
    <xi:include href="default_volume_tables.xml"/>
</audioPolicyConfiguration>
'''
# No effect libraries are shipped; the factory still loads so AudioFlinger has one.
EFFECTS='''<?xml version="1.0" encoding="UTF-8"?>
<audio_effects_conf version="2.0" xmlns="http://schemas.android.com/audio/audio_effects_conf/v2_0">
    <libraries/>
    <effects/>
</audio_effects_conf>
'''
SERVICE='''
service pocket-audioserver /system/bin/audioserver
    class pocket-zygote
    user 1041
    group 1041
'''
VERIFY='''
service pocket-audio-verify /system/bin/pocket-audio-verify
    class pocket-zygote
    user 1000
    group 1000
    oneshot
'''

def build(base_apk,ndk,output):
    output.mkdir(parents=True,exist_ok=True)
    payload=dict(build_media(base_apk,ndk,output/'guest-media',audio=True))
    api=ROOT/'out/api28';av=api/'frameworks-av';native=api/'frameworks-native';libs=output/'guest-media/lib64'
    platform=prepare_resources()
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    def run(argv):subprocess.run(list(map(str,argv)),check=True)
    hal=output/'audio.primary.default.so'
    run([llvm/'aarch64-linux-android28-clang','-std=c11','-O2','-fPIC','-shared','-Wall','-Wextra','-Werror',
         '-I'+str(ROOT/'out/composer-headers/include'),'-I'+str(api/'system-media/audio/include'),
         '-I'+str(api/'system-core/libcutils/include'),'-I'+str(api/'system-core/libsystem/include'),
         ROOT/'runtime/direct/audio_hal_pocket.c','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
         '-Wl,-soname,audio.primary.default.so','-o',hal])
    payload['vendor/lib64/hw/audio.primary.default.so']=str(hal.resolve())
    includes=[ROOT/'runtime/direct/audio_aidl',api/'headers',output/'guest-media/generated',
              native/'libs/binder/include',native/'include',native/'libs/gui/include',native/'libs/ui/include',
              native/'headers/media_plugin',api/'libhidl/base/include',api/'libhidl/transport/include',api/'libhwbinder/include',
              av/'include',av/'media/libmedia/include',av/'media/libaudioclient/include',av/'media/utils/include',
              av/'media/libnbaio/include',av/'media/libaudiohal/include',av/'media/libaudioprocessing/include',
              av/'media/libeffects/include',av/'media/libcpustats/include',av/'media/libmediametrics/include',
              av/'services/audioflinger',av/'services/audiopolicy',av/'services/audiopolicy/service',
              av/'services/audiopolicy/common/include',av/'services/audiopolicy/common/managerdefinitions/include',
              av/'services/audiopolicy/engine/interface',api/'system-media/audio/include',api/'system-media/audio_utils/include',
              ROOT/'out/composer-headers/include']
    includes+=[api/'system-core'/p/'include' for p in ('libutils','libcutils','liblog','libsystem','base')]
    # AOSP's own headers are not warning-clean under this NDK; our source is.
    cxx=[llvm/'aarch64-linux-android28-clang++','-std=gnu++17','-nostdinc++','-isystem',api/'libcxx/include','-nostdlib++',
         '-fno-rtti','-fno-exceptions','-include',ROOT/'runtime/direct/hidl_codegen_compat.h','-O2','-Wall','-Werror',
         '-Wno-unused-parameter','-Wno-tautological-compare','-Wno-inconsistent-missing-override',
         '-Wno-unused-private-field','-Wno-deprecated-declarations',*['-I'+str(p) for p in includes]]
    # The reference image's 64-bit libaudioflinger uses ELF TLS (tlNBLogWriter), which the API 28
    # linker rejects; stock Android 9 only loads the 32-bit copy. Rebuild it so thread_local is emulated.
    flinger=output/'libaudioflinger.so';sources=av/'services/audioflinger'
    run([*cxx,'-fPIC','-shared','-fvisibility=hidden','-I'+str(av/'services/medialog'),'-I'+str(api/'system-core/libmemunreachable/include'),
         '-Wno-implicit-const-int-float-conversion','-Wno-unused-but-set-variable','-Wno-bitwise-instead-of-logical',
         '-DSTATE_QUEUE_INSTANTIATIONS="StateQueueInstantiations.cpp"',
         *[sources/(name+'.cpp') for name in ('AudioFlinger','Threads','Tracks','AudioHwDevice','AudioStreamOut','SpdifStreamOut',
           'Effects','PatchPanel','StateQueue','BufLog','TypedLogger','AudioWatchdog','FastCapture','FastCaptureDumpState',
           'FastCaptureState','FastMixer','FastMixerDumpState','FastMixerState','FastThread','FastThreadDumpState','FastThreadState')],
         av/'media/libcpustats/ThreadCpuUsage.cpp',av/'media/libcpustats/CentralTendencyStatistics.cpp',
         '-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
         *['-l'+name for name in ('audiohal','audioprocessing','audiospdif','audioutils','cutils','utils','log','binder','audioclient',
           'medialogservice','mediautils','nbaio','nblog','powermanager','serviceutility','memunreachable','media_helper','c++')],
         '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-Wl,-soname,libaudioflinger.so','-o',flinger])
    assert b'TLS' not in subprocess.run([llvm/'llvm-readelf','-lW',flinger],capture_output=True,check=True).stdout
    payload['system/lib64/libaudioflinger.so']=str(flinger.resolve())
    server=output/'audioserver'
    run([*cxx,'-fPIE','-pie',ROOT/'runtime/direct/pocket_audioserver.cpp',
         '-L'+str(output),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-laudioflinger','-laudiopolicyservice','-lhidltransport',
         '-lbinder','-lutils','-lc++','-llog','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',server])
    assert reserve_interpreter(server)
    payload['system/bin/audioserver']=str(server.resolve())
    for relative in ('vendor/lib64/hw/android.hardware.audio@2.0-impl.so','vendor/lib64/hw/android.hardware.audio.effect@2.0-impl.so',
                     'vendor/lib64/libeffects.so','vendor/etc/audio_policy_volumes.xml','vendor/etc/default_volume_tables.xml'):
        target=output/'platform'/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(platform/relative,target);payload[relative]=str(target.resolve())
    for name,text in (('audio_policy_configuration.xml',POLICY),('audio_effects.xml',EFFECTS)):
        target=output/name;target.write_text(text);payload['vendor/etc/'+name]=str(target.resolve())
    # The media build already rewrote init.syscall.rc from the base APK; extend that copy.
    rc_path=Path(payload['system/etc/init/init.syscall.rc']);rc=rc_path.read_text()
    if VERIFY in rc: rc=rc.replace(VERIFY,'')
    if '\nservice pocket-audioserver ' not in rc: rc+=SERVICE
    target=output/'init.audio.rc';target.write_text(rc);payload['system/etc/init/init.syscall.rc']=str(target.resolve())
    (output/'payload.json').write_text(json.dumps(payload,indent=2)+'\n')
    print('PASS: guest audioserver, passthrough audio HALs and Pocket host audio HAL built',flush=True)
    return payload


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--out-dir',type=Path,required=True)
    a=p.parse_args();build(a.base_apk.resolve(),a.ndk.resolve(),a.out_dir.resolve())
