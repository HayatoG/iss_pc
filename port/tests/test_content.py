from paths import ROOT
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import content_profile

EXE=ROOT/'out/content-test'

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes()),(b'G3CONT01',1,13,0,0,0xffffffff))

    def test_owned_unlockers_become_installed_addons(self):
        # inFAMOUS Second Son: DLC unlockers under <game>-DLC/<content id>/sce_sys/param.sfo.
        sfos={'game':{'TITLE_ID':'CUSA00004'},
              'b':{'CATEGORY':'ac','CONTENT_ID':'EP9000-CUSA00004_00-UNLOCKVESTJOS000'},
              'a':{'CATEGORY':'ac','CONTENT_ID':'EP9000-CUSA00004_00-UNLOCKCOLEJACKET'},
              'other':{'CATEGORY':'ac','CONTENT_ID':'EP9000-CUSA00223_00-UNLOCKSOMETHING0'},  # another title
              'gd':{'CATEGORY':'gd','CONTENT_ID':'EP9000-CUSA00004_00-SECONDSONSHIP000'}}   # not an add-on
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            for name in sfos:
                d=(p/'sce_sys') if name=='game' else (p/'dlc'/name/'sce_sys')
                d.mkdir(parents=True);(d/'param.sfo').write_bytes(name.encode())
            with patch.object(content_profile,'sfo',side_effect=lambda data:sfos[data.decode()]):
                content_profile.prepare(p,p,addcont=p/'dlc')
            data=(p/'content.bin').read_bytes()
        self.assertEqual(data[:8],b'G3CONT02')
        self.assertEqual(struct.unpack_from('<I',data,28)[0],2)
        self.assertEqual(data[32:],b'UNLOCKCOLEJACKETUNLOCKVESTJOS000')

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_installed_addons_are_listed_and_found(self):
        r=self.run_case('--addons');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
