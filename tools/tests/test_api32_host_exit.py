import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api32_app_on_pi import host_process_exit_observation, pin_host_process_identity

class HostExitObservationTest(unittest.TestCase):
    package='dev.kreijstal.aosppocket.api32managerimage'
    identity={'pid':42,'uid':10157,'startTimeTicks':1234}
    @staticmethod
    def stat(start=1234,name='app (worker)',state='S',pid=42):
        return str(pid)+' ('+name+') '+' '.join([state]+['0']*18+[str(start)]+['0']*6)+'\n'
    def table(self,name=None,uid=10157):
        return 'PID UID NAME\n1 0 init\n42 '+str(uid)+' '+(name or self.package)+'\n'
    def adb(self,processes,stat=None,exit_info='pid=42 reason=3 (LOW_MEMORY)'):
        calls=[]
        if stat is None:stat=self.stat()
        def execute(*args):
            calls.append(args)
            if args==('shell','ps -A -o PID,UID,NAME:128'):value=processes
            elif args==('shell','if [ -d /proc/42 ]; then cat /proc/42/stat; else echo POCKET_HOST_PID_ABSENT; fi'):value=stat
            elif args==('shell','dumpsys activity exit-info '+self.package):value=exit_info
            else:self.fail('Unexpected operation: '+repr(args))
            if isinstance(value,Exception):raise value
            return value
        return execute,calls
    def test_live_parent_does_not_query_exit_history(self):
        adb,calls=self.adb(self.table())
        self.assertIsNone(host_process_exit_observation(adb,42,self.package,self.identity))
        self.assertEqual(len(calls),2)
    def test_startup_and_later_name_changes_preserve_kernel_identity(self):
        for name in ['<pre-initialized>',self.package,'renamed process']:
            with self.subTest(name=name):
                adb,calls=self.adb(self.table(name))
                self.assertEqual(pin_host_process_identity(adb,42,self.package,10157),self.identity)
                self.assertIsNone(host_process_exit_observation(adb,42,self.package,self.identity))
                self.assertFalse(any('exit-info' in args[1] for args in calls))
    def test_missing_ps_row_but_live_proc_is_not_exit(self):
        adb,_=self.adb('PID UID NAME\n1 0 init\n')
        self.assertIsNone(host_process_exit_observation(adb,42,self.package,self.identity))
    def test_absent_proc_preserves_memory_exit_evidence(self):
        adb,calls=self.adb('PID UID NAME\n1 0 init\n43 0 zygote64\n','POCKET_HOST_PID_ABSENT\n')
        result=host_process_exit_observation(adb,42,self.package,self.identity)
        self.assertTrue(result['originalProcessAbsent'])
        self.assertEqual(result['identityReason'],'proc-absent')
        self.assertIn('LOW_MEMORY',result['exitInfo'])
        self.assertEqual(len(calls),3)
    def test_same_uid_and_name_pid_reuse_is_detected_by_birth(self):
        adb,_=self.adb(self.table(),self.stat(start=5678))
        result=host_process_exit_observation(adb,42,self.package,self.identity)
        self.assertEqual(result['identityReason'],'pid-reused')
        self.assertEqual(result['observedIdentity']['startTimeTicks'],5678)
    def test_different_uid_pid_reuse_never_touches_new_owner(self):
        adb,calls=self.adb(self.table('unrelated.app',10158),self.stat(start=5678))
        result=host_process_exit_observation(adb,42,self.package,self.identity)
        self.assertEqual(result['pidReusedBy'],'unrelated.app')
        self.assertEqual(result['observedIdentity']['uid'],10158)
        self.assertTrue(all(args[0]=='shell' and 'kill' not in args[1] for args in calls))
    def test_zombie_is_terminated_without_claiming_pid_absence(self):
        adb,_=self.adb(self.table(),self.stat(state='Z'))
        result=host_process_exit_observation(adb,42,self.package,self.identity)
        self.assertTrue(result['originalProcessExited'])
        self.assertFalse(result['originalProcessAbsent'])
        self.assertEqual(result['identityReason'],'terminated')
    def test_adb_failure_incomplete_and_duplicate_ps_cannot_prove_exit(self):
        for data in [TimeoutError('ADB timed out'),'','PID UID NAME\n','PID UID NAME\n1 0 not-init\n',
                     'PID UID NAME\n1 0 init\nbroken\n',self.table()+'42 10157 duplicate\n']:
            with self.subTest(data=str(data)):
                adb,calls=self.adb(data)
                with self.assertRaises(Exception):host_process_exit_observation(adb,42,self.package,self.identity)
                self.assertEqual(len(calls),1)
    def test_denied_and_malformed_proc_are_not_absence(self):
        for value in [PermissionError('stat denied'),'',self.stat(pid=43),'42 (bad) S 0']:
            with self.subTest(value=str(value)):
                adb,calls=self.adb(self.table(),value)
                with self.assertRaises(Exception):host_process_exit_observation(adb,42,self.package,self.identity)
                self.assertEqual(len(calls),2)
    def test_uid_change_without_new_birth_is_inconclusive(self):
        adb,_=self.adb(self.table(uid=10158))
        with self.assertRaises(RuntimeError):host_process_exit_observation(adb,42,self.package,self.identity)
    def test_no_birth_pin_never_infers_reuse_from_name(self):
        adb,calls=self.adb(self.table('<pre-initialized>'))
        self.assertIsNone(host_process_exit_observation(adb,42,self.package))
        self.assertEqual(len(calls),2)
    def test_missing_exit_history_keeps_proven_proc_absence(self):
        adb,_=self.adb('PID UID NAME\n1 0 init\n','POCKET_HOST_PID_ABSENT',PermissionError('exit-info denied'))
        result=host_process_exit_observation(adb,42,self.package,self.identity)
        self.assertTrue(result['originalProcessAbsent'])
        self.assertIn('denied',result['exitInfoError'])
    def test_invalid_pins_rejected_before_adb(self):
        for pid,package in [(0,self.package),(True,self.package),(42,'unsafe;command')]:
            adb,calls=self.adb('')
            with self.assertRaises(ValueError):host_process_exit_observation(adb,pid,package,self.identity)
            self.assertEqual(calls,[])
        adb,calls=self.adb('')
        with self.assertRaises(ValueError):host_process_exit_observation(adb,42,self.package,dict(self.identity,pid=43))
        self.assertEqual(calls,[])

if __name__=='__main__':unittest.main()
