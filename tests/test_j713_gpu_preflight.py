# SPDX-License-Identifier: MIT
import copy
import importlib.util
from pathlib import Path
import plistlib
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_preflight', ROOT / 'tools/m4-j713-gpu-preflight.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)

# A 13-inch M4 Air whose top GPU state is 1470 MHz at 995 mV.
FIXTURE = ROOT / 'tests/fixtures/j713-sgx-995mv.plist'


def table(node, name):
    return [list(row) for row in struct.iter_unpack('<II', node[name])]


def pack(rows):
    return b''.join(struct.pack('<II', *row) for row in rows)


class GpuPreflightTests(unittest.TestCase):
    def setUp(self):
        self.node = plistlib.loads(FIXTURE.read_bytes())[0]

    def with_top_voltage(self, mv):
        node = copy.deepcopy(self.node)
        for name in ('perf-states', 'perf-states-sram'):
            rows = table(node, name)
            for row in rows[-2:]:
                row[1] = mv
            node[name] = pack(rows)
        return node

    def test_995_mv_air_fails_the_exact_power_gate(self):
        report = preflight.evaluate(self.node, 10)
        self.assertIsNone(report['boot_abort'])
        self.assertEqual((report['groups'], report['top_mhz'], report['top_mv']), (10, 1470, 995))
        self.assertEqual(report['max_power_mw'], 24676)
        self.assertEqual(report['gpu_failures'],
                         ['maximum power 24676 mW, module requires 24800'])

    def test_1000_mv_air_matches_the_qualified_constants(self):
        report = preflight.evaluate(self.with_top_voltage(1000), 10)
        self.assertEqual(report['max_power_mw'], 24800)
        self.assertEqual(report['gpu_failures'], [])
        self.assertEqual(report['unknown'], [])

    def test_relaxed_gate_accepts_power_at_or_below_the_qualified_budget(self):
        self.assertEqual(preflight.evaluate(self.node, 10, exact_power=False)['gpu_failures'], [])
        report = preflight.evaluate(self.with_top_voltage(1005), 10, exact_power=False)
        self.assertEqual(report['max_power_mw'], 24924)
        self.assertEqual(report['gpu_failures'],
                         ['maximum power 24924 mW, module requires at most 24800'])

    def test_eight_core_air_fails_the_core_mask_gate(self):
        report = preflight.evaluate(self.with_top_voltage(1000), 8)
        self.assertIn('core mask for 8 cores is not 0x3ff', report['gpu_failures'])

    def test_unclamped_power_is_reported_as_unknown(self):
        # Eight cores keep the dynamic current below the clamp.
        report = preflight.evaluate(self.node, 8)
        self.assertIsNone(report['max_power_mw'])
        self.assertEqual(report['unknown'], ['maximum power depends on the leakage fuse'])

    def test_tables_that_make_m1n1_abort_the_boot(self):
        unordered = copy.deepcopy(self.node)
        for name in ('perf-states', 'perf-states-sram'):
            rows = table(unordered, name)
            rows[-2][1] = rows[-1][1] = 900
            unordered[name] = pack(rows)
        mismatched = copy.deepcopy(self.node)
        rows = table(mismatched, 'perf-states-sram')
        rows[3][0] += 1000000
        mismatched['perf-states-sram'] = pack(rows)
        merged = self.with_top_voltage(920)
        for node, reason in ((unordered, 'unordered voltage steps'),
                             (mismatched, 'inconsistent performance row 3'),
                             (merged, 'unsupported voltage step count 9')):
            with self.subTest(reason=reason):
                self.assertEqual(preflight.evaluate(node, 10)['boot_abort'], reason)

    def test_unsupported_core_count_aborts_the_boot(self):
        self.assertEqual(preflight.evaluate(self.node, 9)['boot_abort'],
                         'unsupported GPU core count 9')

    def edited(self, edit):
        node = copy.deepcopy(self.node)
        for name in ('perf-states', 'perf-states-sram'):
            rows = table(node, name)
            edit(name, rows)
            node[name] = pack(rows)
        return node

    def assertAborts(self, node, reason):
        report = preflight.evaluate(node, 10)
        self.assertEqual(report['boot_abort'], reason)
        self.assertEqual(report['gpu_failures'], [])

    def test_incomplete_tables_abort_the_boot(self):
        for name, value, reason in (
                ('perf-states', None, 'incomplete performance tables'),
                ('perf-states-sram', 'short', 'incomplete performance tables'),
                ('perf-state-count', None, 'unsupported performance table geometry'),
                ('perf-state-table-count', b'\x01\x00', 'unsupported performance table geometry')):
            with self.subTest(name=name, value=value):
                node = copy.deepcopy(self.node)
                if value is None:
                    del node[name]
                else:
                    node[name] = node[name][:-8] if value == 'short' else value
                self.assertAborts(node, reason)

    def test_base_performance_state(self):
        for value, reason in ((b'\x01\x00', 'invalid base performance state'),
                              (struct.pack('<I', 0), 'invalid transfer tables (base performance state 0)'),
                              (struct.pack('<I', 10), 'invalid transfer tables (base performance state 10)'),
                              (struct.pack('<I', 3), None)):
            with self.subTest(value=value):
                node = copy.deepcopy(self.node)
                node['gpu-perf-base-pstate'] = value
                self.assertEqual(preflight.evaluate(node, 10)['boot_abort'], reason)

    def test_current_limit_selector(self):
        node = copy.deepcopy(self.node)
        node['gpu-supports-higher-mA'] = b'\x01'
        self.assertAborts(node, 'invalid current-limit selector')
        node['gpu-supports-higher-mA'] = struct.pack('<I', 1)
        report = preflight.evaluate(node, 10)
        self.assertIsNone(report['boot_abort'])
        self.assertEqual(report['unknown'], ['maximum power depends on the leakage fuse'])

    def test_group_voltage_outside_the_power_model(self):
        def zero(name, rows):
            if name == 'perf-states':
                rows[0][1] = 0
        self.assertAborts(self.edited(zero), 'invalid power inputs (0 mV)')
        self.assertAborts(self.with_top_voltage(2001), 'invalid power inputs (2001 mV)')

    def test_transfer_tables_that_make_m1n1_abort_the_boot(self):
        def lowest(name, rows):
            rows[0][0] = 100 * 1000000

        def unordered(name, rows):
            rows[3][0] = 600 * 1000000

        def flat_top(name, rows):
            rows[14][0] = rows[15][0] = 1338 * 1000000
        self.assertAborts(self.edited(lowest), 'invalid transfer tables (lowest frequency 100 MHz)')
        self.assertAborts(self.edited(unordered),
                          'invalid transfer tables (unordered frequency at group 3)')
        node = self.edited(flat_top)
        self.assertAborts(node, 'invalid transfer tables (unordered frequency at group 9)')
        node['gpu-perf-base-pstate'] = struct.pack('<I', 8)
        self.assertAborts(node, 'invalid transfer tables (top frequency not above the base state)')

    def test_tiny_top_power_is_reported_as_unknown(self):
        rows = [(mhz * 1000000, mv) for mhz, mv in zip(range(10), range(1, 11))]
        node = {'perf-state-count': struct.pack('<I', 10),
                'perf-state-table-count': struct.pack('<I', 1),
                'perf-states': pack(rows), 'perf-states-sram': pack(rows)}
        report = preflight.evaluate(node, 10)
        self.assertIsNone(report['boot_abort'])
        self.assertEqual(report['unknown'],
                         ['maximum power depends on the leakage fuse',
                          'whether the top power is non-zero depends on the leakage fuse'])

if __name__ == '__main__':
    unittest.main()
