# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import import_addcont as addon


def sfo(values):
    keys, data, entries = bytearray(), bytearray(), bytearray()
    for key, value in sorted(values.items()):
        raw = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x204, len(raw), len(raw), len(data))
        keys += key.encode() + b'\0'
        data += raw
    return struct.pack('<4sIIII', b'\0PSF', 0x101, 20 + len(entries), 20 + len(entries) + len(keys), len(values)) + entries + keys + data


def pkg(data, flags=0, offset=64):
    header = bytearray(32)
    header[:4] = b'\x7fCNT'
    struct.pack_into('>I', header, 0x10, 1)
    struct.pack_into('>I', header, 0x18, 32)
    return header + struct.pack('>6I8x', 0x1000, 0, flags, 0, offset, len(data)) + data


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_sys/param.sfo').write_bytes(sfo({'TITLE_ID': 'CUSA00004'}))
        self.values = {'CATEGORY': 'ac', 'CONTENT_ID': 'EP9000-CUSA00004_00-UNLOCKCOLEJACKET', 'TITLE': "Cole's Jacket"}
        self.source = self.root / 'owned.pkg'
        self.source.write_bytes(pkg(sfo(self.values)))

    def test_inspect_locates_pkg_entry_by_id_without_installing(self):
        title, records = addon.inspect(self.game, [self.source])
        self.assertEqual(title, 'CUSA00004')
        self.assertEqual([r['label'] for r in records], ['UNLOCKCOLEJACKET'])
        self.assertFalse(self.game.with_name('game-DLC').exists())

    def test_import_is_idempotent_and_preserves_game_files(self):
        original = (self.game / 'sce_sys/param.sfo').read_bytes()
        _, records = addon.inspect(self.game, [self.source])
        root = addon.install(self.game, records)
        dest = root / self.values['CONTENT_ID'] / 'sce_sys/param.sfo'
        stamp = dest.stat().st_mtime_ns
        addon.install(self.game, records)
        self.assertEqual(dest.read_bytes(), sfo(self.values))
        self.assertEqual(dest.stat().st_mtime_ns, stamp)
        self.assertEqual((self.game / 'sce_sys/param.sfo').read_bytes(), original)

    def test_rejects_other_titles_base_game_and_unsafe_content_ids(self):
        for changes in ({'CATEGORY': 'gd'}, {'CONTENT_ID': 'EP9000-CUSA00223_00-UNLOCKCOLEJACKET'},
                        {'CONTENT_ID': '../outside'}, {'CONTENT_ID': 'EP9000-CUSA00004_00-../escape'}):
            with self.subTest(changes=changes):
                self.source.write_bytes(pkg(sfo(dict(self.values, **changes))))
                with self.assertRaises(ValueError):
                    addon.inspect(self.game, [self.source])

    def test_rejects_encrypted_truncated_and_out_of_range_metadata(self):
        for raw in (b'bad', pkg(sfo(self.values), flags=0x80000000), pkg(sfo(self.values), offset=0xfffffff0),
                    pkg(sfo(self.values))[:-4]):
            with self.subTest(size=len(raw)):
                self.source.write_bytes(raw)
                with self.assertRaises(ValueError):
                    addon.inspect(self.game, [self.source])

    def test_rejects_malformed_sfo_table_and_entry_bounds(self):
        for index, value in ((8, 1), (12, 0xffffffff), (16, 0xffffffff), (28, 0xffffffff)):
            raw = bytearray(sfo(self.values))
            struct.pack_into('<I', raw, index, value)
            with self.assertRaises(ValueError):
                addon.checked_sfo(raw)

    def test_duplicate_sources_are_deduplicated_but_conflicts_are_refused(self):
        second = self.root / 'param.sfo'
        second.write_bytes(sfo(self.values))
        self.assertEqual(len(addon.inspect(self.game, [self.source, second])[1]), 1)
        second.write_bytes(sfo(dict(self.values, TITLE='different')))
        with self.assertRaises(ValueError):
            addon.inspect(self.game, [self.source, second])

    def test_existing_different_metadata_is_not_overwritten(self):
        _, records = addon.inspect(self.game, [self.source])
        root = addon.install(self.game, records)
        dest = root / self.values['CONTENT_ID'] / 'sce_sys/param.sfo'
        dest.write_bytes(b'existing')
        with self.assertRaises(ValueError):
            addon.install(self.game, records)
        self.assertEqual(dest.read_bytes(), b'existing')

    def test_no_inputs_and_oversized_sfo_are_refused(self):
        (self.root / 'empty').mkdir()
        with self.assertRaises(ValueError):
            addon.inspect(self.game, [self.root / 'empty'])
        with self.assertRaises(ValueError):
            addon.checked_sfo(b'x' * (addon.LIMIT + 1))


if __name__ == '__main__':
    unittest.main()
