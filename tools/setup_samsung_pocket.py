#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Copy and run Samsung Pocket's microG/password setup in one command.

Uses the existing private Termux owner-console pairing and saved guest password.
The phone job survives an SSH disconnect. Run this command again to follow an
already-running job. Use --password-file only to supply a private password file.
"""
import argparse
import getpass
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

REPO=Path(__file__).resolve().parents[1]
REMOTE='.config/pocketctl/microg-install'
SCOPE='Pocket Android 9 guest on Samsung SM-G780G'


def sha256(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def validate_bundle(bundle):
    spec=importlib.util.spec_from_file_location('provision',REPO/'tools/provision_microg_on_termux.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    names=set(module.EXPECTED)|{'guest-lock-probe.jar','microg-owner.jar','microg-api-probe.apk','install-manifest.json'}
    for name,expected in module.EXPECTED.items():
        if sha256(bundle/name)!=expected:raise ValueError('Official APK checksum mismatch: '+name)
    rows=json.loads((bundle/'install-manifest.json').read_text())
    expected_apks={
        'com.google.android.gms-252432034.apk':'system/priv-app/PocketGmsCore/GmsCore.apk',
        'com.android.vending-84022634.apk':'system/priv-app/PocketMicrogCompanion/Companion.apk',
        'GsfProxy-0.1.0.apk':'system/app/PocketGsfProxy/GsfProxy.apk',
    }
    targets=set()
    for row in rows:
        name,target=row['source'],row['target']
        if Path(name).name!=name or target in targets:raise ValueError('Invalid or duplicate manifest entry')
        targets.add(target);names.add(name)
        if name in expected_apks:
            if target!=expected_apks[name]:raise ValueError('Unexpected APK destination')
        else:
            if Path(target).parent.as_posix()!='system/priv-app/PocketGmsCore/lib/arm64':
                raise ValueError('Unexpected native library destination')
            with zipfile.ZipFile(bundle/'com.google.android.gms-252432034.apk') as apk:
                with apk.open('lib/arm64-v8a/'+Path(target).name) as lib:
                    if hashlib.file_digest(lib,'sha256').hexdigest()!=sha256(bundle/name):
                        raise ValueError('Native library differs from official APK: '+name)
    if not set(expected_apks.values())<=targets:raise ValueError('Manifest is missing an APK')
    with zipfile.ZipFile(bundle/'com.google.android.gms-252432034.apk') as apk:
        libs={n for n in apk.namelist() if n.startswith('lib/arm64-v8a/') and n.endswith('.so')}
    installed={t.replace('system/priv-app/PocketGmsCore/lib/arm64/','lib/arm64-v8a/') for t in targets if t.endswith('.so')}
    if libs!=installed:raise ValueError('Manifest does not include every ARM64 library')
    for name in ['guest-lock-probe.jar','microg-owner.jar','microg-api-probe.apk']:
        with zipfile.ZipFile(bundle/name) as archive:
            if 'classes.dex' not in archive.namelist():raise ValueError('Missing executable DEX: '+name)
    return {name:sha256(bundle/name) for name in sorted(names)}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',default='192.168.188.157',help='Samsung SSH host or alias')
    p.add_argument('--port',type=int,default=8022)
    p.add_argument('--user',default='u0_a401')
    known=REPO/'out/samsung-delivery-0.70/known_hosts'
    p.add_argument('--known-hosts',type=Path,default=known if known.is_file() else Path.home()/'.ssh/known_hosts')
    p.add_argument('--bundle',type=Path,default=REPO/'out/samsung-microg-lock/transfer')
    p.add_argument('--password-file',type=Path,help='Private file containing the guest password; otherwise reuse the phone credential')
    p.add_argument('--timeout',type=int,default=900,help='Hard runtime limit for the phone job, in seconds')
    p.add_argument('--check',action='store_true',help='Validate the complete local payload without connecting')
    p.add_argument('--rerun',action='store_true',help='Run again even if the previous phone job passed')
    args=p.parse_args()
    if args.timeout<30:p.error('--timeout must be at least 30 seconds')
    hashes=validate_bundle(args.bundle)
    print('Local payload verified: official APKs, matching ARM64 libraries and executable probes.',flush=True)
    if args.check:return 0
    for tool in ['ssh','rsync']:
        if not shutil.which(tool):raise RuntimeError('Required host command missing: '+tool)
    ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=5','-o','ServerAliveInterval=3',
         '-o','ServerAliveCountMax=2','-o','ControlPath=none','-o','StrictHostKeyChecking=yes',
         '-o','UserKnownHostsFile='+str(args.known_hosts.resolve()),'-p',str(args.port),'-l',args.user]
    # HostName overrides SSH aliases consistently in both ssh and rsync.
    ssh+=['-o','HostName='+args.host]
    def remote(code,data=None,timeout=15):
        result=subprocess.run([*ssh,args.host,'python3 -c '+shlex.quote(code)],input=data,
                              capture_output=True,timeout=timeout)
        if result.returncode:raise RuntimeError(result.stderr.decode(errors='replace').strip() or 'SSH command failed')
        return result.stdout.decode()
    preflight=r'''
import json,os,signal,subprocess,time
from pathlib import Path
assert subprocess.check_output(['getprop','ro.product.model'],text=True).strip()=='SM-G780G','Wrong phone model'
b=Path.home()/'.config/pocketctl/microg-install';b.mkdir(parents=True,exist_ok=True);b.chmod(0o700)
assert (b.parent/'config.json').is_file(),'Enable and pair Pocket Termux administration first'
script=str(b/'provision.py')
def ours(pid):
 try:return script.encode() in Path('/proc',str(pid),'cmdline').read_bytes().split(b'\0')
 except OSError:return False
job={}
try:job=json.loads((b/'job.json').read_text())
except (OSError,ValueError):pass
active=bool(job.get('pid') and ours(job['pid']))
if not active:
 # Retire an interrupted legacy controller which predates job tracking.
 for proc in Path('/proc').iterdir():
  if proc.name.isdigit() and ours(int(proc.name)):
   try:
    os.kill(int(proc.name),signal.SIGTERM);time.sleep(.2)
    if ours(int(proc.name)):os.kill(int(proc.name),signal.SIGKILL)
   except ProcessLookupError:pass
report={}
try:report=json.loads((b/'provision-validation.json').read_text())
except (OSError,ValueError):pass
print(json.dumps({'active':active,'credential':(b.parent/'guest-lock-credential.json').is_file(),'report':report}))
'''
    print('Connecting to Samsung Termux...',flush=True)
    state=json.loads(remote(preflight))
    def save_report(report):
        dest=REPO/'out/samsung-microg-lock/provision-validation.json';dest.parent.mkdir(parents=True,exist_ok=True)
        dest.write_text(json.dumps(report,indent=2)+'\n');print('Report:',dest,flush=True)
    if not state['active'] and state.get('report',{}).get('validation')=='PASS' and not args.rerun and not args.password_file:
        save_report(state['report']);print('Previous phone setup passed; use --rerun to repeat it.');return 0
    if not state['active']:
        password=None
        if args.password_file:
            if args.password_file.stat().st_mode&0o077:raise ValueError('Password file must have mode 0600')
            password=args.password_file.read_text().rstrip('\r\n')
        elif not state['credential']:
            if not sys.stdin.isatty():raise RuntimeError('No saved guest credential; supply --password-file with a private password file')
            password=getpass.getpass('Guest lock-screen password: ')
        if password is not None:
            if len(password)<4 or '\n' in password or '\r' in password:raise ValueError('Password must contain at least four characters and no newline')
            credential=json.dumps({'scope':SCOPE,'password':password}).encode()
            remote("import os,sys;from pathlib import Path;p=Path.home()/'.config/pocketctl/guest-lock-credential.json';t=p.with_suffix('.part');f=os.open(t,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600);os.fchmod(f,0o600);f=os.fdopen(f,'wb');f.write(sys.stdin.buffer.read());f.close();os.replace(t,p)",credential)
        with tempfile.TemporaryDirectory(prefix='pocket-samsung-') as temporary:
            stage=Path(temporary)
            for name in hashes:
                try:os.link(args.bundle/name,stage/name)
                except OSError:shutil.copy2(args.bundle/name,stage/name)
            for source,name in [(REPO/'tools/provision_microg_on_termux.py','provision.py'),(REPO/'tools/pocketctl.py','pocketctl.py')]:
                shutil.copyfile(source,stage/name);hashes[name]=sha256(stage/name)
            (stage/'payload-sha256.json').write_text(json.dumps(hashes,sort_keys=True)+'\n')
            print('Copying complete setup bundle to Termux (resumes partial transfers)...',flush=True)
            subprocess.run(['rsync','-rL','--checksum','--partial','--timeout=20','--chmod=D700,F600',
                            '-e',shlex.join(ssh),str(stage)+'/',args.host+':'+REMOTE+'/'],check=True,timeout=180)
        launch=r'''
import json,os,subprocess,sys,time
from pathlib import Path
b=Path.home()/'.config/pocketctl/microg-install'
report=b/'provision-validation.json'
if report.exists():os.replace(report,b/'previous-validation.json')
with (b/'provision.log').open('wb') as log:
 os.chmod(b/'provision.log',0o600)
 p=subprocess.Popen([sys.executable,str(b/'provision.py'),'--timeout',str(TIMEOUT)],stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
job={'pid':p.pid,'started':time.time()}
fd=os.open(b/'job.json',os.O_CREAT|os.O_TRUNC|os.O_WRONLY,0o600)
with os.fdopen(fd,'w') as f:json.dump(job,f)
print(json.dumps(job))
'''.replace('TIMEOUT',str(args.timeout))
        print('Started phone job:',json.loads(remote(launch))['pid'],flush=True)
    else:print('Setup is already running on the phone; following its report.',flush=True)
    inspect=r'''
import json
from pathlib import Path
b=Path.home()/'.config/pocketctl/microg-install'
report={}
try:report=json.loads((b/'provision-validation.json').read_text())
except (OSError,ValueError):pass
log=(b/'provision.log').read_text(errors='replace') if (b/'provision.log').exists() else ''
alive=False
try:
 pid=json.loads((b/'job.json').read_text())['pid']
 alive=str(b/'provision.py').encode() in Path('/proc',str(pid),'cmdline').read_bytes().split(b'\0')
except (OSError,ValueError,KeyError):pass
print(json.dumps({'report':report,'log':log,'alive':alive}))
'''
    seen='';deadline=time.monotonic()+args.timeout+30
    try:
        while time.monotonic()<deadline:
            status=json.loads(remote(inspect));log=status['log']
            if log!=seen:print(log[len(seen):] if log.startswith(seen) else log,end='',flush=True);seen=log
            report=status['report']
            if report.get('validation') in ('PASS','FAIL'):
                save_report(report)
                return 0 if report['validation']=='PASS' else 1
            if not status['alive']:
                print('Phone setup exited before writing a final report; see the log above.',file=sys.stderr);return 1
            time.sleep(3)
    except (subprocess.TimeoutExpired,RuntimeError) as error:
        print('SSH disconnected; setup continues on the phone. Run this command again to retrieve its result.',file=sys.stderr)
        print(str(error),file=sys.stderr);return 3
    raise TimeoutError('No final report before monitoring deadline; phone job has its own hard runtime limit')


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as error:
        print('Setup failed:',error,file=sys.stderr);sys.exit(1)
