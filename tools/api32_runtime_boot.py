# SPDX-License-Identifier: GPL-3.0-only
"""Stage a normal API 32 service boot alongside the separate validator."""
from pathlib import Path
import shutil
import zipfile
from prepare_api32 import ROOT
from build_api32_media import SERVICES as MEDIA_SERVICES,add_services as add_media_services
from build_api32_adbd import CONFIG as ADBD_CONFIG

SERVICES=('pocket-registry','pocket-hardware','pocket-mediametrics','pocket-audio',
    'pocket-network','pocket-memory','mdnsd','pocket-mdns-control','pocket-dns',
    'pocket-apex','pocket-storage','pocket-keymaster','pocket-keystore',
    'pocket-gatekeeper-hal','gatekeeperd','pocket-installd','idmap2d','pocket-traced',
    'pocket-tombstoned','pocket-framework','pocket-cameraserver',
    'pocket-input-control','pocket-surfaceflinger')
CORE_COMPONENTS=('pocket-network','pocket-memory','pocket-mdns-control',
    'pocket-dns','pocket-apex','pocket-storage','pocket-hardware')
CONSOLIDATED_COMPONENTS=CORE_COMPONENTS+('pocket-gatekeeper-hal',)
CORE_SERVICES_LAYOUT=2
NORMAL_CORE_SERVICES=(set(SERVICES)-set(CONSOLIDATED_COMPONENTS)-{'pocket-input-control','pocket-mediametrics'})|set(MEDIA_SERVICES)|{'pocket-runtime-bootstrap','pocket-adbd'}

def shared_installd_frontend(config,boot,core):
    """Select the complete UID-0 installd component and its matching startup."""
    if b'POCKET_INSTALLD_FRONTEND_READY pid=' not in core:
        raise ValueError('Shared installd requires its matching core executable')
    binder='/system/lib64/libpocket_binder_api32.so'
    sections=[];current=[]
    for line in config.splitlines():
        if line and not line[0].isspace() and not line.startswith('#'):
            if current:sections.append(current)
            current=[]
        current.append(line)
    if current:sections.append(current)
    seen=set();result=[];shared=None
    flag='setenv POCKET_API32_INSTALLD_IN_CORE 1'
    for section in sections:
        first=next((line for line in section if line and not line.startswith('#')),'')
        words=first.split()
        if words[:1]!=['service'] or words[1] not in ['pocket-registry','pocket-installd']:
            if any('POCKET_API32_INSTALLD_IN_CORE' in line for line in section):
                raise ValueError('Installd selection outside the root core')
            result.extend(section);continue
        name=words[1]
        if name in seen:raise ValueError('Duplicate root service '+name)
        seen.add(name)
        identity=[line.strip() for line in section if line.strip().startswith(('user ','group '))]
        if identity!=['user 0','group 0']:
            raise ValueError('Unexpected root service identity '+name)
        preloads=[line.strip() for line in section if line.strip().startswith('setenv LD_PRELOAD ')]
        flags=[line.strip() for line in section if 'POCKET_API32_INSTALLD_IN_CORE' in line]
        if name=='pocket-installd':
            if first!='service pocket-installd /system/bin/installd' or flags or preloads!=[
                    'setenv LD_PRELOAD '+binder+':/system/lib64/libpocket_policy_api32.so']:
                raise ValueError('Unexpected standalone installd service')
            continue
        if first!='service pocket-registry /system/bin/pocket-core-api32' or preloads!=[
                'setenv LD_PRELOAD '+binder+':/system/lib64/libpocket_hidl_api32.so']:
            raise ValueError('Expected the root core with its scoped Binder/HIDL adapters')
        if flags not in [[],[flag]]:raise ValueError('Invalid shared installd selection')
        shared=bool(flags)
        if not shared:section.append('    '+flag)
        result.extend(section)
    if 'pocket-registry' not in seen or shared!=('pocket-installd' not in seen):
        raise ValueError('Mixed standalone/shared installd configuration')
    old='    start_service pocket-installd\n'
    new='    await_marker POCKET_INSTALLD_FRONTEND_READY\n'
    wait='    wait_service installd\n'
    start='    start_service pocket-registry\n'
    ready='    await_marker POCKET_SERVICEMANAGER_READY\n'
    if boot.count(old)!=(0 if shared else 1) or boot.count(new)!=(1 if shared else 0):
        raise ValueError('Mixed standalone/shared installd startup')
    if any(boot.count(item)!=1 for item in [start,ready,wait]):
        raise ValueError('Missing core or typed installd readiness')
    frontend=new if shared else old
    if not boot.index(start)<boot.index(ready)<boot.index(frontend)<boot.index(wait):
        raise ValueError('Unexpected installd startup order')
    if not shared:boot=boot.replace(old,new)
    return '\n'.join(result)+'\n',boot

