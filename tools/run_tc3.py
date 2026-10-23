#!/usr/bin/env python3
"""Run the linked TC3 compatibility baseline with isolated calibrated data."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage1', action='store_true', help='Load the calibrated stage 1 starting state')
    p.add_argument('--seconds', type=int, help='Bounded playtest duration (whole seconds)')
    p.add_argument('--log', type=Path, help='Capture stdout/stderr to this file')
    p.add_argument('--background', action='store_true', help='Leave the test window running')
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--single', action='store_true', help='Run the single-cabinet baseline')
    mode.add_argument('--twin', action='store_true', help='Run the linked arcade pair (default)')
    args = p.parse_args()
    binary = ROOT / '.local/core-build/bin/tc3-native'
    private = ROOT / '.local/playtest'
    manifest = private / 'games/NM00012.acgame'
    if not binary.is_file() or not manifest.is_file():
        p.error('Build with tools/build_core.py --playable and prepare tools/prepare_playtest.py first')
    command = [str(binary), '--data-root', str(private), '--boot-manifest', str(manifest)]
    if args.stage1:
        p.error('The legacy stage-1 state omits arcade I/O state. Use normal boot; fresh-process state replay is not validated.')
    if args.seconds is not None:
        if not 1 <= args.seconds <= 86400:
            p.error('--seconds must be between 1 and 86400')
        command += ['--seconds', str(args.seconds)]
    if not args.single:
        if args.log:
            p.error('Twin workers use .local/cabinet-left.log and cabinet-right.log; use --single with --log')
        pair = ['python3', str(ROOT / 'tools/run_cabinet_pair.py')]
        if args.seconds is not None:
            pair += ['--seconds', str(args.seconds)]
        if args.background:
            pair.append('--background')
        return subprocess.run(pair, cwd=ROOT).returncode
    env = dict(os.environ)
    env.setdefault('SDL_VIDEO_DRIVER', 'x11')
    print('Starting TC3 compatibility baseline (native translation is still partial).', flush=True)
    if args.background:
        pidfile = ROOT / '.local/tc3-live.pid'
        if pidfile.exists():
            try:
                previous = int(pidfile.read_text())
                if Path(f'/proc/{previous}/exe').resolve() == binary.resolve():
                    p.error(f'TC3 is already running as PID {previous}')
            except (ValueError, OSError):
                pass
        log = args.log or ROOT / '.local/tc3-live.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('w') as output:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        pidfile.write_text(str(child.pid) + '\n')
        print(f'TC3 PID: {child.pid}; log: {log}')
        return 0
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        with args.log.open('w') as output:
            return subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT).returncode
    return subprocess.run(command, cwd=ROOT, env=env).returncode


if __name__ == '__main__':
    raise SystemExit(main())
