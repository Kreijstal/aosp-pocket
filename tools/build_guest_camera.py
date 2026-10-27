#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build guest cameras for the ARM64-only Android 9 guest, on top of guest audio.

A 64-bit cameraserver (the stock libcameraservice) whose "legacy/0" camera provider runs
in process: AOSP's external camera HAL (device@3.4), patched by
runtime/camera/external-camera-pocket.patch to stream I420 frames from the host cameras
through the broker instead of V4L2. Output is cropped, scaled and JPEG-encoded by that HAL.
"""
import argparse
import json
import re
from pathlib import Path
import shutil
import subprocess
import zipfile
from concurrent.futures import ThreadPoolExecutor
from target import ROOT
from build_boot import reserve_interpreter
from build_guest_audio import build as build_audio

# Frame rates the host stream sustains per size; larger sizes are offered for stills at lower rates.
CONFIG='''<?xml version="1.0" encoding="utf-8"?>
<ExternalCamera>
    <Provider>
        <ignore></ignore>
    </Provider>
    <Device>
        <MaxJpegBufferSize bytes="8388608"/>
        <NumVideoBuffers count="4"/>
        <NumStillBuffers count="2"/>
        <FpsList>
            <Limit width="640" height="480" fpsBound="30.0"/>
            <Limit width="1280" height="720" fpsBound="30.0"/>
            <Limit width="1920" height="1080" fpsBound="15.0"/>
            <Limit width="4096" height="3072" fpsBound="10.0"/>
        </FpsList>
    </Device>
</ExternalCamera>
'''
SERVICE='''
service pocket-cameraserver /system/bin/cameraserver
    class pocket-zygote
    user 1047
    group 1047
'''
PERMISSIONS='''<?xml version="1.0" encoding="utf-8"?>
<!-- Pocket guest: cameras streamed from the host. Assumes the host has a back camera; front is not claimed. -->
<permissions>
    <feature name="android.hardware.camera" />
    <feature name="android.hardware.camera.any" />
</permissions>
'''

def build(base_apk,ndk,output):
    output.mkdir(parents=True,exist_ok=True)
    payload=dict(build_audio(base_apk,ndk,output/'guest-audio'))
    api=ROOT/'out/api28';av=api/'frameworks-av';native=api/'frameworks-native'
    libs=output/'guest-audio/guest-media/lib64'
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    def run(argv,**kw):subprocess.run(list(map(str,argv)),check=True,**kw)
    # Patched copy of the AOSP camera HAL sources.
    source=output/'camera-src';shutil.rmtree(source,ignore_errors=True)
    hidl=api/'hardware-interfaces'
    for part in ('camera/device/3.4/default','camera/device/3.3/default','camera/device/3.2/default','camera/common/1.0/default'):
        shutil.copytree(hidl/part,source/part)
    run(['patch','-p1','--forward','-i',ROOT/'runtime/camera/external-camera-pocket.patch'],cwd=source)
    generated=output/'generated';generated.mkdir(exist_ok=True)
    gen=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/out/host/linux-x86/bin/hidl-gen'
    for package in ('android.hidl.base@1.0','android.hidl.manager@1.0','android.hidl.memory@1.0',
                    'android.hardware.graphics.common@1.0','android.hardware.graphics.common@1.1',
                    'android.hardware.graphics.mapper@2.0',
                    'android.hardware.camera.common@1.0','android.hardware.camera.device@1.0',
                    'android.hardware.camera.device@3.2','android.hardware.camera.device@3.3',
                    'android.hardware.camera.device@3.4','android.hardware.camera.provider@2.4'):
        run([gen,'-o',generated,'-Lc++-headers','-randroid.hidl:'+str(api/'libhidl/transport'),
             '-randroid.hardware:'+str(hidl),package])
    camera=source/'camera'
    includes=[generated,ROOT/'runtime/camera',camera/'common/1.0/default/include',
              camera/'device/3.4/default/include/ext_device_v3_4_impl',camera/'device/3.4/default/include',
              camera/'device/3.4/default',camera/'device/3.3/default',camera/'device/3.3/default/include',
              camera/'device/3.2/default',camera/'device/3.2/default/include',
              api/'libyuv/files/include',api/'libjpeg-turbo',api/'tinyxml2',api/'libexif',api/'libfmq/include',
              api/'system-media/camera/include',api/'system-media/private/camera/include',
              api/'libhidl/base/include',api/'libhidl/transport/include',api/'libhwbinder/include',
              native/'libs/ui/include',native/'include',ROOT/'out/composer-headers/include']
    includes+=[api/'system-core'/p/'include' for p in ('libutils','libcutils','liblog','libsystem','base','libsync')]
    common=['-O2','-fPIC','-nostdinc++','-isystem',api/'libcxx/include',*['-I'+str(p) for p in includes]]
    clang=llvm/'aarch64-linux-android28-clang++'
    objects=output/'camera-objects';objects.mkdir(exist_ok=True)
    def compile_one(item):
        path,flags=item
        target=objects/(str(path.relative_to(path.parents[3] if 'libyuv' in str(path) else ROOT.parent)).replace('/','_')+'.o')
        run([clang,'-std=gnu++17',*common,*flags,'-c',path,'-o',target])
        return target
    # libyuv: built statically, with its MJPEG path (the HAL still links it).
    yuv=api/'libyuv/files/source'
    yuv_sources=[yuv/n for n in ('compare.cc','compare_common.cc','compare_gcc.cc','compare_neon.cc','compare_neon64.cc',
        'convert.cc','convert_argb.cc','convert_from.cc','convert_from_argb.cc','convert_to_argb.cc','convert_to_i420.cc',
        'cpu_id.cc','planar_functions.cc','rotate.cc','rotate_any.cc','rotate_argb.cc','rotate_common.cc','rotate_gcc.cc',
        'rotate_neon.cc','rotate_neon64.cc','row_any.cc','row_common.cc','row_gcc.cc','row_neon.cc','row_neon64.cc',
        'scale.cc','scale_any.cc','scale_argb.cc','scale_common.cc','scale_gcc.cc','scale_neon.cc','scale_neon64.cc',
        'video_common.cc','convert_jpeg.cc','mjpeg_decoder.cc','mjpeg_validate.cc')]
    hal_flags=['-Wall','-Wno-unused-parameter','-Wno-unused-private-field','-Wno-deprecated-declarations',
               '-Wno-unused-variable','-Wno-format','-Wno-missing-field-initializers','-Wno-unused-lambda-capture',
               '-Wno-c99-designator','-Wno-reorder-init-list','-Wno-implicit-const-int-float-conversion','-fno-rtti',
               '-include',ROOT/'runtime/direct/hidl_codegen_compat.h']
    hal_sources=[camera/'device/3.4/default'/n for n in ('ExternalCameraDevice.cpp','ExternalCameraDeviceSession.cpp','ExternalCameraUtils.cpp')]
    hal_sources+=[camera/'device/3.2/default/convert.cpp',camera/'device/3.3/default/convert.cpp']
    hal_sources+=[camera/'common/1.0/default'/n for n in ('CameraMetadata.cpp','HandleImporter.cpp','Exif.cpp','VendorTagDescriptor.cpp')]
    hal_sources+=[ROOT/'runtime/camera/host_camera.cpp',ROOT/'runtime/camera/pocket_camera_provider.cpp']
    work=[(p,['-fexceptions','-DHAVE_JPEG','-Wno-unused-parameter']) for p in yuv_sources]+[(p,hal_flags) for p in hal_sources]
    with ThreadPoolExecutor(max_workers=8) as pool: built=list(pool.map(compile_one,work))
    provider=output/'libpocket_camera_api28.so'
    run([clang,'-shared','-nostdlib++',*built,'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
         *['-l:'+n for n in ('android.hardware.camera.device@3.2.so','android.hardware.camera.device@3.3.so',
           'android.hardware.camera.device@3.4.so','android.hardware.camera.provider@2.4.so',
           'android.hardware.camera.common@1.0.so','android.hardware.graphics.mapper@2.0.so')],
         *['-l'+n for n in ('hidlbase','hidltransport','hwbinder','utils','cutils','log','hardware','camera_metadata',
           'fmq','sync','jpeg','exif','tinyxml2','c++')],
         '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-Wl,-soname,libpocket_camera_api28.so','-o',provider])
    payload['system/lib64/libpocket_camera_api28.so']=str(provider.resolve())
    print('BUILT libpocket_camera_api28.so',flush=True)
    # cameraserver: the stock 64-bit libcameraservice, with C++ binder headers generated from the pinned AIDL.
    aidl=output/'aidl';shutil.rmtree(aidl,ignore_errors=True);(aidl/'cpp').mkdir(parents=True)
    tool=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/out/host/linux-x86/bin/aidl-cpp'
    # The native libcamera_client_aidl list from the pinned Android.bp; ICamera and ICameraClient are hand-written there.
    for name in ('ICameraService','ICameraServiceListener','ICameraServiceProxy','camera2/ICameraDeviceCallbacks','camera2/ICameraDeviceUser'):
        path=av/'camera/aidl/android/hardware'/(name+'.aidl')
        run([tool,'-I'+str(av/'camera/aidl'),'-I'+str(native/'aidl/gui'),path,aidl/'include',aidl/'cpp'/(path.stem+'.cpp')])
    for header in (aidl/'include').rglob('*.h'):
        text=header.read_text()
        # Parcelables named with cpp_header have no generated header, only an #error placeholder.
        if '#error' in text:header.unlink();continue
        # This aidl-cpp is newer than Android 9: drop its delegator helpers, which need a newer libbinder
        # and which the platform library never uses.
        text=text.replace('#include <binder/Delegate.h>\n','')
        text=re.sub(r'class LIBBINDER_EXPORTED (\w+Delegator) : .*?\n};  // class \1\n','',text,flags=re.S)
        header.write_text(text)
    server=output/'cameraserver'
    service=av/'services/camera/libcameraservice'
    run([clang,'-std=gnu++17','-fPIE','-pie','-O2','-nostdinc++','-isystem',api/'libcxx/include','-nostdlib++','-fno-rtti',
         '-DLIBBINDER_EXPORTED=','-I'+str(ROOT/'runtime/direct'),
         '-Wno-deprecated-declarations','-Wno-inconsistent-missing-override',
         *['-I'+str(p) for p in (aidl/'include',service,av/'camera/include',av/'camera/include/camera',av/'include',
           av/'media/libmedia/include',av/'media/utils/include',native/'libs/binder/include',native/'libs/gui/include',
           native/'libs/ui/include',native/'libs/nativewindow/include',native/'libs/arect/include',native/'libs/nativebase/include',
           native/'headers/media_plugin',
           api/'headers',api/'libhidl/transport/token/1.0/utils/include',output/'guest-audio/guest-media/generated')],
         *['-I'+str(p) for p in includes],ROOT/'runtime/camera/pocket_cameraserver.cpp',
         '-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lcameraservice','-lbinder','-lutils','-lhidltransport','-lhidlbase',
         '-llog','-lc++','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',server])
    assert reserve_interpreter(server)
    payload['system/bin/cameraserver']=str(server.resolve())
    print('BUILT cameraserver',flush=True)
    # Extend the init script the audio build already rewrote.
    rc_path=Path(payload['system/etc/init/init.syscall.rc']);rc=rc_path.read_text()
    if '\nservice pocket-cameraserver ' not in rc: rc+=SERVICE
    target=output/'init.camera.rc';target.write_text(rc);payload['system/etc/init/init.syscall.rc']=str(target.resolve())
    # gralloc.pocket: camera buffer formats in front of the reference gralloc.redroid; selected by build.prop.
    gralloc=output/'gralloc.pocket.so'
    run([llvm/'aarch64-linux-android28-clang','-std=c11','-O2','-fPIC','-shared','-Wall','-Wextra','-Werror',
         '-I'+str(ROOT/'out/composer-headers/include'),'-I'+str(api/'system-core/libcutils/include'),
         '-I'+str(api/'system-core/libsystem/include'),ROOT/'runtime/direct/gralloc_pocket.c','-ldl',
         '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-Wl,-soname,gralloc.pocket.so','-o',gralloc])
    payload['vendor/lib64/hw/gralloc.pocket.so']=str(gralloc.resolve())
    with zipfile.ZipFile(base_apk) as apk:props=apk.read('assets/api28-rootfs/system/build.prop.payload').decode()
    assert '\nro.hardware.gralloc=redroid\n' in props or '\nro.hardware.gralloc=pocket\n' in props
    target=output/'build.prop';target.write_text(props.replace('\nro.hardware.gralloc=redroid\n','\nro.hardware.gralloc=pocket\n'))
    payload['system/build.prop']=str(target.resolve())
    for name,text in (('external_camera_config.xml',CONFIG),):
        target=output/name;target.write_text(text);payload['vendor/etc/'+name]=str(target.resolve())
    target=output/'pocket-camera.xml';target.write_text(PERMISSIONS);payload['system/etc/permissions/pocket-camera.xml']=str(target.resolve())
    (output/'payload.json').write_text(json.dumps(payload,indent=2)+'\n')
    return payload


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--out-dir',type=Path,required=True)
    a=p.parse_args();build(a.base_apk.resolve(),a.ndk.resolve(),a.out_dir.resolve())
