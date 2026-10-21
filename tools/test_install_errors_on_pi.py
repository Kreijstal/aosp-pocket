#!/usr/bin/env python3
"""Exercise real Android 9 install failures and the host compatibility preview."""
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
base='/data/pocket-import/base.apk'
report={'command':'python3 /home/pi/test_install_errors_on_pi.py','managerVersion':'0.31','installFailures':{}}
def command(*argv):return guest_request(2,list(argv)).decode(errors='replace')
def upload(data):
    guest_request(2,['/system/bin/sh','-c','/system/bin/toybox cat > "$1"','upload',base],source=io.BytesIO(data))
try:
    for name,data,expected in [('missing',None,'missing or empty'),('malformed',b'not an apk\n','Cannot install this APK'),
            ('newerAndroid',Path('/home/pi/future.apk').read_bytes(),'Requires newer sdk')]:
        command('/system/bin/toybox','rm','-rf','/data/pocket-import')
        command('/system/bin/toybox','mkdir','-p','/data/pocket-import')
        if data is not None:upload(data)
        result=command('/system/bin/toybox','timeout','90','/data/local/tmp/pocket-syscall-control','--install').strip()
        print(name+': '+result,flush=True)
        assert result.startswith('Install failed: ') and expected in result,result
        report['installFailures'][name]=result
    report['controlAfterFailures']=command('/data/local/tmp/pocket-syscall-control','dev.kreijstal.pocketsyscalltest').strip()
    assert 'launched' in report['controlAfterFailures']
finally:command('/system/bin/toybox','rm','-rf','/data/pocket-import')
# The outer VM can install API29. Its real package metadata drives the picker;
# root is used only to open our non-exported host Activity for UI automation.
host_shell('am start -n '+PACKAGE+'/.AppImportActivity --ei guest_sdk 28',True)
def hierarchy():
    path='/sdcard/pocket-install-errors.xml'
    host_adb('shell','uiautomator','dump',path,timeout=45)
    return ET.fromstring(host_adb('shell','cat',path))
def tap(node):
    left,top,right,bottom=map(int,re.findall(r'\d+',node.get('bounds')))
    host_adb('shell','input','tap',str((left+right)//2),str((top+bottom)//2))
ui=hierarchy()
search=next(n for n in ui.iter('node') if n.get('class')=='android.widget.EditText' and n.get('package')==PACKAGE)
tap(search)
host_adb('shell','input','text','Pocket%sAndroid%s10')
for _ in range(10):
    ui=hierarchy()
    entry=next((n for n in ui.iter('node') if n.get('text')=='Pocket Android 10 install test'),None)
    if entry is not None:break
    time.sleep(1)
else:raise AssertionError('Future-Android fixture not listed')
tap(entry)
ui=hierarchy()
text='\n'.join(n.get('text','') for n in ui.iter('node'))
expected='Requires Android 10 or later. This guest runs Android 9.'
assert expected in text,text
button=next(n for n in ui.iter('node') if n.get('text')=='Needs a newer Android version')
assert button.get('enabled')=='false',button.attrib
report.update(exitCode=0,previewMessage=expected,installButtonEnabled=False,controlSurvived=True)
Path('/home/pi/pocket-install-errors-validation.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: missing/malformed/newer-Android errors returned; control remains usable; incompatible preview blocks install')
host_adb('shell','input','keyevent','4')
host_adb('shell','input','keyevent','4')
