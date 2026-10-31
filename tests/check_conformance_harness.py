"""Regression checks for the conformance runner, without native extensions."""
import contextlib
import io
import subprocess
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import check_conformance as checker


class ConformanceHarnessTests(unittest.TestCase):
    def run_main(self, records, expectation='i', empty=False):
        with tempfile.TemporaryDirectory() as tmp:
            case = ('suite', str(Path(tmp) / 'case.json'), expectation)
            modules = {lib: types.SimpleNamespace(__version__='test') for lib in ('oxjson', 'orjson')}
            with patch.object(sys, 'argv', ['check_conformance.py', tmp]), \
                    patch.dict(sys.modules, modules), \
                    patch.object(checker, 'cases', return_value=[] if empty else [case]), \
                    patch.object(checker, 'run_lib', side_effect=lambda lib, *args: {0: records[lib]}), \
                    contextlib.redirect_stdout(io.StringIO()):
                return checker.main()

    def records(self):
        return {lib: dict(ok=True, val='same', back=True, exact=True) for lib in checker.LIBS}

    def test_value_difference_fails(self):
        records = self.records()
        records['oxjson']['val'] = 'different'
        self.assertEqual(self.run_main(records), 1)

    def test_acceptance_difference_fails(self):
        records = self.records()
        records['oxjson'] = dict(ok=False, err='rejected')
        self.assertEqual(self.run_main(records), 1)

    def test_wrong_fixed_expectation_fails(self):
        self.assertEqual(self.run_main(self.records(), expectation='n'), 1)

    def test_crash_fails(self):
        records = self.records()
        records['oxjson'] = dict(ok=None, err='CRASH')
        self.assertEqual(self.run_main(records), 1)

    def test_stdlib_crash_is_informational(self):
        records = self.records()
        records['json'] = dict(ok=None, err='CRASH')
        self.assertEqual(self.run_main(records), 0)

    def test_empty_corpus_fails(self):
        self.assertEqual(self.run_main(self.records(), empty=True), 1)

    def test_matching_results_pass(self):
        self.assertEqual(self.run_main(self.records()), 0)

    def test_stdlib_semantic_difference_is_informational(self):
        records = self.records()
        records['json']['val'] = 'different'
        self.assertEqual(self.run_main(records), 0)

    def test_roundtrip_failure_fails(self):
        records = self.records()
        records['oxjson']['back'] = False
        self.assertEqual(self.run_main(records), 1)

    def test_dumps_acceptance_difference_fails(self):
        records = self.records()
        records['oxjson']['dumperr'] = 'TypeError'
        self.assertEqual(self.run_main(records), 1)

    def test_documented_depth_difference_passes(self):
        records = self.records()
        records['orjson']['dumperr'] = 'TypeError: Recursion limit reached'
        self.assertEqual(self.run_main(records), 0)

    def test_other_orjson_dumps_error_fails(self):
        records = self.records()
        records['orjson']['dumperr'] = 'TypeError: something else'
        self.assertEqual(self.run_main(records), 1)

    def test_oxjson_recursion_error_fails(self):
        records = self.records()
        records['oxjson']['dumperr'] = 'TypeError: Recursion limit reached'
        self.assertEqual(self.run_main(records), 1)

    def test_canonicalization_failure_fails(self):
        records = self.records()
        for r in records.values():
            r['val'] = 'canon-fail MemoryError'
        self.assertEqual(self.run_main(records), 1)

    def test_invalid_worker_output_fails(self):
        for output, rc in [('', 0), ('DONE\n', 0), ('DONE\n', 7),
                           ('START 0\nR {"i":0,"ok":true}\nDONE\n', 0),
                           ('START 0\nR {"i":0,"ok":false,"err":"x"}\nDONE\n', 7)]:
            with self.subTest(output=output, rc=rc), tempfile.TemporaryDirectory() as tmp:
                def fake_run(*args, **kwargs):
                    (Path(tmp) / 'out_oxjson.txt').write_text(output, encoding='utf-8')
                    return types.SimpleNamespace(returncode=rc, stderr='test')
                with patch.object(checker.subprocess, 'run', side_effect=fake_run):
                    with self.assertRaises((RuntimeError, SystemExit)):
                        checker.run_lib('oxjson', 'unused', 1, tmp)

    def test_empty_corpus_cli_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([sys.executable, checker.__file__, tmp], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('no conformance files', result.stdout)

    def test_crashed_case_is_recorded_and_next_case_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            outputs = iter([
                ('START 0\n', 7),
                ('START 1\nR {"i":1,"ok":false,"err":"bad JSON"}\nDONE\n', 0),
            ])
            def fake_run(*args, **kwargs):
                output, rc = next(outputs)
                (Path(tmp) / 'out_oxjson.txt').write_text(output, encoding='utf-8')
                return types.SimpleNamespace(returncode=rc, stderr='test')
            with patch.object(checker.subprocess, 'run', side_effect=fake_run):
                results = checker.run_lib('oxjson', 'unused', 2, tmp)
            self.assertIsNone(results[0]['ok'])
            self.assertIs(results[1]['ok'], False)


if __name__ == '__main__':
    unittest.main()
