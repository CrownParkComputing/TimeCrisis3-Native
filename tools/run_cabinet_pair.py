#!/usr/bin/env python3
"""Run two isolated, linked TC3 cabinets with their genuine arcade views.

The current Linux presentation uses two adjacent windows. Both cabinets share
pause/quit through the Nuklear menu. Full stage validation remains outstanding.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time

from prepare_playtest import prepare

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int)
    parser.add_argument('--background', action='store_true')
    args = parser.parse_args()
    if args.seconds is not None and not 1 <= args.seconds <= 86400:
        parser.error('--seconds must be between 1 and 86400')
    binary = ROOT / '.local/core-build/bin/tc3-native'
    if not binary.is_file():
        parser.error('Build the playable target first')
    pidfile = ROOT / '.local/cabinet-supervisor.pid'
    if pidfile.exists():
        try:
            previous = int(pidfile.read_text())
            command = Path(f'/proc/{previous}/cmdline').read_bytes().split(b'\0')
            if os.fsencode(Path(__file__).resolve()) in command:
                parser.error(f'The cabinet pair is already running (supervisor PID {previous})')
        except (OSError, ValueError):
            pass
    if args.background:
        command = ['python3', str(Path(__file__).resolve())]
        if args.seconds is not None:
            command += ['--seconds', str(args.seconds)]
        with (ROOT / '.local/cabinet-supervisor.log').open('w') as output:
            child = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        print(f'TC3 twin-cabinet supervisor PID: {child.pid}', flush=True)
        return 0
    pidfile.write_text(str(os.getpid()) + '\n')
    roots = {role: ROOT / f'.local/cabinet-{role}' for role in ('left', 'right')}
    for root in roots.values():
        prepare(ROOT / '.local/playtest', root, with_state=False)
    directory = Path(tempfile.mkdtemp(prefix='tc3-link-'))
    (directory / 'control.bin').write_bytes(struct.pack('<5I', 0x54433301, 0, 0, 0, 0))
    report = {'session': str(directory), 'workers': {}, 'linked_gameplay_validated': False}
    report_path = ROOT / '.local/cabinet-pair-run.json'
    workers = []
    outputs = []
    try:
        for role, root in roots.items():
            env = dict(os.environ, TC3_CABINET_ROLE=role, TC3_CABINET_DIR=str(directory))
            env.setdefault('SDL_VIDEO_DRIVER', 'x11')
            log = ROOT / f'.local/cabinet-{role}.log'
            output = log.open('w'); outputs.append(output)
            command = [str(binary), '--data-root', str(root),
                '--boot-manifest', str(root / 'games/NM00012.acgame')]
            if args.seconds is not None:
                command += ['--seconds', str(args.seconds)]
            child = subprocess.Popen(command, cwd=ROOT, env=env,
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
            workers.append(child)
            report['workers'][role] = {'pid': child.pid, 'log': str(log), 'data_root': str(root)}
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2), flush=True)
        until = time.monotonic() + args.seconds + 30 if args.seconds else float('inf')
        laid_out = set()
        while time.monotonic() < until:
            if any(child.poll() is not None for child in workers):
                break
            # Hyprland ignores SDL's requested positions for tiled windows.
            # Change only our worker windows, and leave other applications alone.
            if len(laid_out) < 2:
                try:
                    monitors = json.loads(subprocess.check_output(['hyprctl', 'monitors', '-j'], text=True))
                    monitor = next((m for m in monitors if m.get('focused')), monitors[0])
                    scale = monitor.get('scale', 1)
                    width, height = monitor['width'] / scale, monitor['height'] / scale
                    game_height = int(min(900, height - 100, (width - 40) * 3 / 8))
                    game_width = game_height * 4 // 3
                    start_x = monitor['x'] + int((width - game_width * 2 - 12) / 2)
                    start_y = monitor['y'] + int((height - game_height) / 2)
                    clients = json.loads(subprocess.check_output(['hyprctl', 'clients', '-j'], text=True))
                    for index, child in enumerate(workers):
                        client = next((c for c in clients if c['pid'] == child.pid), None)
                        if client and child.pid not in laid_out:
                            address = 'address:' + client['address']
                            for operation, value in (
                                ('setfloating', address),
                                ('resizewindowpixel', f'exact {game_width} {game_height},{address}'),
                                ('movewindowpixel', f'exact {start_x + index * (game_width + 12)} {start_y},{address}')):
                                subprocess.run(['hyprctl', 'dispatch', operation, value],
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                            laid_out.add(child.pid)
                except (OSError, ValueError, KeyError, IndexError, subprocess.CalledProcessError):
                    pass  # Other desktops use their normal window placement.
            time.sleep(.2)
    finally:
        for child in workers:
            if child.poll() is None:
                # SDL translates SIGINT into its normal quit event.
                child.send_signal(signal.SIGINT)
        for child in workers:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        print(f'TC3 worker {child.pid} did not exit after SIGKILL', flush=True)
        for output in outputs:
            output.close()
        for role, child in zip(roots, workers):
            report['workers'][role]['exit_code'] = child.returncode
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        try:
            if pidfile.read_text().strip() == str(os.getpid()):
                pidfile.unlink()
        except OSError:
            pass
        (directory / 'control.bin').unlink(missing_ok=True)
        # Retain unexpected files as evidence; only remove an empty session.
        try:
            directory.rmdir()
        except OSError:
            pass
    return 0 if len(workers) == 2 and all(child.returncode == 0 for child in workers) else 1


if __name__ == '__main__':
    raise SystemExit(main())
