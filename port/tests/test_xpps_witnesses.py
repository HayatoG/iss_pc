"""tools/xpps/witnesses.py: the descriptors renderer v2 recorded in play, merged into the table the
job generator reads, and the extra records it makes from them.

SYNTHETIC data only (made-up hashes and sharps): nothing of the game is needed.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'tools'))
from xpps import jobs as J, signature as S, witnesses as W  # noqa: E402
from test_xpps_jobs import Base, PS_HASH, VS_HASH, vsharp  # noqa: E402


def line(vs=VS_HASH, ps=PS_HASH, vs_words=((4, 0x100), (5, 64 << 16), (6, 1), (7, 0x27FAC)),
         ps_words=(), vb=((10, 0, 0x100, 12 << 16, 1, 0x6FAC),)):
    return json.dumps({'vs': hex(vs), 'ps': hex(ps), 'VS': [list(w) for w in vs_words],
                       'PS': [list(w) for w in ps_words], 'vb': [list(a) for a in vb]})


class Merge(unittest.TestCase):
    def test_lines_become_variants_once(self):
        table, stats = W.merge([line(), line(), '', line(vs_words=((5, 48 << 16),))])
        self.assertEqual({'lines': 3, 'rejected': 0, 'added': 2}, stats)
        self.assertEqual(2, len(table['pairs'][f'{VS_HASH:#x}:{PS_HASH:#x}']))

    def test_a_vertex_sharp_with_an_address_is_rejected(self):
        for vb in (((10, 0, 0x7F001000, 12 << 16, 1, 0),),     # address in dword 0
                   ((10, 0, 0x100, 12 << 16 | 0x10, 1, 0),),   # address bits in dword 1
                   ((10, 0, 0x100, 12 << 16, 4096, 0),)):     # a size instead of bound/unbound
            table, stats = W.merge([line(vb=vb)])
            self.assertEqual(1, stats['rejected'])
            self.assertEqual({}, table['pairs'])

    def test_the_padding_of_a_vertex_sharp_is_cleared(self):
        table, stats = W.merge([line(vb=((10, 0, 0x100, 12 << 16 | 0x7000, 1, 0),))])
        self.assertEqual(0, stats['rejected'])
        variant = next(iter(table['pairs'].values()))[0]
        self.assertEqual([[10, 0, 0x100, 12 << 16, 1, 0]], variant['vb'])

    def test_malformed_lines_are_rejected(self):
        _table, stats = W.merge(['{', '{"vs": "zz"}', json.dumps({'vs': '0x1'})])
        self.assertEqual(3, stats['rejected'])

    def test_merging_again_adds_nothing(self):
        table, _ = W.merge([line()])
        table, stats = W.merge([line()], json.loads(json.dumps(table)))
        self.assertEqual(0, stats['added'])

    def test_load_round_trip(self):
        table, _ = W.merge([line()])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 't.json'
            p.write_text(json.dumps(table))
            loaded = W.load(p)
            self.assertEqual({}, W.load(Path(d) / 'missing.json'))
        variant = loaded[(VS_HASH, PS_HASH)][0]
        self.assertEqual(64 << 16, variant['VS'][5])
        self.assertEqual([[10, 0, 0x100, 12 << 16, 1, 0x6FAC]], variant['vb'])


class Records(Base):
    def variant(self, stride=48):
        words = vsharp(stride=stride)
        fetched = vsharp(dfmt=13)   # the base record's vertex V#, as the recorder writes it
        _k, v = W.parse_line(line(vs_words=((4, 0x100), (5, words[1]), (6, 1), (7, words[3])),
                                  vb=((10, 0, 0x100, fetched[1], 1, fetched[3]),)))
        return v

    def test_apply_replaces_the_observed_dwords_and_vertex_sharps(self):
        rec = self.graphics(stride=64)
        out = W.apply(rec, self.variant(stride=48), J.S_PS, J.S_VS)
        vs = next(st for st in out['stages'] if st['sw'] == J.S_VS)
        self.assertEqual(48 << 16, vs['flat'][5])
        self.assertEqual(vs['flat'][:16], vs['ud'])
        fetched = vsharp(dfmt=13)
        self.assertEqual([(10, 0, (0x100, fetched[1], 1, fetched[3]))], vs['vsharps'])
        # the record it was made from is untouched
        self.assertEqual(vsharp(stride=64)[1], rec['stages'][1]['flat'][5])

    def test_apply_extends_a_short_flat(self):
        _k, v = W.parse_line(line(vs_words=((40, 7),)))
        out = W.apply(self.graphics(), v, J.S_PS, J.S_VS)
        self.assertEqual(7, out['stages'][1]['flat'][40])

    def test_builder_adds_one_record_per_new_signature(self):
        class Ix:
            headers = self.headers
        b = J.Builder.__new__(J.Builder)
        b.ix, b.seen, b.stats = Ix(), set(), J.Counter()
        base = self.graphics(stride=64)
        b.seen.add(S.freeze(S.signature(base, self.headers)))
        b.witnesses = {(VS_HASH, PS_HASH): [self.variant(48), self.variant(48), self.variant(64)]}
        out = list(b.observed(VS_HASH, PS_HASH, [base]))
        # stride 48 is new (once), stride 64 is the base record again
        self.assertEqual(1, len(out))
        self.assertEqual(48 << 16, out[0]['stages'][1]['flat'][5])
        self.assertEqual({'witness_records': 1, 'witness_duplicate': 2}, dict(b.stats))
        self.assertEqual([], list(b.observed(PS_HASH, VS_HASH, [base])))


if __name__ == '__main__':
    unittest.main()
