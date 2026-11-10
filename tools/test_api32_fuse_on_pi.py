#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the isolated packaged12L libfuse transport probe on dedicated VM73."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import time
import uuid

PACKAGE='dev.kreijstal.aosppocket.api32fusetest'
ACTIVITY=PACKAGE+'/dev.kreijstal.aosppocket.runtime.FuseTransportActivity'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apk',type=Path,required=True)
    p.add_argument('--vm-root',type=Path,default=Path('/home/pi/pocket-api32-vm'))
    p.add_argument('--require-storage-mode',action='store_true')
    p.add_argument('--require-state-compat',action='store_true')
    p.add_argument('--require-peer-identity',action='store_true')
    p.add_argument('--require-concurrent-replies',action='store_true')
    p.add_argument('--require-lower-view',action='store_true')
    p.add_argument('--require-unix-datagram',action='store_true')
    p.add_argument('--require-namespace-rpc',action='store_true')
    p.add_argument('--require-directory-rpc',action='store_true')
    p.add_argument('--require-volume-session',action='store_true')
    p.add_argument('--require-received-rights',action='store_true',help='Require real Bionic timeout/EOF/truncation/FD-cleanup decoder checks')
    p.add_argument('--require-detach-retry',action='store_true',help='Require lost-detach reply recovery with the callback broker retained')
    p.add_argument('--require-live-proc-discovery',action='store_true',help='Require host evidence that retirement enumerated live proc entries')
    p.add_argument('--require-path-handles',action='store_true')
    p.add_argument('--require-app-directories',action='store_true')
    p.add_argument('--require-lower-canonical',action='store_true')
    p.add_argument('--require-held-anchor',action='store_true')
    p.add_argument('--require-unlink-types',action='store_true')
    p.add_argument('--prior-report',type=Path,help='Explicit terminal native-namespace attempt to inspect before updating that isolated fixture APK')
    p.add_argument('--prior-holder-capability-report',type=Path,help='Completed no-guest pidfd capability check in the same isolated volume APK sandbox')
    p.add_argument('--resume-report',type=Path,help='Observe the exact already-completed run without installing or starting anything; write into a different work directory')
    args=p.parse_args();base=args.vm_root.resolve();work=args.apk.resolve().parent
    assert sum(bool(v) for v in [args.prior_report,args.prior_holder_capability_report,args.resume_report])<=1
    metadata=json.loads((work/'build.json').read_text())
    package=metadata.get('package',PACKAGE);activity=metadata.get('activity',ACTIVITY)
    external=metadata.get('externalProxy',False)
    namespace=metadata.get('namespaceRpc',False)
    volume=metadata.get('volumeSession',False);assert volume==args.require_volume_session
    assert not args.require_received_rights or volume
    assert not args.require_detach_retry or volume
    assert not args.require_live_proc_discovery or volume
    assert not args.require_path_handles or volume
    assert not (args.require_app_directories or args.require_lower_canonical or args.require_held_anchor or args.require_unlink_types) or volume
    assert metadata.get('directoryRpc',False)==args.require_directory_rpc
    assert not args.require_directory_rpc or args.require_namespace_rpc
    assert namespace==args.require_namespace_rpc and not (namespace and external)
    assert package==('dev.kreijstal.aosppocket.api32volumetest' if volume else 'dev.kreijstal.aosppocket.api32namespacerpctest' if namespace else 'dev.kreijstal.aosppocket.api32externaltest' if external else PACKAGE)
    assert activity==package+'/dev.kreijstal.aosppocket.runtime.'+('VolumeSessionActivity' if volume else 'ExternalProxyActivity' if external else 'FuseTransportActivity')
    assert not external or not (args.require_storage_mode or args.require_state_compat or args.require_peer_identity or args.require_concurrent_replies or args.require_lower_view or args.require_unix_datagram)
    assert not namespace or not (args.require_storage_mode or args.require_state_compat or args.require_peer_identity or args.require_concurrent_replies or args.require_lower_view or args.require_unix_datagram)
    assert work.is_relative_to(base) and (base/'.pocket-api32-test-vm').is_file()
    assert set(json.loads((base/'runtime/assembly/cuttlefish_config.json').read_text())['instances'])=={'73'}
    spec=importlib.util.spec_from_file_location('vm',base/'tools/cuttlefish_arm64.py')
    vm=importlib.util.module_from_spec(spec);spec.loader.exec_module(vm)
    assert vm.BASE.resolve()==base and vm.lock()['instance']==73;vm.require_virtual()
    prior=json.loads((base/'ce-metadata-manager-validation/ce-manager-report.json').read_text())
    assert prior['runId']=='d2cf3f83999442a3be7f61274766c641' and prior['strictPassed']
    def adb(*argv,timeout=40):
        r=vm.adb('-s',vm.serial(),*argv,check=False,timeout=timeout)
        if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-2000:])
        return r.stdout.strip()
    processes=adb('shell','ps -A -o PID,NAME,ARGS')
    assert not any(any(n in line for n in ['pocket-init-api32','pocket-core-api32','pocket-runtime-boot.sh','fuse-stock','system/bin/pocket-loader']) for line in processes.splitlines()),'Inspect the exact existing native owner before another test'
    installed=adb('shell','pm list packages --user 0 '+package).splitlines();attempt=None;resuming=None
    if args.resume_report:
        previous=args.resume_report.resolve();assert volume and previous.is_relative_to(base) and previous.parent!=work
        resuming=json.loads(previous.read_text());assert resuming['vmInstance']==73 and resuming['validation'] in ('PASS','FAIL')
        assert resuming['scope']==metadata['scope'] and resuming['apkSha256']==metadata['sha256']
        assert len(resuming['runId'])==32 and all(c in '0123456789abcdef' for c in resuming['runId'])
        assert 'package:'+package in installed
    elif args.prior_holder_capability_report:
        previous=args.prior_holder_capability_report.resolve();assert volume and previous.is_relative_to(base)
        attempt=json.loads(previous.read_text());assert attempt['validation']=='PASS'
        assert attempt['scope']=='Host pidfd open/self signal0 capability only; no guest started or process retired'
        assert attempt['selinuxBefore']==attempt['selinuxAfter']=='Enforcing'
        assert attempt['runId']==attempt['app']['runId'] and attempt['app']['validation']=='PASS'
        assert attempt['app']['pidfdOpen'] is True and attempt['app']['pidfdSignalZero'] is True and attempt['app']['guestStarted'] is False
        assert 'package:'+package in installed
        path=adb('shell','pm path '+package).splitlines();assert len(path)==1 and path[0].startswith('package:/data/app/')
        assert adb('shell','sha256sum '+shlex.quote(path[0][8:])).split()[0]==attempt['apkSha256']
    elif args.prior_report:
        previous=args.prior_report.resolve();assert (namespace or volume) and previous.is_relative_to(base)
        attempt=json.loads(previous.read_text());assert attempt['vmInstance']==73 and attempt['validation'] in ('PASS','FAIL')
        assert attempt['scope']==metadata['scope'] and attempt['runId']==attempt['app']['runId']
        if attempt['validation']=='PASS':assert attempt['app']['nativeExit']==0 and attempt['app']['validation']=='PASS' and attempt['volumeSessionNativeVerified' if volume else 'namespaceRpcNativeVerified']
        else:
            assert attempt['app']['validation']=='FAIL'
            assert attempt['app'].get('nativeExit',0)!=0 or attempt['app'].get('directoryLibcChild',{}).get('exit',0)!=0
        assert 'package:'+package in installed
        path=adb('shell','pm path '+package).splitlines();assert len(path)==1 and path[0].startswith('package:/data/app/')
        assert adb('shell','sha256sum '+shlex.quote(path[0][8:])).split()[0]==attempt['apkSha256']
    else:assert 'package:'+package not in installed,'Existing FUSE probe sandbox: inspect its exact completed or live run before reuse'
    digest=hashlib.file_digest(args.apk.open('rb'),'sha256').hexdigest();assert digest==metadata['sha256']
    assert metadata['assets']['system/lib64/libfuse.so']=='3e97c7bea3a4479ff5643a14ccd37c86f9682ba7a2e29b9dbe853f4da350dd54'
    report={'runId':resuming['runId'] if resuming else uuid.uuid4().hex,'vmInstance':73,'apkSha256':digest,'scope':metadata['scope']}
    if attempt:report['priorRunId']=attempt['runId'];report['guestDataReset']=False
    if resuming:report.update(observationOnly=True,resumedReport=str(previous),originalValidation=resuming['validation'],guestDataReset=False)
    settings=[('selinux','getenforce'),('phantomLimit','device_config get activity_manager max_phantom_processes'),('phantomMonitoring','settings get global settings_enable_monitor_phantom_procs')]
    for key,command in settings:
        current=adb('shell',command);report[key+'Before']=resuming[key+'Before'] if resuming else current
        assert current==report[key+'Before']
    assert report['selinuxBefore']=='Enforcing'
    if attempt and not resuming:
        adb('shell','am start -W -a android.intent.action.MAIN -c android.intent.category.HOME')
        adb('shell','am force-stop '+package)
    if not resuming:print(adb('install','-r','--no-incremental',str(args.apk),timeout=120),flush=True)
    assert 'DEBUGGABLE' not in adb('shell','dumpsys package '+package)
    installed=adb('shell','pm path '+package).splitlines();assert len(installed)==1 and installed[0].startswith('package:/data/app/')
    assert adb('shell','sha256sum '+shlex.quote(installed[0][8:])).split()[0]==digest
    try:
        if not resuming:print(adb('shell','am start -n '+activity+' --es probe_run_id '+report['runId']+(' --ez detach_retry true' if args.require_detach_retry else '')),flush=True)
        try:
            pid=adb('shell','pidof '+package)
            if pid.isdigit() and int(pid)>0:report['hostPid']=int(pid)
        except Exception as error:report['hostPidLookupError']=str(error)
        deadline=time.monotonic()+180;errors=0
        while time.monotonic()<deadline:
            try:text=adb('shell','dumpsys activity '+activity+' pocket-fuse32')
            except Exception:errors+=1;time.sleep(1);continue
            rows=[line.strip().split('=',1)[1] for line in text.splitlines() if line.strip().startswith('POCKET_FUSE32=')]
            state=json.loads(base64.b64decode(rows[-1],validate=True)) if rows else {}
            if state.get('runId')==report['runId'] and state.get('validation'):
                report['app']=state
                if not report.get('hostPid'):
                    try:
                        pid=adb('shell','pidof '+package)
                        if pid.isdigit() and int(pid)>0:report['hostPid']=int(pid)
                    except Exception:pass
                break
            time.sleep(1)
        else:raise TimeoutError('FUSE probe observation timed out; inspect exact native process before any restart')
        report['pollErrors']=errors
        for key,command in settings:
            report[key+'After']=adb('shell',command);assert report[key+'After']==report[key+'Before']
        assert state['validation']=='PASS',state.get('error','FUSE probe failed')
        assert state['hostUid']>=10000 and state['nativeExit']==0
        markers=['EXTERNAL_NAMESPACE_RPC_PASS','live_pid_start_tid=1','descriptor_proof=1','guessed_cap_denied=1','stale_cap_denied=1','partial_header_rights=1','excess_rights_closed=1','path_fd_attrs_agree=1','real_filesystem_mutations=1','readonly_truncate_denied=1','cross_process_denied=1','loader_dispatch_client=1','missing_relative_mkdir=1','cross_mount_rename_denied=1','bad_user_buffer=1','mask_not_changed=1','forced_provider_fd=1','lookup_refs_and_handles_balanced=1','fd_leaks=0'] if namespace else ['FUSE_PROXY32_PASS','shared_mmap=coherent','dup=coherent','errno=EACCES','blocked_sync_read=PASS','uid=19991'] if external else ['FUSE_STOCK32_PASS','protocol=7.31','wrong_uid=denied','loop_exit=0','fd_leaks=0','kernel_mounts=0']
        for marker in markers:
            if not volume:assert marker in state['log'],marker
        if volume:
            assert state['clientExit']==137 and state['holderPid']>0
            for marker in ['VOLUME_ADAPTER_ERRORS32_PASS','timeout_returns=1','eof_returns=1','no_ancillary_walk_on_error=1','fd_leaks=0']:assert marker in state['log'],marker
            report['volumeAdapterErrorsNativeVerified']=True
            if args.require_received_rights:
                for marker in ['RECEIVED_RIGHTS32_PASS','timeout=1','eof=1','actual_rights=1','truncated_rights_closed=1','malformed_length=1','unaligned_control=1','fd_leaks=0']:assert marker in state['log'],marker
                report['receivedRightsNativeVerified']=True
            for marker in ['VOLUME_SESSION32_PASS','authenticated_host_adapter=1','actual_appfuse_device=1','scoped_proxy_metadata=1','revoke_with_cached_holder=1','backing_data=1','provider_handles_and_refs_balanced=1','no_caller_context_preserved=1','bootstrap_lower_scope=1','unscoped_namespace_preserved=1','real_mediaprovider_claim=0']:assert marker in state['log'],marker
            for marker in ['VOLUME_CLIENT32_READY pid='+str(state['holderPid'])+' ','uid=10001','ordinary_openat=1','real_appfuse_data=1','path_fd_attrs=1','create_unlink=1','shared_mmap=1','scoped_kernel_denied=1','host_adapter_peer_denied=1','cached_read=1','held_mapping=1']:assert marker in state['clientLog'],marker
            assert 'VOLUME_CLIENT32_NOT_RETIRED' not in state['clientLog']
            adapter=state['adapter'];assert adapter['attaches']==1 and adapter['revocations']==1 and adapter['denials']==1 and not adapter['attached'] and adapter['workerStopped'] and 'lastError' not in adapter
            assert state['holderPid'] in adapter['retiredHolders']
            report['volumeSessionNativeVerified']=True
            if args.require_live_proc_discovery:
                assert report.get('hostPid',0)>0
                report['adapterHostLog']=adb('shell','logcat -d --pid='+str(report['hostPid'])+' -s PocketExternal32:I PocketHolders32:I *:S')
                assert any('phase=summary scan=proc ' in line and ' result=1 ' in line for line in report['adapterHostLog'].splitlines())
                report['liveProcessDiscoveryVerified']=True
            if args.require_detach_retry:
                for marker in ['VOLUME_DETACH_RETRY32_PASS','lost_reply=1','callback_broker_retained=1','duplicate_mount_denied=1','same_epoch_retry=1','provider_handles_balanced=1']:assert marker in state['log'],marker
                assert adapter['replyFailures']==1 and 'lastReplyFailure' in adapter
                report['detachRetryNativeVerified']=True
            if args.require_app_directories:
                for marker in ['VOLUME_APP_DIRECTORIES32_PASS','setup=1','ensure_existing=1','fix_missing=1','fixup_entries=1','actual_guest_uid_gid_metadata=1','user0_only=1','host_kernel_owner_changed=0']:assert marker in state['log'],marker
                report['appDirectoriesNativeVerified']=True
            if args.require_lower_canonical:
                for marker in ['VOLUME_LOWER_CANONICAL32_PASS','pass_through=1','bionic_realpath=1','storage_alias=1','sdcard_alias=1','view_restored=1']:assert marker in state['log'],marker
                report['lowerCanonicalNativeVerified']=True
            if args.require_path_handles:
                for marker in ['VOLUME_PATH32_PASS','bionic_realpath=1','sdcard_alias=1','path_only_flags=1','metadata=1','proc_link=1','rename_unlink=1','relative_path_fd=1','regular_proxy_link=1']:assert marker in state['clientLog'],marker
                report['pathHandlesNativeVerified']=True
            if args.require_held_anchor:
                for marker in ['VOLUME_HELD_ANCHOR32_PASS','provider_getattr_enoent=1','new_path_open_denied=1','opath_fstat=1','data_fstat=1','data_read=1','external_rename=1']:assert marker in state['clientLog'],marker
                report['heldAnchorNativeVerified']=True
            if args.require_unlink_types:
                for marker in ['VOLUME_UNLINK_TYPES32_PASS','unlink_dir_eisdir=1','rmdir_file_enotdir=1','empty_dir_rmdir=1','provider_assumes_kernel_type_checks=1']:assert marker in state['clientLog'],marker
                report['unlinkTypesNativeVerified']=True
        if namespace:report['namespaceRpcNativeVerified']=True
        if args.require_directory_rpc:
            for marker in ['DIRECTORY_RPC_PASS','provider_metadata=1','logical_flags=1','getdents64=1','dup_cursor=1','independent_open=1','concurrent_reads=1','seek_rewind=1','copy_failure_rollback=1','directory_sync=1','descriptor_io_errors=1','abandoned_fd_release=1','fork_scm_rights_last_close=1']:assert marker in state['log'],marker
            report['directoryRpcNativeVerified']=True
            if metadata.get('directoryLibcChild'):
                child=state['directoryLibcChild'];assert child['validation']=='PASS' and child['exit']==0
                for marker in ['DIRECTORY_LIBC_CLIENT_PASS','uid=10001','kernel_scope_denied=1','real_opendir=1','real_readdir=1','real_rewinddir=1','path_fd_metadata=1','real_sync=1','read_eisdir=1','socket_facade_denied=1','close=1']:assert marker in child['log'],marker
                assert 'DIRECTORY_LIBC_BRIDGE_PASS separate_host_process=1 fresh_interpreter_filter=1 ordinary_guest_uid=10001' in state['log']
                report['directoryLibcSyscallsVerified']=True
                for marker in ['relative_open_stat_mkdir_rename_remove=1','renamed_parent_fd=1','unlinked_directory_fd=1']:assert marker in child['log'],marker
                report['directoryRelativeSyscallsVerified']=True
        if external:
            assert len(state['readTids'])>=2 and set(state['syncFlags'])=={0,1}
            report['externalProxyDataPlaneVerified']=True
        if args.require_storage_mode:
            assert 'FUSE32_STORAGE_MODE_EXEC_PASS mode=3 uid=19991 environment=consumed mutation=denied' in state['log']
            report['storageModeHandoffNativeVerified']=True
        if args.require_state_compat:
            assert args.require_storage_mode and 'mutation=denied groups=64 denylist=1' in state['log']
            report['storageModeStateCompatibilityVerified']=True
        if args.require_peer_identity:
            for marker in ['peer_identity=verified','thread_id=preserved','foreign_thread=denied','gid_only_change=verified']:
                assert marker in state['log'],marker
            report['peerThreadIdentityVerified']=True
        if args.require_concurrent_replies:
            assert 'blocked_sync_read=verified' in state['log'] and 'out_of_order=verified' in state['log']
            report['concurrentFuseRepliesVerified']=True
        if args.require_lower_view:
            assert args.require_storage_mode and 'lower_view=verified forced_fuse=not_bypassed' in state['log']
            report['mediaProviderLowerViewNativeVerified']=True
        if args.require_unix_datagram:
            assert 'unix_datagram_paths=verified' in state['log']
            report['unixDatagramPathsNativeVerified']=True
        assert 'fuse-stock' not in adb('shell','ps -A -o PID,NAME,ARGS')
        report['validation']='PASS'
    except Exception as error:report['validation']='FAIL';report['error']=str(error);raise
    finally:
        if volume:
            try:report['adapterHostLog']=adb('shell','logcat -d'+(' --pid='+str(report['hostPid']) if report.get('hostPid') else '')+' -s PocketExternal32:I PocketHolders32:I *:S')
            except Exception as error:report['adapterHostLogError']=str(error)
        (work/'fuse-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2),flush=True)


if __name__=='__main__':main()