def shared_gatekeeper_frontend(config,boot,crypto):
    """Pair a real three-component crypto binary with one frontend instance.

    Accept the historical fixture or our own output, so rebuilding an image
    cannot restore the standalone service or duplicate a preload.
    """
    if b'POCKET_GATEKEEPER_FRONTEND_READY pid=' not in crypto:
        raise ValueError('Shared frontend requires its matching crypto executable')
    hidl='/system/lib64/libpocket_hidl_api32.so'
    dual='/system/lib64/libpocket_binder_api32.so:'+hidl
    sections=[];current=[]
    for line in config.splitlines():
        if line and not line[0].isspace() and not line.startswith('#'):
            if current:sections.append(current)
            current=[]
        current.append(line)
    if current:sections.append(current)
    seen=set();result=[];shared=None
    for section in sections:
        first=next((line for line in section if line and not line.startswith('#')),'')
        words=first.split()
        if words[:1]!=['service'] or words[1] not in ['pocket-keymaster','gatekeeperd']:
            result.extend(section);continue
        name=words[1]
        if name in seen:raise ValueError('Duplicate credential service '+name)
        seen.add(name)
        identity=[line.strip() for line in section if line.strip().startswith(('user ','group '))]
        if identity!=['user 1000','group 1000']:
            raise ValueError('Unexpected credential service identity '+name)
        preloads=[line.strip() for line in section if line.strip().startswith('setenv LD_PRELOAD ')]
        if name=='gatekeeperd':
            if first!='service gatekeeperd /system/bin/gatekeeperd /data/misc/gatekeeper' or preloads!=['setenv LD_PRELOAD '+dual]:
                raise ValueError('Unexpected standalone credential service')
            continue
        if first!='service pocket-keymaster /system/bin/pocket-crypto-api32' or len(preloads)!=1:
            raise ValueError('Expected the existing shared software HAL host')
        if preloads[0] not in ['setenv LD_PRELOAD '+hidl,'setenv LD_PRELOAD '+dual]:
            raise ValueError('Unexpected crypto Binder preload')
        shared=preloads[0]=='setenv LD_PRELOAD '+dual
        result.extend('    setenv LD_PRELOAD '+dual if line.strip()==preloads[0] else line for line in section)
    if 'pocket-keymaster' not in seen or shared!=('gatekeeperd' not in seen):
        raise ValueError('Mixed standalone/shared credential configuration')
    old='    start_service gatekeeperd\n'
    new='    await_marker POCKET_GATEKEEPER_FRONTEND_READY\n'
    if boot.count(old)!=(0 if shared else 1) or boot.count(new)!=(1 if shared else 0):
        raise ValueError('Mixed standalone/shared credential startup')
    if boot.count('    start_service pocket-keymaster\n')!=1:
        raise ValueError('Missing crypto startup')
    if not shared:boot=boot.replace(old,new)
    return '\n'.join(result)+'\n',boot

def eager_camera_boot(boot):
    # Java ServiceManager.getService delegates to checkService in 12L. Camera2
    # therefore cannot activate the native getService lazy-start hook. Like
    # stock cameraserver (class main), start it alongside system_server. Its
    # UID/privacy observers need framework services, so readiness must wait
    # until after framework startup, rather than blocking that startup.
    framework='    start_service pocket-framework\n'
    ready='    await_marker API32_INPUT_CONTROL_READY\n'
    camera='    start_service pocket-cameraserver\n'
    wait='    wait_service media.camera\n'
    if boot.count(framework)!=1 or boot.count(ready)!=1 or boot.index(framework)>boot.index(ready):
        raise ValueError('Expected ordered framework startup and readiness')
    if 'start_service pocket-cameraserver' in boot or 'wait_service media.camera' in boot:
        if boot.count(camera+framework)!=1 or boot.count(wait+ready)!=1 or boot.count('start_service pocket-cameraserver')!=1 or boot.count('wait_service media.camera')!=1:
            raise ValueError('Unexpected camera startup sequence')
        return boot
    return boot.replace(framework,camera+framework).replace(ready,wait+ready)

