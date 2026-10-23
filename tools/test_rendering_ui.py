#!/usr/bin/env python3
"""Exercise a fresh TC3 window's real rendering controls and restore its settings.

Requires the Linux desktop session (hyprctl/xdotool). --pid must name the
project's current tc3-native executable, freshly booted with its menu closed.
Start this diagnostic with tools/run_tc3.py --single (the default is now twin).
Verifies applied GS state, persisted settings, and paused-menu redraws. This
does not test DLSS/FSR, frame generation, linked cabinets or the stage-1 freeze.
"""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / '.local/tc3-live.log'
CONFIG = ROOT / '.local/playtest/PCSX2x6/inis/PCSX2.ini'


def run(*args):
    return subprocess.check_output(args, text=True)


def applied():
    matches = re.findall(r'\[TC3 rendering applied\] ([^\n]+)', LOG.read_text())
    if not matches:
        raise RuntimeError('No applied GS state in log')
    return {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', matches[-1])}


def wait_for(predicate, description, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.05)
    raise RuntimeError('Timed out: ' + description)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    args = parser.parse_args()
    binary = (ROOT / '.local/core-build/bin/tc3-native').resolve()
    if Path(f'/proc/{args.pid}/exe').resolve() != binary:
        parser.error('PID does not run this project executable')
    os.kill(args.pid, 0)
    client = next(c for c in json.loads(run('hyprctl', 'clients', '-j')) if c['pid'] == args.pid)
    address = client['address']
    before = applied()
    expected = dict(before)
    checks = []

    def keys(*values):
        run('hyprctl', 'dispatch', 'focuswindow', 'address:' + address)
        run('xdotool', 'key', '--clearmodifiers', *values)
        time.sleep(.12)

    def check_change(key, value, *values):
        keys(*values)
        expected[key] = value
        wait_for(lambda: all(applied().get(k) == v for k, v in expected.items()), key)
        checks.append({key: value})

    # Fresh process starts on File. F10 pauses and Tab selects the top tabs.
    keys('F10', 'Tab', 'Tab', 'Down')
    try:
        check_change('vsync', 1 - before['vsync'], 'Return')
        check_change('vsync', before['vsync'], 'Return')
        direction = 'Right' if before['queue'] < 3 else 'Left'
        queue = before['queue'] + (1 if direction == 'Right' else -1)
        check_change('queue', queue, 'Down', direction)
        check_change('queue', before['queue'], 'Left' if direction == 'Right' else 'Right')
        keys('Tab', 'Down')
        direction = 'Right' if before['scale'] < 4 else 'Left'
        scale = before['scale'] + (1 if direction == 'Right' else -1)
        check_change('scale', scale, direction)
        check_change('scale', before['scale'], 'Left' if direction == 'Right' else 'Right')
        check_change('fxaa', 1 - before['fxaa'], 'Down', 'Return')
        check_change('fxaa', before['fxaa'], 'Return')
        keys('Down')
        if before['cas_supported']:
            # Choose a nonzero mode so the sharpness control is available.
            mode = (before['cas'] + 1) % 3
            check_change('cas', mode, 'Right')
            if mode == 0:
                mode = 1
                check_change('cas', mode, 'Right')
            sharp_key = 'Right' if before['sharpness'] <= 90 else 'Left'
            sharpness = before['sharpness'] + (10 if sharp_key == 'Right' else -10)
            check_change('sharpness', sharpness, 'Down', sharp_key)
            check_change('sharpness', before['sharpness'], 'Left' if sharp_key == 'Right' else 'Right')
            keys('Up')
            while mode != before['cas']:
                mode = (mode + 1) % 3
                check_change('cas', mode, 'Right')
        # More than a second paused: idle presents increase but guest execution stops.
        def sample():
            matches = re.findall(r'guest_fields=(\d+) present_calls=(\d+).*?state=(\d+)', LOG.read_text())
            return tuple(map(int, matches[-1]))
        time.sleep(1.2)
        first = sample()
        wait_for(lambda: sample()[1] > first[1], 'paused menu redraw', timeout=3)
        last = sample()
        if first[2] != 3 or last[2] != 3 or first[0] != last[0]:
            raise RuntimeError(f'Guest advanced during pause: {first}, {last}')
        checks.append({'paused_guest_fields': [first[0], last[0]],
                       'paused_presents': [first[1], last[1]]})
        ini = configparser.ConfigParser(interpolation=None)
        ini.read(CONFIG)
        gs = ini['EmuCore/GS']
        assert int(float(gs['upscale_multiplier'])) == before['scale']
        assert gs.getboolean('fxaa') == bool(before['fxaa'])
        assert int(gs['CASMode']) == before['cas']
        assert int(gs['CASSharpness']) == before['sharpness']
        assert gs.getboolean('VsyncEnable') == bool(before['vsync'])
        assert int(gs['VsyncQueueSize']) == before['queue']
        assert not ini.getboolean('EmuCore', 'EnablePatches')
        assert gs.getint('OsdMessagesPos') == 0 and gs.getint('OsdPerformancePos') == 0
        report = {'pid': args.pid, 'applied_before': before, 'applied_after': applied(),
                  'checks': checks, 'settings_restored': True, 'no_patches_or_core_osd': True}
        (ROOT / '.local/rendering-ui-validation.json').write_text(json.dumps(report, indent=2) + '\n')
        print('PASS live GS setting changes, persistence, restored values and paused menu redraw')
    finally:
        # Resume even when a check fails. The log records any un-restored change.
        keys('Escape')


if __name__ == '__main__':
    main()
