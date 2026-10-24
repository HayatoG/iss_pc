"""Release-note regression cases; GitHub responses are local fixtures, with no network."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('release_notes.sh')
FAKE_GH = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
d = json.loads(Path(os.environ['FIXTURE']).read_text())
a = sys.argv[1:]
if a[0] == 'api' and any('/compare/' in x for x in a):
    assert '--paginate' in a
    print('first\\nsecond')
elif a[0] == 'api' and any('/commits/' in x for x in a):
    if d.get('fail_second') and any('/second/' in x for x in a):
        sys.exit(1)
    print('7' if not d.get('no_pr') else '')
elif a[:2] == ['pr', 'view']:
    print(json.dumps({'title': 'Fallback title', 'body': d['body'],
                      'closingIssuesReferences': []}))
elif a[0] == 'api' and any('/issues/' in x for x in a):
    print('reporter')
else:
    raise AssertionError(a)
'''


class ReleaseNotesTests(unittest.TestCase):
    def run_notes(self, body='', **extra):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'gh').write_text(FAKE_GH)
            (root / 'gh').chmod(0o755)
            fixture = root / 'fixture.json'
            fixture.write_text(json.dumps({'body': body, **extra}))
            return subprocess.run(
                ['bash', str(SCRIPT), 'head'], text=True, capture_output=True,
                env={**os.environ, 'PATH': folder + os.pathsep + os.environ['PATH'],
                     'FIXTURE': str(fixture), 'GH_REPO': 'owner/repo', 'PREV': 'previous'},
                timeout=10)

    def test_language_specific_closers_do_not_mix_languages(self):
        result = self.run_notes('''<!-- notes:pt-BR -->
- Português (#12)
<!-- /notes:pt-BR -->
<!-- notes:en-US -->
- English (#12)
<!-- /notes:en-US -->''')
        self.assertEqual(result.returncode, 0, result.stderr)
        # English first, then Portuguese (#517).
        en, pt = result.stdout.split("## Novidades")
        self.assertNotIn('- English', pt)
        self.assertNotIn('- Português', en)
        self.assertIn('#12 (@reporter)', pt)
        self.assertIn('#12 (@reporter)', en)

    def test_empty_template_falls_back_and_no_reporters_is_valid(self):
        result = self.run_notes('<!-- notes:pt-BR -->\n- \n<!-- /notes -->')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('- Fallback title (PR #7)'), 2)
        self.assertNotIn('Issues relatadas', result.stdout)

    def test_every_bullet_has_a_pr_reference_and_repeated_pr_is_deduplicated(self):
        result = self.run_notes('''<!-- notes:pt-BR -->
- First
- Second
<!-- /notes -->
<!-- notes:en-US -->
- Third
<!-- /notes -->''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count('(PR #7)'), 3)

    def test_new_language_marker_also_ends_previous_block(self):
        result = self.run_notes('<!-- notes:pt-BR -->\n- PT\n<!-- notes:en-US -->\n- EN')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('- EN', result.stdout.split("## What's new")[0])

    def test_api_error_cannot_publish_partial_notes(self):
        result = self.run_notes(fail_second=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')

    def test_no_merged_pr_has_no_notes(self):
        result = self.run_notes(no_pr=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