def embedded_system_server_bridge(config):
    sections=[];current=[]
    for line in config.splitlines():
        if line and not line[0].isspace() and not line.startswith('#'):
            if current:sections.append(current)
            current=[]
        current.append(line)
    if current:sections.append(current)
    found=set();result=[]
    for section in sections:
        first=next((line for line in section if line and not line.startswith('#')),'')
        words=first.split()
        if words[:1]==['service'] and words[1] in ['pocket-input-control','pocket-framework']:
            name=words[1]
            if name in found:raise ValueError('Duplicate bridge service '+name)
            found.add(name)
            identity=[line.strip() for line in section if line.strip().startswith(('user ','group '))]
            if identity!=['user 0','group 0']:raise ValueError('Unexpected bridge entry credentials')
            if name=='pocket-input-control':
                if not first.endswith(' dev.kreijstal.aosppocket.guest.InputControl32') or '    socket pocket_control32 stream 0600 1000 1000' not in section:
                    raise ValueError('Unexpected standalone input bridge')
                continue
            if '--start-system-server' not in words or '--zygote' not in words:raise ValueError('Expected a system-server zygote service')
            if any('POCKET_API32_SYSTEM_SERVER_BRIDGE' in line for line in section):raise ValueError('Bridge already embedded')
            section.append('    setenv POCKET_API32_SYSTEM_SERVER_BRIDGE 1')
        result.extend(section)
    if found!={'pocket-input-control','pocket-framework'}:raise ValueError('Missing input/framework service')
    return '\n'.join(result)+'\n'

def supervised_bootstrap(config):
    if '\nservice pocket-runtime-bootstrap ' in '\n'+config:
        raise ValueError('Runtime bootstrap is already declared')
    return config+'''\non boot
    start pocket-runtime-bootstrap

service pocket-runtime-bootstrap /system/bin/sh /system/etc/pocket-runtime-boot.sh --services
    disabled
    oneshot
    user 0
    group 0
'''

def supervised_boot_script(legacy):
    start='await_marker API32_INIT_READY\n'
    finish='echo POCKET_RUNTIME_BOOT_READY'
    if legacy.count(start)!=1 or legacy.count(finish)!=1 or '--services' in legacy:
        raise ValueError('Expected one legacy runtime boot sequence')
    services=legacy[legacy.index(start):legacy.index(finish)]
    template=(ROOT/'runtime/api32/runtime-boot.sh').read_text()
    begin='    await_marker API32_INIT_READY\n'
    end='    builtin print -r -- "$POCKET_API32_RUN_ID" > /tmp/pocket-runtime-services-ready.new\n'
    if template.count(begin)!=1 or template.count(end)!=1:raise ValueError('Invalid supervised bootstrap template')
    return template[:template.index(begin)]+''.join('    '+line+'\n' for line in services.splitlines())+template[template.index(end):]

def consolidate_core(config,test_echo=False,metrics=False):
    sections=[];current=[]
    for line in config.splitlines():
        if line and not line[0].isspace() and not line.startswith('#'):
            if current:sections.append(current)
            current=[]
        current.append(line)
    if current:sections.append(current)
    expected={'pocket-registry':('0','0'),'pocket-keymaster':('1000','1000 1026'),
        'pocket-gatekeeper-hal':('1000','1000')}
    expected.update({name:('0','1000' if name=='pocket-mdns-control' else '0') for name in CORE_COMPONENTS})
    if metrics:expected['pocket-mediametrics']=('1013','1013')
    seen=set();result=[];found=False
    for section in sections:
        first=next((line for line in section if line and not line.startswith('#')),'')
        words=first.split()
        if words[:1]==['service'] and words[1] in expected:
            name=words[1]
            if name in seen:raise ValueError('Duplicate service '+name)
            seen.add(name)
            identity={p[0]:' '.join(p[1:]) for line in section if (p:=line.split()) and p[0] in ['user','group']}
            if (identity.get('user'),identity.get('group'))!=expected[name]:
                raise ValueError('Unexpected credentials for shared service '+name)
        if words[:1]==['service'] and (words[1] in CONSOLIDATED_COMPONENTS or metrics and words[1]=='pocket-mediametrics'):continue
        if words[:2]==['service','pocket-registry']:
            if found:raise ValueError('Duplicate core registry service')
            found=True
            target='pocket-core-test-api32' if test_echo else 'pocket-core-api32'
            section=[line if line!=first else 'service pocket-registry /system/bin/'+target for line in section]
            section=[line.replace('/system/lib64/libpocket_binder_api32.so',
                '/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so')
                if line.strip().startswith('setenv LD_PRELOAD ') else line for line in section]
            section += ['    socket lmkd seqpacket 0660 1000 1000','    socket mdns stream 0660 0 1000']
            if metrics:section.append('    setenv POCKET_API32_METRICS_IN_CORE 1')
        if words[:2]==['service','pocket-keymaster']:
            section=[('service pocket-keymaster /system/bin/pocket-crypto-api32' if line==first else
                '    group 1000' if line.strip().startswith('group ') else line) for line in section]
        result.extend(section)
    if not found:raise ValueError('No registry service to consolidate')
    if seen!=expected.keys():raise ValueError('Missing services required by the shared layout')
    return '\n'.join(result)+'\n'

