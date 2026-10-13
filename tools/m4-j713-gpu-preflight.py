#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Predict, from macOS and without changes, whether the J713 GPU will start.

m1n1 stage 2 groups the Air's GPU performance states by voltage and models a
maximum power for the top group (kboot_gpu.c dt_set_gpu_t8132_perf,
m4_power.h). It aborts the whole boot on an inconsistent table. The alpha.1
GPU module then accepts only the qualified board constants below and fails
its probe with -22 (no log line) for any other value, leaving no desktop.

This reads the same Apple device-tree properties through ioreg. The leakage
fuse is not visible from macOS, so the power is only predicted when the
dynamic current alone reaches the current clamp, which then fixes the result.
"""

import argparse
import json
import plistlib
import re
import struct
import subprocess
import sys

QUALIFIED = {'core_mask': 0x3ff, 'groups': 10, 'top_mhz': 1470, 'max_power_mw': 24800}
MAX_PSTATES = 16


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def u32(node, name, default=None):
    value = node.get(name)
    if value is None:
        return default
    if not isinstance(value, bytes) or len(value) != 4:
        raise ValueError('invalid ' + name)
    return struct.unpack('<I', value)[0]


def states(node, name, count):
    value = node.get(name)
    if not isinstance(value, bytes) or len(value) != count * 8:
        raise ValueError('incomplete ' + name)
    return [struct.unpack_from('<II', value, offset) for offset in range(0, len(value), 8)]


def group(core, sram):
    """Mirror m1n1's voltage grouping; return (groups, abort reason or None)."""
    groups = []
    first = 0
    while first < len(core):
        last = first
        while last + 1 < len(core) and core[last + 1][1] == core[first][1]:
            last += 1
        rows = range(first, last + 1)
        for row in rows:
            if (core[row][0] != sram[row][0] or sram[row][1] != sram[first][1] or
                    core[row][0] % 1000000):
                return groups, 'inconsistent performance row %d' % row
        if first and core[first][1] <= core[first - 1][1]:
            return groups, 'unordered voltage steps'
        high = max(rows, key=lambda row: core[row][0])
        groups.append({'mhz': core[high][0] // 1000000, 'mv': core[first][1]})
        first = last + 1
    if len(groups) not in (10, 11):
        return groups, 'unsupported voltage step count %d' % len(groups)
    return groups, None


def predicted_power(mhz, mv, cores, higher_ma):
    """Top-state power if the current clamp applies regardless of leakage."""
    maximum = 37000.0 if higher_ma else 24800.0
    voltage = f32(mv / 1000.0)
    dynamic = (voltage ** 1.2) * 18.2 * mhz * (cores / 10.0)
    # Leakage current only adds, so a dynamic current at the clamp fixes it.
    # Keep a margin for single-precision powf differences.
    if dynamic < maximum * 1.001:
        return None
    return int(f32(voltage * f32(maximum)))


def evaluate(node, cores, gate_max_power=QUALIFIED['max_power_mw'], exact_power=True):
    count = u32(node, 'perf-state-count')
    tables = u32(node, 'perf-state-table-count')
    report = {'cores': cores, 'boot_abort': None, 'gpu_failures': [], 'unknown': []}
    if tables != 1 or not count or count > MAX_PSTATES:
        report['boot_abort'] = 'unsupported performance table geometry'
        return report
    groups, abort = group(states(node, 'perf-states', count),
                          states(node, 'perf-states-sram', count))
    report['groups'] = len(groups)
    if abort:
        report['boot_abort'] = abort
        return report
    top = groups[-1]
    report.update(top_mhz=top['mhz'], top_mv=top['mv'])
    if cores not in (8, 10):
        report['boot_abort'] = 'unsupported GPU core count %d' % cores
        return report
    higher_ma = bool(u32(node, 'gpu-supports-higher-mA', 0))
    power = predicted_power(top['mhz'], top['mv'], cores, higher_ma)
    report['max_power_mw'] = power

    if cores != 10:
        report['gpu_failures'].append('core mask for %d cores is not 0x3ff' % cores)
    if len(groups) != QUALIFIED['groups']:
        report['gpu_failures'].append('%d voltage groups, module requires %d'
                                      % (len(groups), QUALIFIED['groups']))
    if top['mhz'] != QUALIFIED['top_mhz']:
        report['gpu_failures'].append('top frequency %d MHz, module requires %d'
                                      % (top['mhz'], QUALIFIED['top_mhz']))
    if power is None:
        report['unknown'].append('maximum power depends on the leakage fuse')
    elif power > gate_max_power or (exact_power and power != gate_max_power):
        report['gpu_failures'].append('maximum power %d mW, module requires %s%d'
                                      % (power, '' if exact_power else 'at most ',
                                         gate_max_power))
    return report


def read_macos():
    raw = subprocess.run(['/usr/sbin/ioreg', '-p', 'IODeviceTree', '-n', 'sgx', '-r', '-a', '-d', '1'],
                         capture_output=True, check=True).stdout
    nodes = plistlib.loads(raw)
    if not isinstance(nodes, list) or len(nodes) != 1:
        raise ValueError('expected one sgx device-tree node')
    profile = subprocess.run(['/usr/sbin/system_profiler', 'SPDisplaysDataType'],
                             capture_output=True, text=True, check=True).stdout
    cores = re.findall(r'Total Number of Cores:\s*(\d+)', profile)
    if len(cores) != 1:
        raise ValueError('cannot determine the GPU core count')
    return nodes[0], int(cores[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ioreg-plist', help='saved `ioreg -p IODeviceTree -n sgx -r -a -d 1` output')
    parser.add_argument('--cores', type=int, help='GPU core count (with --ioreg-plist)')
    args = parser.parse_args()
    try:
        if args.ioreg_plist:
            if args.cores is None:
                parser.error('--cores is required with --ioreg-plist')
            with open(args.ioreg_plist, 'rb') as stream:
                node, cores = plistlib.load(stream)[0], args.cores
        else:
            node, cores = read_macos()
        report = evaluate(node, cores)
    except (OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError,
            plistlib.InvalidFileException) as error:
        print(json.dumps({'error': str(error)}), file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    if report['boot_abort']:
        return 4
    if report['gpu_failures']:
        return 3
    return 2 if report['unknown'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
