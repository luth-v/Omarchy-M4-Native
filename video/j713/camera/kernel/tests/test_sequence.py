# SPDX-License-Identifier: GPL-2.0-only
"""Exact reviewed production fixture, including wire, pool, IPC and log bytes."""
import hashlib
import json
from pathlib import Path
import subprocess
import unittest

HERE = Path(__file__).resolve().parent


class ProductionSequence(unittest.TestCase):
    def test_reviewed_production_bytes(self):
        expected = json.loads((HERE / 'session-production-baseline.json').read_text())
        result = subprocess.run([str(HERE / '.build/sequence_test'), '--production-trace'],
                                check=True, capture_output=True)
        self.assertEqual(len(result.stdout), expected['trace_bytes'])
        self.assertEqual(hashlib.sha256(result.stdout).hexdigest(), expected['trace_sha256'])


if __name__ == '__main__':
    unittest.main()
