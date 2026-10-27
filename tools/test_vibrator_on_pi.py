#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the guest Vibrator HAL on the dedicated Pi Android VM: guest VibratorService -> host vibrator.

Covers timed and cancelled vibrations, amplitude (one-shots and a waveform), predefined effects,
and the mapping from the guest's haptic intensity setting to effect strength.
"""
import argparse
import io
import json
from pathlib import Path
import shlex
import sys
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
    def save():(Path.home()/'vibrator-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    if args.install:
        state=json.loads(rpc(1))
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
    destination='/data/local/tmp/pocket-vibrator-probe.jar'
    c.adb('-s',c.serial(),'push',str(args.probe),destination,check=True,timeout=60)
    physical=ROOT+'/system-data'+destination[len('/data'):]
    root('cp '+destination+' '+physical+' && chmod 0644 '+physical)
    def probe(*extra):
        command='export CLASSPATH='+destination+'; exec /system/bin/pocket-app-process-api28 /system/bin VibratorProbe '+' '.join(extra)
        out,err=io.BytesIO(),io.BytesIO()
        code=request(config,2,['/system/bin/toybox','timeout','60','/system/bin/sh','-c',command],stdout=out,stderr=err)
        return code,out.getvalue().decode(),err.getvalue().decode()[-2000:]
    def markers():return rpc(2,['/system/bin/toybox','grep','-E','^POCKET_VIBRATOR','/logs/manager-init.log']).splitlines()
    before=len(markers())
    started=time.time()
    code,stdout,stderr=probe()
    result.update(exitCode=code,stdout=stdout,stderr=stderr)
    # Android 9 sets a prebaked effect's strength from the user's haptic intensity (1 low .. 3 high);
    # the probe sets it, clicks and restores it.
    strengths={intensity:probe('click',intensity)[:2] for intensity in ('1','3')}
    result['intensity_runs']={k:{'exitCode':v[0],'stdout':v[1]} for k,v in strengths.items()}
    all_markers=markers()
    result['guest_hal']=[l for l in all_markers if l.startswith('POCKET_VIBRATOR_HAL')][-1:]
    result['guest_markers']=all_markers[before:]
    result['guest_log']=rpc(2,['/system/bin/sh','-c','/system/bin/toybox grep -E "VibratorService" /logs/manager-init.log | /system/bin/toybox tail -n 20'])
    # Host evidence: vibrations the host VibratorManagerService accepted from the Manager, with their effect details.
    history=c.shell('dumpsys vibrator_manager').splitlines()
    result['host_history']=[history[i].strip()+' || '+(history[i+1].strip() if i+1<len(history) else '')
                            for i,line in enumerate(history) if PACKAGE in line][-24:]
    result['host_vibrator']=[line.strip() for line in history if 'apabilit' in line or 'rimitive' in line][:12]
    result['probe_started_epoch']=started
    effects=[l for l in result['guest_markers'] if l.startswith('POCKET_VIBRATOR_EFFECT')]
    ok=code==0 and 'VIBRATOR_PROBE has_vibrator=true' in stdout and 'VIBRATOR_PROBE_DONE' in stdout and \
        all(v[0]==0 for v in strengths.values()) and any('version=1.2' in l for l in result['guest_hal']) and \
        any('effect=0 strength=0 length_ms=' in l for l in effects) and any('effect=0 strength=2 length_ms=' in l for l in effects) and \
        not any('HOST_UNAVAILABLE' in l and 'call=' in l and 'status=404' not in l for l in result['guest_markers'])
    result['validation']='PASS' if ok and result['host_history'] else 'FAIL'
    save();print(json.dumps(result,indent=2))
    assert result['validation']=='PASS',result['validation']

if __name__=='__main__':main()