def stage_runtime_boot(stage,core_services=False):
    if b'API32_INIT_SUBREAPER_READY kernel_child_ownership=1' not in (stage/'system/bin/pocket-init-api32').read_bytes():
        raise ValueError('Rebuild native init with kernel descendant ownership before staging normal startup')
    if not (stage/'.pocket-api32-fixture').is_file():
        raise ValueError('Runtime staging requires the isolated API 32 build root')
    # Reject an old --reuse-native fixture before enabling the matching hook.
    native=(stage/'system/lib64/libpocket_zygote_api32.so').read_bytes()
    if any(symbol not in native for symbol in [b'pocketSystemServerChild32',b'pocketSystemServerMicrog32']):
        raise ValueError('Rebuild the API32 zygote for the embedded bridge and microG support')
    if core_services and b'POCKET_CORE_METRICS_READY' not in (stage/'system/bin/pocket-core-api32').read_bytes():
        raise ValueError('Rebuild the API32 core for the shared stock media-metrics service')
    for name,method in [('zygote',b'startSystemServerBridge'),('tools',b'startInSystemServer'),('microg',b'MicrogSupport32')]:
        with zipfile.ZipFile(stage/('system/framework/pocket-'+name+'.jar')) as jar:
            if method not in jar.read('classes.dex'):raise ValueError('Rebuild the matching API32 '+name+' JAR')
    blocks={};active=None
    for line in (stage/'system/etc/init/pocket-api32.rc').read_text().splitlines():
        if line and not line[0].isspace() and not line.startswith('#'):
            active=line.split()[1] if line.startswith('service ') else None
            if active:
                if active in blocks:raise ValueError('Duplicate service '+active)
                blocks[active]=[]
        if active:blocks[active].append(line)
    if set(SERVICES)-blocks.keys():raise ValueError('Normal boot is missing required services')
    blocks['pocket-registry'][0]='service pocket-registry /system/bin/pocket-servicemanager-api32'
    config='on boot\n    setprop dalvik.vm.dex2oat64.enabled 1\n\n'
    config+='\n\n'.join('\n'.join(blocks[name]).rstrip() for name in SERVICES)+'\n'
    if core_services:config=consolidate_core(config,metrics=True)
    config=embedded_system_server_bridge(config)
    config=add_media_services(config)+ADBD_CONFIG
    config=supervised_bootstrap(config)
    (stage/'system/etc/init/pocket-runtime.rc').write_text(config)
    for name in ['env','boot','validate']:
        shutil.copyfile(ROOT/('runtime/api32/runtime-'+name+'.sh'),stage/('system/etc/pocket-runtime-'+name+'.sh'))
    boot=stage/'system/etc/pocket-runtime-boot.sh'
    boot.write_text(eager_camera_boot(boot.read_text()))
    if core_services:
        boot=stage/'system/etc/pocket-runtime-boot.sh';text=boot.read_text()
        marker='    await_marker POCKET_SERVICEMANAGER_READY\n'
        if text.count(marker)!=1:raise ValueError('Missing normal registry readiness gate')
        text=text.replace(marker,marker+'    await_marker POCKET_CORE_METRICS_READY\n')
        for name in CONSOLIDATED_COMPONENTS:text=text.replace('start_service '+name+'\n','')
        boot.write_text(text)
    (stage/'probe.sh').write_text('exec /system/bin/sh /system/etc/pocket-runtime-validate.sh\n')
