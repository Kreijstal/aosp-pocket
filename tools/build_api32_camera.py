#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the 12L host camera provider, libpocket_camera_api32.so.

cameraserver (stock 12L) lists camera provider "legacy/0" through Pocket's HIDL manager and loads this
library in process (runtime/api32/hidl_services.cpp). It is 12L's external camera HAL (device@3.4),
edited by runtime/camera/api32/port_*.py to stream I420 frames from the host cameras through the host
broker instead of V4L2, plus the provider in runtime/camera/api32/pocket_camera_provider32.cpp.
It links against the 12L image's own libraries (libyuv, libjpeg, libexif, libtinyxml2, ...).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess
import sys
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_hidl

PARTS = ('camera/device/3.4/default', 'camera/device/3.3/default', 'camera/device/3.2/default',
         'camera/common/1.0/default')
PACKAGES = ('android.hidl.base@1.0', 'android.hidl.manager@1.0', 'android.hidl.memory@1.0',
            'android.hidl.safe_union@1.0',
            'android.hardware.graphics.common@1.0', 'android.hardware.graphics.common@1.1',
            'android.hardware.graphics.common@1.2', 'android.hardware.graphics.mapper@2.0',
            'android.hardware.graphics.mapper@2.1', 'android.hardware.graphics.mapper@3.0',
            'android.hardware.graphics.mapper@4.0',
            'android.hardware.camera.common@1.0', 'android.hardware.camera.device@1.0',
            'android.hardware.camera.device@3.2', 'android.hardware.camera.device@3.3',
            'android.hardware.camera.device@3.4', 'android.hardware.camera.provider@2.4')


def port_sources(out):
    source = out / 'src'
    shutil.rmtree(source, ignore_errors=True)
    for part in PARTS:
        shutil.copytree(CACHE / 'hardware-interfaces' / part, source / part)
    tools = ROOT / 'runtime/camera/api32'
    for script in ('port_device.py', 'port_session.py', 'port_common.py'):
        subprocess.run([sys.executable, str(tools / script), str(source)], check=True)
    return source / 'camera'


def build_camera(ndk):
    prepare_sources(['libcxx', 'system-core', 'system-logging', 'libbase', 'libhardware', 'hardware-interfaces',
                     'libhidl', 'libhwbinder', 'libfmq', 'system-media', 'frameworks-native',
                     'libyuv', 'libjpeg-turbo', 'libexif', 'tinyxml2'])
    reference = prepare()
    out = CACHE / 'camera'
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    for package in PACKAGES:
        subprocess.run([str(prepare_hidl()), '-o', str(generated), '-Lc++-headers',
                        '-randroid.hidl:' + str(CACHE / 'libhidl/transport'),
                        '-randroid.hardware:' + str(CACHE / 'hardware-interfaces'), package], check=True)
    camera = port_sources(out)
    includes = [generated, ROOT / 'runtime/camera', camera / 'common/1.0/default/include',
                camera / 'device/3.4/default/include/ext_device_v3_4_impl', camera / 'device/3.4/default/include',
                camera / 'device/3.4/default', camera / 'device/3.3/default', camera / 'device/3.3/default/include',
                camera / 'device/3.2/default', camera / 'device/3.2/default/include',
                CACHE / 'libyuv/files/include', CACHE / 'libjpeg-turbo', CACHE / 'libexif', CACHE / 'tinyxml2',
                CACHE / 'system-media/camera/include', CACHE / 'system-media/private/camera/include',
                CACHE / 'libhardware/include', CACHE / 'libbase/include', CACHE / 'system-logging/liblog/include',
                CACHE / 'libhidl/base/include', CACHE / 'libhidl/transport/include', CACHE / 'libhwbinder/include',
                CACHE / 'libfmq/include', CACHE / 'libfmq/base',
                CACHE / 'frameworks-native/libs/binder/include', CACHE / 'frameworks-native/libs/nativebase/include',
                CACHE / 'frameworks-native/libs/ui/include', CACHE / 'frameworks-native/include']
    includes += [CACHE / 'system-core' / name / 'include' for name in ('libutils', 'libcutils', 'libsystem', 'libsync')]
    llvm = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    clang = llvm / 'aarch64-linux-android32-clang++'
    flags = ['-DPOCKET_GUEST_API=32', '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-nostdinc++',
             '-isystem', CACHE / 'libcxx/include', '-std=gnu++17', '-O2', '-fPIC', '-fno-rtti',
             '-Wall', '-Wno-unused-parameter', '-Wno-unused-private-field', '-Wno-deprecated-declarations',
             '-Wno-unused-variable', '-Wno-unused-function', '-Wno-format', '-Wno-missing-field-initializers',
             '-Wno-unused-lambda-capture', '-Wno-c99-designator', '-Wno-reorder-init-list',
             '-Wno-implicit-const-int-float-conversion', '-Wno-inconsistent-missing-override', '-DHAVE_JPEG',
             *['-I' + str(path) for path in includes]]
    sources = [camera / 'device/3.4/default' / name for name in
               ('ExternalCameraDevice.cpp', 'ExternalCameraDeviceSession.cpp', 'ExternalCameraUtils.cpp')]
    sources += [camera / 'device/3.2/default/convert.cpp', camera / 'device/3.3/default/convert.cpp']
    sources += [camera / 'common/1.0/default' / name for name in
                ('CameraMetadata.cpp', 'HandleImporter.cpp', 'Exif.cpp', 'VendorTagDescriptor.cpp')]
    sources += [ROOT / 'runtime/camera/host_camera.cpp', ROOT / 'runtime/camera/api32/pocket_camera_provider32.cpp']
    objects = out / 'objects'
    shutil.rmtree(objects, ignore_errors=True)
    objects.mkdir()

    def compile_one(path):
        target = objects / (path.parent.name + '_' + path.stem + '.o')
        if path.parent.parent.name in ('3.2', '3.3', '3.4'):
            target = objects / (path.parent.parent.name + '_' + path.stem + '.o')
        subprocess.run([str(clang), *map(str, flags), '-c', str(path), '-o', str(target)], check=True)
        return target
    with ThreadPoolExecutor(max_workers=8) as pool:
        built = list(pool.map(compile_one, sources))
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    provider = out / 'libpocket_camera_api32.so'
    subprocess.run([str(clang), '-shared', '-nostdlib++', *map(str, built), str(bionic / 'libc.so'),
                    '-L' + str(libs), '-L' + str(bionic), '-Wl,-rpath-link,' + str(libs), '-Wl,-rpath-link,' + str(bionic),
                    *['-l:' + package + '.so' for package in PACKAGES if package.startswith('android.hardware.')],
                    *['-l' + name for name in ('hidlbase', 'utils', 'cutils', 'log', 'hardware', 'camera_metadata',
                                               'fmq', 'sync', 'jpeg', 'exif', 'tinyxml2', 'yuv', 'base', 'c++')],
                    '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384', '-Wl,-soname,libpocket_camera_api32.so',
                    '-o', str(provider)], check=True)
    print('API32_CAMERA_BUILD_PASS', provider, flush=True)
    return provider


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    build_camera(parser.parse_args().ndk.resolve())
