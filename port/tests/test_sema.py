from paths import ROOT
from pathlib import Path
import os
import subprocess
import unittest

EXE=ROOT/'out/sema-test'

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class SemaphoreTests(unittest.TestCase):
    def run_case(self,*args,env=None):
        full=dict(os.environ,**(env or {}))
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=10,env=full)

    def test_sync_toggles_keep_contracts(self):
        # TLG_SEMA_SRW, TLG_EF_SRW and TLG_EF_WAKE_WAITERS change the host primitives only.
        on={'TLG_SEMA_SRW':'1','TLG_EF_SRW':'1','TLG_EF_WAKE_WAITERS':'1'}
        for env in ({},{'TLG_EF_WAKE_WAITERS':'1'},on):
            for case in ((),('--concurrency',),('--cancel-delete',),('--priority',),('--event-flags',)):
                with self.subTest(env=env,case=case):
                    r=self.run_case(*case,env=env)
                    self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_counts_errors_timeout_and_stale_ids(self):
        r=self.run_case()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_fifo_wakeup_and_token_reservation(self):
        r=self.run_case('--concurrency')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_cancel_and_delete_wake_all_waiters(self):
        r=self.run_case('--cancel-delete')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_priority_wait_blocks_until_signal(self):
        r=self.run_case('--priority')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
