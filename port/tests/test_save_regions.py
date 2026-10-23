"""Regional saves must stay separate through backup, duplicate and ZIP round trips."""
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import savemanager as sm
import userdata


class RegionTests(unittest.TestCase):
    def test_same_slot_in_both_regions_keeps_identity_and_contents(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for serial in userdata.SERIALS:
                p = root / 'savedata/1' / serial / 'infamousssUser0'
                p.mkdir(parents=True)
                (p / 'dis.dat').write_bytes(serial.encode())
            eu = '1/CUSA00004/infamousssUser0'
            self.assertEqual({s['id'] for s in sm.list_slots(root)}, {'1/infamousssUser0', eu})
            self.assertTrue(userdata.has_saves(root))
            backup = sm.backup_now(root, extra='')['path']
            self.assertEqual((backup / '1/CUSA00004/infamousssUser0/dis.dat').read_bytes(), b'CUSA00004')
            new = sm.duplicate_slot(root, eu, running=lambda: False)
            self.assertEqual(new, '1/CUSA00004/infamousssUser1')
            archive = root / 'export.zip'
            self.assertEqual(sm.export_zip(root, archive, [eu]), 1)
            with zipfile.ZipFile(archive) as z:
                self.assertIn('savedata/1/CUSA00004/infamousssUser0/dis.dat', z.namelist())
            plan = sm.plan_from_zip(archive)
            dest = root / 'imported'
            result = sm.import_plan(dest, plan, target_uid='2', running=lambda: False)
            self.assertEqual(result['imported'], ['2/CUSA00004/infamousssUser0'])
            self.assertEqual((dest / 'savedata/2/CUSA00004/infamousssUser0/dis.dat').read_bytes(), b'CUSA00004')
            self.assertFalse((dest / 'savedata/2/CUSA00223').exists())
            sm.delete_slot(dest, '2/CUSA00004/infamousssUser0', running=lambda: False)
            self.assertEqual(sm.list_slots(dest), [])

    def test_region_cannot_escape_save_root(self):
        for sid in ('1/../../infamousssUser0', '1/CUSA99999/infamousssUser0', '../infamousssUser0'):
            with self.assertRaises(sm.SaveError):
                sm.slot_paths(Path('.'), sid)


if __name__ == '__main__':
    unittest.main()
