#!/usr/bin/env python3
"""Import an existing compatible host app through the actual Pocket picker."""
import io
import json
from pathlib import Path
import re
import shlex
import sys
import time
import xml.etree.ElementTree as ET
sys.path.insert(0,'/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
from pocketctl import request
c.require_virtual()
PACKAGE='dev.kreijstal.aosppocket.runtime'
config=json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())
def guest_request(op,argv=(),source=None):
    out,err=io.BytesIO(),io.BytesIO()
    rc=request(config,op,argv,source=source,stdout=out,stderr=err)
    if rc:raise RuntimeError((rc,err.getvalue().decode(errors='replace')))
    return out.getvalue()
def host_adb(*argv,timeout=40):
    result=c.adb('-s',c.serial(),*argv,timeout=timeout,check=False)
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    return result.stdout.strip()
def host_shell(command,privileged=False):
    return host_adb('shell','-T','su -c '+shlex.quote(command) if privileged else command)
assert json.loads(guest_request(1))['ready']

APP='net.sourceforge.opencamera'
assert host_shell('pm path '+APP).startswith('package:'), 'Install Open Camera on the outer VM first'
host_shell('am start -f 0x04000000 -n '+PACKAGE+'/.SettingsActivity',True)
def hierarchy():
    path='/sdcard/pocket-install-success.xml'
    host_adb('shell','uiautomator','dump',path,timeout=45)
    return ET.fromstring(host_adb('shell','cat',path))
def tap(node):
    assert node.get('enabled')=='true',node.attrib
    left,top,right,bottom=map(int,re.findall(r'\d+',node.get('bounds')))
    host_adb('shell','input','tap',str((left+right)//2),str((top+bottom)//2))
def node_with_text(value):
    return next(n for n in hierarchy().iter('node') if n.get('text','').lower()==value.lower())
tap(node_with_text('Install APK'))
search=next(n for n in hierarchy().iter('node') if n.get('class')=='android.widget.EditText' and n.get('package')==PACKAGE)
tap(search)
host_adb('shell','input','text','Open%sCamera')
for _ in range(10):
    entry=next((n for n in hierarchy().iter('node') if n.get('text')=='Open Camera' and n.get('class')=='android.widget.TextView'),None)
    if entry is not None:break
    time.sleep(1)
else:raise AssertionError('Open Camera not listed')
tap(entry)
preview=hierarchy()
assert APP in '\n'.join(n.get('text','') for n in preview.iter('node'))
button=next(n for n in preview.iter('node') if n.get('text')=='Install in Android')
assert button.get('enabled')=='true'
started=time.monotonic()
tap(button)
for _ in range(90):
    status=json.loads(guest_request(1))
    if status['status']=='Installed '+APP:break
    assert 'operation failed' not in status['status'],status
    time.sleep(1)
else:raise AssertionError(status)
print('Installed via host picker: '+APP,flush=True)
log=guest_request(2,['/system/bin/toybox','tail','-c','131072','/logs/manager-init.log']).decode(errors='replace')
launch=[line for line in log.splitlines() if 'tag=ActivityManager Start proc ' in line and ':'+APP+'/' in line]
assert launch, 'No guest activity process started'
pid=re.search(r'Start proc (\d+):',launch[-1]).group(1)
cmdline=guest_request(2,['/system/bin/toybox','cat','/proc/'+pid+'/cmdline']).decode().split('\0')[0]
assert cmdline==APP,cmdline
assert status['ready'] and status['running'],status
last=host_shell('cat /data/user/0/'+PACKAGE+'/shared_prefs/guest-last-app.xml',True)
assert APP in last,last
report={'command':'python3 /home/pi/test_install_success_on_pi.py','managerVersion':'0.31',
        'package':APP,'path':'Settings > Install APK > Device apps > Open Camera > Install in Android',
        'compatibleInstallEnabled':True,'status':status['status'],'launchLog':launch[-1],'liveProcess':cmdline,
        'lastAppSaved':True,'guestStillReady':True,'elapsedSeconds':round(time.monotonic()-started,1),'exitCode':0}
Path('/home/pi/pocket-install-success-validation.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: compatible host APK imported, installed, launched; last app saved and guest still ready',flush=True)
