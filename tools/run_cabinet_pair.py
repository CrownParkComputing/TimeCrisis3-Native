#!/usr/bin/env python3
"""Run two isolated, linked cabinets with their genuine arcade views.

The current Linux presentation uses two adjacent windows. Both cabinets share
pause/quit through the Nuklear menu. Full stage validation remains outstanding.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import struct
import subprocess
import tempfile
import time

from prepare_playtest import prepare

ROOT = Path(__file__).resolve().parents[1]


def clone_playtest(source, destination):
    """Clone a prepared playtest data-root for one cabinet role.

    Bulk media (CHD/ISO) is hardlinked to keep the second instance cheap; all
    mutable state (SRAM, memcards, INIs, NVRAM) is copied.
    """
    source = Path(source).resolve()
    destination = Path(destination).resolve()
    if not source.is_dir():
        raise ValueError(f'Playtest source does not exist: {source}')
    if destination == source or destination.is_relative_to(source) or \
            source.is_relative_to(destination):
        raise ValueError('Source and role storage must be separate')
    for part in (destination, *destination.parents):
        if part.is_symlink():
            raise ValueError(f'Destination ancestor is a symlink: {part}')
    media_suffixes = {'.chd', '.iso', '.img', '.bin.gz'}
    for src in sorted(source.rglob('*')):
        rel = src.relative_to(source)
        dst = destination / rel
        if src.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.exists() or dst.is_symlink():
            dst.unlink()
        if any(str(src).endswith(s) for s in media_suffixes) or \
                (src.is_file() and src.stat().st_size > 64 * 1024 * 1024):
            try:
                os.link(src, dst)
            except OSError:
                shutil.copyfile(src, dst)
        else:
            shutil.copyfile(src, dst)
            os.chmod(dst, 0o600)


# Mutable files that make the pair one machine: a change written by either
# cabinet is mirrored to its twin and back to the source playtest so it
# survives the next launch's fresh clone.
SYNC_FILES = ('games/sram.bin', 'games/nvram.bin', 'PCSX2x6/inis/PCSX2.ini')


def digest(path):
    try:
        return hashlib.sha256(path.read_bytes()).digest()
    except OSError:
        return None


def sync_pair_state(roots, source):
    """Propagate changed EEPROM/INI files between workers and the source.

    Newest stable writer wins; a file still being written (mtime < 0.75s)
    is left alone until it settles, so a half-written buffer never spreads.
    """
    for rel in SYNC_FILES:
        paths = {role: root / rel for role, root in roots.items()}
        digests = {role: digest(path) for role, path in paths.items()}
        if any(d is None for d in digests.values()):
            continue
        source_differs = source is not None and digest(source / rel) not in set(digests.values())
        if len(set(digests.values())) < 2 and not source_differs:
            continue
        try:
            mtimes = {role: paths[role].stat().st_mtime for role in paths}
        except OSError:
            continue
        now = time.time()
        newest = max(mtimes, key=mtimes.get)
        if now - mtimes[newest] < 0.75:
            continue
        for role, path in paths.items():
            if role != newest:
                shutil.copyfile(paths[newest], path)
                os.chmod(path, 0o600)
        if source is not None:
            dst = source / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(paths[newest], dst)
        print(f'TC3 twin-cabinet: synced {rel} from {newest} cabinet', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int)
    parser.add_argument('--background', action='store_true')
    parser.add_argument('--source', type=Path,
        help='Prepared playtest data-root to clone per role (e.g. a TC4 '
             '.local/playtest). Defaults to the TC3 reference assets.')
    parser.add_argument('--manifest', type=Path,
        help='Boot manifest. With --source this may be inside the cloned root '
             'or a bare filename under its games/ directory.')
    parser.add_argument('--state-root', type=Path,
        help='Directory for cabinet clones, logs, pidfile and run report. '
             'Defaults to this repo\'s .local (used by sibling game launchers '
             'that point at their own .local instead).')
    args = parser.parse_args()
    if args.seconds is not None and not 1 <= args.seconds <= 86400:
        parser.error('--seconds must be between 1 and 86400')
    if args.source is None and args.manifest is not None:
        args.manifest = args.manifest.resolve()
        if not args.manifest.is_file():
            parser.error(f'Boot manifest does not exist: {args.manifest}')
    binary = ROOT / '.local/core-build/bin/tc3-native'
    if not binary.is_file():
        parser.error('Build the playable target first')
    state = (args.state_root.expanduser().resolve() if args.state_root
             else ROOT / '.local')
    state.mkdir(parents=True, exist_ok=True)
    pidfile = state / 'cabinet-supervisor.pid'
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
        if args.source is not None:
            command += ['--source', str(args.source)]
        if args.manifest is not None:
            command += ['--manifest', str(args.manifest)]
        if args.state_root is not None:
            command += ['--state-root', str(args.state_root)]
        with (state / 'cabinet-supervisor.log').open('w') as output:
            child = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        print(f'TC3 twin-cabinet supervisor PID: {child.pid}', flush=True)
        return 0
    pidfile.write_text(str(os.getpid()) + '\n')
    roots = {role: state / f'cabinet-{role}' for role in ('left', 'right')}
    for root in roots.values():
        if args.source is not None:
            clone_playtest(args.source, root)
        else:
            prepare(ROOT / '.local/playtest', root, with_state=False)
    if args.manifest is not None:
        name = args.manifest.name
        manifest_rel = Path('games') / name
        for root in roots.values():
            if not (root / manifest_rel).is_file():
                parser.error(f'Cloned cabinet is missing {manifest_rel}')
        boot_manifest = manifest_rel
    else:
        boot_manifest = Path('games/NM00012.acgame')
    directory = Path(tempfile.mkdtemp(prefix='tc3-link-'))
    (directory / 'control.bin').write_bytes(struct.pack('<6I', 0x54433301, 0, 0, 0, 0, 0))
    report = {'session': str(directory), 'workers': {}, 'linked_gameplay_validated': False}
    report_path = state / 'cabinet-pair-run.json'
    workers = []
    outputs = []
    try:
        for role, root in roots.items():
            env = dict(os.environ, TC3_CABINET_ROLE=role, TC3_CABINET_DIR=str(directory))
            env['TC3_PAD_INDEX'] = '1' if role == 'right' else '0'
            env.setdefault('SDL_VIDEO_DRIVER', 'x11')
            # One machine, one sound output: only the left cabinet talks.
            if role == 'right':
                env['TC3_CABINET_MUTE'] = '1'
            log = state / f'cabinet-{role}.log'
            output = log.open('w'); outputs.append(output)
            command = [str(binary), '--data-root', str(root),
                '--boot-manifest', str(root / boot_manifest)]
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
            # The cabinet is one machine with two flush screens: fullscreen each
            # worker on its own monitor when there are two, otherwise tile both
            # as borderless halves of the focused monitor with no gap.
            if len(laid_out) < 2:
                try:
                    monitors = json.loads(subprocess.check_output(['hyprctl', 'monitors', '-j'], text=True))
                    monitor = next((m for m in monitors if m.get('focused')), monitors[0])
                    scale = monitor.get('scale', 1)
                    width, height = monitor['width'] / scale, monitor['height'] / scale
                    for title in ('Time Crisis', 'Ridge Racer'):
                        subprocess.run(['hyprctl', 'keyword', 'windowrule',
                                        f'border_size 0, match:title {title}'],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                    clients = json.loads(subprocess.check_output(['hyprctl', 'clients', '-j'], text=True))
                    for index, child in enumerate(workers):
                        client = next((c for c in clients if c['pid'] == child.pid), None)
                        if client and child.pid not in laid_out:
                            address = 'address:' + client['address']
                            if len(monitors) >= 2:
                                target = monitors[index] if index < len(monitors) else monitors[-1]
                                operations = (
                                    ('setfloating', address),
                                    ('movewindowpixel', f'exact {target["x"]} {target["y"]},{address}'),
                                    ('fullscreen', f'0,{address}'))
                            else:
                                half = int(width / 2)
                                operations = (
                                    ('setfloating', address),
                                    ('resizewindowpixel', f'exact {half} {int(height)},{address}'),
                                    ('movewindowpixel', f'exact {monitor["x"] + index * half} {monitor["y"]},{address}'))
                            for operation, value in operations:
                                subprocess.run(['hyprctl', 'dispatch', operation, value],
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                            laid_out.add(child.pid)
                except (OSError, ValueError, KeyError, IndexError, subprocess.CalledProcessError):
                    pass  # Other desktops use their normal window placement.
            sync_pair_state(roots, args.source)
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
        # Last writer wins once both cabinets have flushed and exited. Wait
        # for the mutable files to stop changing so the workers' exit-time
        # writes are not skipped by the freshness window.
        deadline = time.monotonic() + 5
        stamps = None
        while time.monotonic() < deadline:
            current = tuple(path.stat().st_mtime_ns for root in roots.values()
                            for rel in SYNC_FILES if (path := root / rel).exists())
            if current and current == stamps:
                break
            stamps = current
            time.sleep(.5)
        sync_pair_state(roots, args.source)
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
