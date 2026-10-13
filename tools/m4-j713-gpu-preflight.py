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


class BootAbort(Exception):
    """A table on which m1n1 stage 2 bails out of the whole boot."""


def u32(node, name, reason, default=None):
    value = node.get(name)
    if value is None and default is not None:
        return default
    if not isinstance(value, bytes) or len(value) != 4:
        raise BootAbort(reason)
    return struct.unpack('<I', value)[0]


def states(node, name, count):
    value = node.get(name)
    if not isinstance(value, bytes) or len(value) != count * 8:
        raise BootAbort('incomplete performance tables')
    return [struct.unpack_from('<II', value, offset) for offset in range(0, len(value), 8)]


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


def derive(node, cores):
    """Walk dt_set_gpu_t8132_perf in m1n1's order; raise BootAbort at each bail.

    Returns the voltage groups ({'mhz', 'mv'}), the top-state power (None when
    it depends on the leakage fuse) and what could not be decided from macOS.
    """
    count = u32(node, 'perf-state-count', 'unsupported performance table geometry')
    tables = u32(node, 'perf-state-table-count', 'unsupported performance table geometry')
    if tables != 1 or not count or count > MAX_PSTATES:
        raise BootAbort('unsupported performance table geometry')
    core = states(node, 'perf-states', count)
    sram = states(node, 'perf-states-sram', count)

    groups = []
    first = 0
    while first < count:
        last = first
        while last + 1 < count and core[last + 1][1] == core[first][1]:
            last += 1
        rows = range(first, last + 1)
        for row in rows:
            if (core[row][0] != sram[row][0] or sram[row][1] != sram[first][1] or
                    core[row][0] % 1000000):
                raise BootAbort('inconsistent performance row %d' % row)
        if first and core[first][1] <= core[first - 1][1]:
            raise BootAbort('unordered voltage steps')
        high = max(rows, key=lambda row: core[row][0])
        groups.append({'mhz': core[high][0] // 1000000, 'mv': core[first][1]})
        first = last + 1
    if len(groups) not in (10, 11):
        raise BootAbort('unsupported voltage step count %d' % len(groups))

    if cores not in (8, 10):
        raise BootAbort('unsupported GPU core count %d' % cores)
    base = u32(node, 'gpu-perf-base-pstate', 'invalid base performance state', 1)
    higher_ma = bool(u32(node, 'gpu-supports-higher-mA', 'invalid current-limit selector', 0))
    # m4_power_mw: the core count was checked above and the leakage fuse
    # field always decodes to 2..16384, so only the voltage can be rejected.
    for item in groups:
        if not item['mv'] or item['mv'] > 2000:
            raise BootAbort('invalid power inputs (%d mV)' % item['mv'])

    # m4_relative_tables. A zero frequency gives zero power, so
    # "power[0] || frequency[0]" reduces to frequency[0]. Voltage rises
    # strictly between groups, so power cannot fall where frequency rises.
    unknown = []
    mhz = [item['mhz'] for item in groups]
    top = groups[-1]
    if not base or base >= len(groups):
        raise BootAbort('invalid transfer tables (base performance state %d)' % base)
    if mhz[0]:
        raise BootAbort('invalid transfer tables (lowest frequency %d MHz)' % mhz[0])
    power = predicted_power(top['mhz'], top['mv'], cores, higher_ma)
    if power is None:
        unknown.append('maximum power depends on the leakage fuse')
        voltage = f32(top['mv'] / 1000.0)
        if voltage * (voltage ** 1.2) * 18.2 * top['mhz'] * (cores / 10.0) < 1.001:
            unknown.append('whether the top power is non-zero depends on the leakage fuse')
    elif not power:
        raise BootAbort('invalid transfer tables (zero top power)')
    if base < len(groups) - 1 and mhz[-1] <= mhz[base]:
        raise BootAbort('invalid transfer tables (top frequency not above the base state)')
    for i in range(1, len(mhz)):
        if mhz[i] <= mhz[i - 1]:
            raise BootAbort('invalid transfer tables (unordered frequency at group %d)' % i)
    return groups, power, unknown


def evaluate(node, cores, gate_max_power=QUALIFIED['max_power_mw'], exact_power=True):
    report = {'cores': cores, 'boot_abort': None, 'gpu_failures': [], 'unknown': [],
              'groups': None, 'top_mhz': None, 'top_mv': None, 'max_power_mw': None}
    try:
        groups, power, report['unknown'] = derive(node, cores)
    except BootAbort as error:
        report['boot_abort'] = str(error)
        return report
    top = groups[-1]
    report.update(groups=len(groups), top_mhz=top['mhz'], top_mv=top['mv'], max_power_mw=power)

    if cores != 10:
        report['gpu_failures'].append('core mask for %d cores is not 0x3ff' % cores)
    if len(groups) != QUALIFIED['groups']:
        report['gpu_failures'].append('%d voltage groups, module requires %d'
                                      % (len(groups), QUALIFIED['groups']))
    if top['mhz'] != QUALIFIED['top_mhz']:
        report['gpu_failures'].append('top frequency %d MHz, module requires %d'
                                      % (top['mhz'], QUALIFIED['top_mhz']))
    if power is not None and (power > gate_max_power or
                              (exact_power and power != gate_max_power)):
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
