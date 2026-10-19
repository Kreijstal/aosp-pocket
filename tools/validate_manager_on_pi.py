#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Guarded VM integration test for the persistent direct Manager session.

Run on the Pi, e.g. python3 - --install < tools/validate_manager_on_pi.py.
This installs only the separate development package; the original API25
runtime must keep the same host/init/PRoot PIDs throughout the test.
"""
import argparse,json,re,shlex,sys,time
from pathlib import Path
import xml.etree.ElementTree as ET

PACKAGE='dev.kreijstal.aosppocket.managerdev'
CLASS='dev.kreijstal.aosppocket.runtime.'
ROOT='/data/user/0/'+PACKAGE+'/files/api28-rootfs'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--install',action='store_true')
    parser.add_argument('--boot-only',action='store_true')
    parser.add_argument('--reset-host-permissions',action='store_true',
                        help='Revoke development-app device permissions after a clean stop')
    parser.add_argument('--original-stopped',action='store_true',
                        help='Sequential test after an already stopped API25 guest; never stop it here')
    args=parser.parse_args()
    if args.reset_host_permissions and not args.install:parser.error('--reset-host-permissions requires --install')
    sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    c.require_virtual()
    def adb(*arguments,timeout=40):
        r=c.adb('-s',c.serial(),*arguments,check=False,timeout=timeout)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        return r.stdout.strip()
    def shell(command,root=False,timeout=40):
        return adb('shell','-T','su -c '+shlex.quote(command) if root else command,timeout=timeout)
    def original():
        rows=[line.split() for line in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
        host=next((r for r in rows if len(r)==4 and r[3]=='dev.kreijstal.aosppocket.runtime'),None)
        if host is None:return None
        proot=next((r for r in rows if len(r)==4 and r[2]==host[1] and r[3]=='libproot.so'),None)
        init=next((r for r in rows if proot and len(r)==4 and r[2]==proot[1] and r[3]=='init'),None)
        return [host,proot,init] if proot and init else None
    before=original()
    assert (before is None) == args.original_stopped,'Original API25 presence differs from the requested test mode'
    def launch(activity):
        print(shell('am start -f 0x10000000 -n '+PACKAGE+'/'+CLASS+activity),flush=True)
    def hierarchy():
        for attempt in range(5):
            path='/sdcard/pocket-manager-ui-'+str(time.time_ns())+'.xml'
            output=''
            try:
                output=shell('timeout 30 uiautomator dump '+path)
                return ET.fromstring(shell('cat '+path))
            except (RuntimeError,ET.ParseError):pass
            time.sleep(1)
        raise RuntimeError('UIAutomator could not capture a fresh hierarchy: '+output)
    def click(label):
        nodes=list(hierarchy().iter('node'))
        node=next((n for n in nodes if n.get('package')==PACKAGE and
                   n.get('text','').casefold().startswith(label.casefold()) and n.get('enabled')=='true'),None)
        if node is None and label in ('Float','Close window','Manager'):
            handle=next(n for n in nodes if n.get('package')==PACKAGE and n.get('content-desc')=='Android controls')
            l,t,r,b=map(int,re.findall(r'\d+',handle.get('bounds')))
            shell(f'input tap {(l+r)//2} {(t+b)//2}')
            node=next(n for n in hierarchy().iter('node') if n.get('package')==PACKAGE and n.get('text','').casefold().startswith(label.casefold()) and n.get('enabled')=='true')
        assert node is not None,'Missing control: '+label
        l,t,r,b=map(int,re.findall(r'\d+',node.get('bounds')))
        shell(f'input touchscreen -d 0 tap {(l+r)//2} {(t+b)//2}')
        time.sleep(1)
    if args.install:
        if shell('pm path '+PACKAGE):
            launch('MainActivity')
            tree=hierarchy()
            if any(n.get('package')==PACKAGE and n.get('text')=='Stop' and n.get('enabled')=='true'
                   for n in tree.iter('node')):
                click('Stop')
                deadline=time.monotonic()+30
                while time.monotonic()<deadline:
                    rows=[line.split() for line in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
                    host=next((row for row in rows if row[-1]==PACKAGE),None)
                    if host and not any(row[0]==host[0] and row[1]!=host[1] for row in rows):break
                    time.sleep(.5)
                else:raise RuntimeError('Refusing to update before the development guest stops cleanly')
        assert before==original(),'Original API25 guest changed before update'
        if args.reset_host_permissions:
            for permission in ['CAMERA','RECORD_AUDIO','READ_CONTACTS','READ_PHONE_STATE',
                               'ACCESS_FINE_LOCATION','ACCESS_COARSE_LOCATION']:
                shell('pm revoke '+PACKAGE+' android.permission.'+permission)
        print(adb('install','--streaming','-r','/home/pi/aosp-pocket-manager-dev.apk',timeout=360),flush=True)
    def log():return shell('if [ -f '+ROOT+'/logs/manager-init.log ]; then grep -E "POCKET_CONTROL|API28_INIT_STOPPED|FATAL EXCEPTION IN SYSTEM PROCESS" '+ROOT+'/logs/manager-init.log || true; fi',True)
    print(shell('am start -f 0x10000000 -n '+PACKAGE+'/'+CLASS+'MainActivity --ei guest_sdk 28 --ez start_guest true'),flush=True)
    deadline=time.monotonic()+480
    while time.monotonic()<deadline:
        text=log()
        if 'POCKET_CONTROL_READY' in text and 'API28_INIT_STOPPED' not in text:
            # Preparing the image can leave the previous boot's log in place.
            # Require a live framework under the current host UID as well.
            rows=[line.split() for line in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
            current_host=next((row for row in rows if row[-1]==PACKAGE),None)
            if current_host and any(row[0]==current_host[0] and row[-1]=='system_server' for row in rows):break
        if 'POCKET_CONTROL_FAILED' in text or 'FATAL EXCEPTION IN SYSTEM PROCESS' in text:
            raise RuntimeError(text)
        time.sleep(2)
    else:raise RuntimeError('Manager did not boot: '+text)
    measured=re.findall(r'POCKET_CONTROL_READY sdk=28 width=(\d+) height=(\d+) density=(\d+)',text)
    assert measured,'Guest did not report actual display size and density'
    guest_width,guest_height,guest_dpi=map(int,measured[-1])
    host_width,host_height=map(int,re.findall(r'(\d+)x(\d+)',shell('wm size'))[-1])
    host_dpi=int(re.findall(r'density:\s*(\d+)',shell('wm density'))[-1])
    assert (guest_width,guest_height,guest_dpi)==(min(host_width,host_height),max(host_width,host_height),host_dpi)
    time.sleep(3)
    if any(n.get('text','').startswith('Android is open in its own window.')
           for n in hierarchy().iter('node')):
        click('Android is open in its own window.')
        click('Close window')
        launch('MainActivity')
    assert before==original(),'Original API25 guest changed'
    processes=shell('ps -A -o UID,PID,PPID,NAME')
    host=next(line.split() for line in processes.splitlines() if line.endswith(PACKAGE))
    uid=host[0]
    guests=[line for line in processes.splitlines() if line.split()[0]==uid]
    assert not any('proot' in line for line in guests),'Android9 must use direct backend'
    server=next(line.split() for line in guests if line.endswith(' system_server'))
    status=shell('cat /proc/'+server[1]+'/status',True)
    assert re.search(r'^Uid:\s+'+uid+r'\s+'+uid,status,re.M),status
    assert re.search(r'^TracerPid:\s+0$',status,re.M),status
    assert re.search(r'^Seccomp:\s+2$',status,re.M),status
    assert re.search(r'^CapEff:\s+0+$',status,re.M),status
    domain=shell('cat /proc/'+server[1]+'/attr/current',True).rstrip('\0')
    assert 'untrusted_app' in domain,domain
    tree=hierarchy()
    assert any(n.get('class')=='android.view.SurfaceView' and n.get('package')==PACKAGE for n in tree.iter('node'))
    assert any(n.get('text')=='Install APK' and n.get('enabled')=='true'
               for n in tree.iter('node')),'Manager has not enabled the booted direct guest controls'
    shell('screencap -p /sdcard/pocket-manager9.png')
    adb('pull','/sdcard/pocket-manager9.png','/home/pi/pocket-manager9.png')
    result={'passed':True,'scope':'manager_android9_boot','host_uid':int(uid),'host_pid':int(host[1]),
            'system_server_pid':int(server[1]),'domain':domain,'original_api25_unchanged':True if before else None,'original_api25_pids':{'host':int(before[0][1]),'proot':int(before[1][1]),'init':int(before[2][1])} if before else None,
            'direct_backend':True,'boot_completed':True,'guest_display':{'width':guest_width,'height':guest_height,'density_dpi':guest_dpi},'tracer_pid':0,'seccomp':2,'effective_capabilities':0,'screenshot':'/home/pi/pocket-manager9.png'}
    Path('/home/pi/pocket-manager9-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2),flush=True)
    if args.boot_only:return
    def guest_log():
        return shell('tail -n 800 '+ROOT+'/logs/manager-init.log',True)
    def document_click(label):
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            nodes=list(hierarchy().iter('node'))
            node=next((n for n in nodes if n.get('package')=='com.android.documentsui' and
                       (n.get('text','').casefold().startswith(label.casefold()) or
                        n.get('content-desc','').casefold().startswith(label.casefold()))),None)
            if node is not None:break
            time.sleep(.5)
        else:raise RuntimeError('Document picker control not found: '+label)
        l,t,r,b=map(int,re.findall(r'\d+',node.get('bounds')))
        shell(f'input touchscreen -d 0 tap {(l+r)//2} {(t+b)//2}')
        time.sleep(1)
    adb('push','/home/pi/pocket-guest-test.apk','/sdcard/Download/pocket-test.apk')
    click('Install APK')
    document_click('Show roots')
    document_click('Downloads')
    document_click('pocket-test.apk')
    deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        text=guest_log()
        if 'POCKET_GUEST_BUTTON center=' in text:break
        time.sleep(1)
    else:raise RuntimeError('Guest APK installation/launch failed: '+text)
    assert 'DIRECT_FRAMEWORK_INSTALL_PASS package=dev.kreijstal.pocketguesttest' in text,text
    def tap_guest(expected):
        text=guest_log()
        x,y=map(int,re.findall(r'POCKET_GUEST_BUTTON center=([0-9]+),([0-9]+)',text)[-1])
        surfaces=[n for n in hierarchy().iter('node') if n.get('package')==PACKAGE and
                  n.get('class')=='android.view.SurfaceView']
        def area(node):
            l,t,r,b=map(int,re.findall(r'\d+',node.get('bounds')));return (r-l)*(b-t)
        node=max(surfaces,key=area)
        l,t,r,b=map(int,re.findall(r'\d+',node.get('bounds')))
        offset=int(shell('stat -c %s '+ROOT+'/logs/manager-init.log',True))
        shell(f'input touchscreen -d 0 tap {l+x*(r-l)//guest_width} {t+y*(b-t)//guest_height}')
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            new_text=shell('tail -c +'+str(offset+1)+' '+ROOT+'/logs/manager-init.log',True)
            if re.search(r'tag=PocketGuestTest tap='+str(expected)+r'\b',new_text):return
            time.sleep(.3)
        raise RuntimeError('Preview/window touch did not reach guest counter')
    tap_guest(1)
    shell('screencap -p /sdcard/pocket-manager9-tap.png')
    adb('pull','/sdcard/pocket-manager9-tap.png','/home/pi/pocket-manager9-tap.png')
    launch('GuestActivity')
    tap_guest(2)
    click('Manager')
    click('Close manager')
    assert any(row.split()[1]==server[1] for row in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:])
    launch('GuestActivity')
    tap_guest(3)
    click('Float')
    time.sleep(1)
    launch('MainActivity')
    tree=hierarchy()
    def displays(tree):
        return [n for n in tree.iter('node') if n.get('package')==PACKAGE and
                n.get('class')=='android.view.SurfaceView']
    assert any(n.get('text','').startswith('Android is open in its own window.')
               for n in tree.iter('node')),'Manager is mirroring the floating guest'
    # dump reports the active Manager window, not the separate PiP hierarchy.
    assert not displays(tree),'Manager still exposes its hidden guest surface'
    tasks=shell('dumpsys activity activities')
    assert any(PACKAGE+'.android' in line and 'mode=pinned' in line and 'visible=true' in line
               for line in tasks.splitlines()),'The separate guest PiP window is not visible'
    shell('screencap -p /sdcard/pocket-manager9-exclusive.png')
    adb('pull','/sdcard/pocket-manager9-exclusive.png','/home/pi/pocket-manager9-exclusive.png')
    # The placeholder returns to the existing Android task, then closing that
    # window hands the same guest back to Manager without restarting it.
    click('Android is open in its own window.')
    click('Close window')
    launch('MainActivity')
    tree=hierarchy()
    assert len(displays(tree))==1,'Manager did not regain its display'
    assert not any(n.get('text','').startswith('Android is open in its own window.')
                   for n in tree.iter('node')),'Manager still thinks Android owns the display'
    tap_guest(4)
    assert before==original(),'Original API25 guest changed after windows/input test'
    generations=shell("grep 'system_server=1' "+ROOT+'/logs/manager-init.log',True)
    assert len(set(re.findall(r'DIRECT_ZYGOTE_FORK pid=([1-9][0-9]*) system_server=1',generations)))==1,generations
    result['system_server_restarts_during_test']=0
    result['window_lifetime_verified']=True
    result['exclusive_display_verified']=True
    result['display_handback_verified']=True
    result['exclusive_display_screenshot']='/home/pi/pocket-manager9-exclusive.png'
    result['apk_install_verified']=True
    result['guest_taps']=4
    result['pip_verified']='Guest PiP=true' in shell('logcat -d --pid='+host[1]+' -s PocketWindows:I')
    assert result['pip_verified'],'Guest window did not enter PiP'
    click('Stop')
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if 'API28_INIT_STOPPED' in guest_log():break
        time.sleep(.5)
    else:raise RuntimeError('Init did not stop')
    assert 'remaining=0 failed=0' in guest_log(),guest_log()
    remaining=[line.split() for line in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:] if line.split()[0]==uid]
    assert len(remaining)==1 and remaining[0][3]==PACKAGE,remaining
    result['stop_reaped_all_guest_processes']=True
    assert 'dev.kreijstal.pocketguesttest' in shell('cat '+ROOT+'/system-data/system/packages.xml',True)
    click('Start')
    deadline=time.monotonic()+240
    while time.monotonic()<deadline:
        restarted=log()
        if 'POCKET_CONTROL_READY' in restarted and 'API28_INIT_STOPPED' not in guest_log():break
        time.sleep(1)
    else:raise RuntimeError('Persistent Android9 restart failed')
    assert 'FATAL EXCEPTION IN SYSTEM PROCESS' not in log(),log()
    assert before==original(),'Original API25 guest changed after restart'
    result['restart_verified']=True
    result['scope']='manager_android9_integration'
    Path('/home/pi/pocket-manager9-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2),flush=True)
    print('PASS: Android9 rootless direct boot, APK installation, four real display taps, exclusive PiP display and handback, window lifetime, stop/reap/restart and preserved API25 guest',flush=True)

if __name__=='__main__':main()
