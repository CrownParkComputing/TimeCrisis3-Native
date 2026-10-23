#!/usr/bin/env python3
"""Read real guest counters/camera state without stopping or modifying TC3.

Linux development diagnostic. Reads only named host globals and bounded known
TC3 EE fields from /proc/PID/mem. Camera reads are best-effort observations,
not atomic savestates. No input stream or stage completion is fabricated.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time

from check_replay import measure

ROOT = Path(__file__).resolve().parents[1]
RAM_BYTES = 128 * 1024**2


class Reader:
    def __init__(self, pid, binary, symbols):
        if Path(f'/proc/{pid}/exe').resolve() != binary.resolve():
            raise ValueError('Worker is not running the current TC3 binary')
        elf = binary.open('rb')
        try:
            header = elf.read(18)
        finally:
            elf.close()
        if header[:6] != b'\x7fELF\x02\x01':
            raise ValueError('Only little-endian ELF64 development binaries are supported')
        base = 0
        if struct.unpack_from('<H', header, 16)[0] == 3:
            mappings = []
            for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
                parts = line.split(maxsplit=5)
                if len(parts) == 6 and parts[5] == str(binary.resolve()):
                    mappings.append(int(parts[0].split('-')[0], 16) - int(parts[2], 16))
            if not mappings:
                raise ValueError('Executable load address was not found')
            base = min(mappings)
        self.fields_address = base + symbols['g_FrameCount']
        self.ram_pointer_address = base + symbols['eeMem']
        self.descriptor = os.open(f'/proc/{pid}/mem', os.O_RDONLY | os.O_CLOEXEC)

    def read(self, address, size):
        value = os.pread(self.descriptor, size, address)
        if len(value) != size:
            raise OSError('Incomplete process-memory read')
        return value

    def fields(self):
        return struct.unpack('<I', self.read(self.fields_address, 4))[0]

    def camera(self):
        ram = struct.unpack('<Q', self.read(self.ram_pointer_address, 8))[0]
        if not ram:
            return None
        def read(fmt, offset):
            size = struct.calcsize(fmt)
            if offset < 0 or offset > RAM_BYTES - size:
                raise ValueError('Camera pointer outside EE RAM')
            return struct.unpack(fmt, self.read(ram + offset, size))
        def u32(offset):
            return read('<I', offset)[0]
        try:
            root = u32(0x3798ac)
            if not root:
                return None
            obj = root + 0x320
            pose, state = u32(obj+0x18), u32(obj+0x1c)
            if not pose or not state:
                return None
            result = {'root': root, 'pose_pointer': pose, 'state_pointer': state,
                'position': read('<3i', pose), 'angles': read('<3h', pose+0x0c),
                'resource_ids': [u32(state+0x88), u32(state+0x8c)],
                'camera_time': read('<i', state+0x70)[0], 'view_index': u32(0x379c90)}
            if u32(0x3798ac) != root or u32(obj+0x18) != pose or u32(obj+0x1c) != state:
                return None
            return result
        except (ValueError, OSError):
            return None

    def close(self):
        os.close(self.descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--interval', type=float, default=.1)
    parser.add_argument('--camera', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / '.local/guest-progress.json')
    args = parser.parse_args()
    if not 0 < args.seconds <= 3600 or not .02 <= args.interval <= .1:
        parser.error('Use 0–3600 seconds and a 20–100 ms sampling interval')
    pair = json.loads((ROOT / '.local/cabinet-pair-run.json').read_text())
    binary = ROOT / '.local/core-build/bin/tc3-native'
    symbols = {}
    for line in subprocess.check_output(['nm', '-n', str(binary)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in ('g_FrameCount', 'eeMem'):
            symbols[fields[2]] = int(fields[0], 16)
    if len(symbols) != 2:
        parser.error('Required diagnostic symbols are missing from this build')
    readers = {}
    report = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'scenario': 'manual-linked-gameplay-observation', 'completed': False,
        'completed_milestones': [], 'workers': {},
        'scope': 'Guest counters and best-effort camera reads; no deterministic input/state identity or stage-completion proof'}
    try:
        for role, worker in pair['workers'].items():
            readers[role] = Reader(worker['pid'], binary, symbols)
            report['workers'][role] = {'pid': worker['pid'], 'samples': []}
        start = time.monotonic_ns()
        next_sample = time.monotonic()
        while True:
            for role, reader in readers.items():
                sample = {'time_ns': time.monotonic_ns() - start, 'guest_fields': reader.fields()}
                if args.camera:
                    sample['camera'] = reader.camera()
                report['workers'][role]['samples'].append(sample)
            if (time.monotonic_ns() - start) / 1e9 >= args.seconds:
                break
            next_sample += args.interval
            time.sleep(max(0, next_sample-time.monotonic()))
    except (OSError, ValueError) as error:
        report['collection_error'] = str(error)
    finally:
        for reader in readers.values():
            reader.close()
        for worker in report['workers'].values():
            if len(worker['samples']) >= 2:
                worker['measurement'] = measure(worker)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'output': str(args.output), 'collection_error': report.get('collection_error'),
        'measurements': {r: w.get('measurement') for r, w in report['workers'].items()},
        'release_gate_passed': False, 'scope': report['scope']}, indent=2))
    return 1 if report.get('collection_error') else 0


if __name__ == '__main__':
    raise SystemExit(main())
