#!/usr/bin/env python3
"""Live regression for a fresh cabinet pair: shared menu pause/rendering/quit.

Requires its desktop session and a freshly launched pair with File selected and
menus closed. Changes are restored. --quit additionally checks a normal menu
quit shuts down both workers. This does not prove full-stage gameplay.
"""
import argparse
import configparser
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_for(check, description, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.05)
    raise RuntimeError('Timed out: ' + description)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quit', action='store_true')
    args = parser.parse_args()
    pair = json.loads((ROOT / '.local/cabinet-pair-run.json').read_text())
    workers = pair['workers']
    binary = (ROOT / '.local/core-build/bin/tc3-native').resolve()
    for worker in workers.values():
        if Path(f"/proc/{worker['pid']}/exe").resolve() != binary:
            parser.error('Pair workers are not running the current executable')

    def log(role):
        return Path(workers[role]['log']).read_text()

    def applied(role):
        lines = re.findall(r'\[TC3 rendering applied\] ([^\n]+)', log(role))
        return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[-1])} if lines else {}

    def sample(role):
        matches = re.findall(r'guest_fields=(\d+) present_calls=(\d+).*?state=(\d+)', log(role))
        return tuple(map(int, matches[-1])) if matches else (0, 0, 0)

    def clients():
        return json.loads(subprocess.check_output(['hyprctl', 'clients', '-j'], text=True))

    def keys(*values):
        client = next(c for c in clients() if c['pid'] == workers['left']['pid'])
        subprocess.run(['hyprctl', 'dispatch', 'focuswindow', 'address:' + client['address']],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['xdotool', 'key', '--clearmodifiers', *values], check=True)
        time.sleep(.12)

    wait_for(lambda: all('Original socket 1 connected' in log(role) for role in workers),
             'both original peer sockets connected', timeout=45)
    before = applied('left')
    assert before == applied('right'), 'Start from matching cabinet rendering settings'
    expected = dict(before)
    checks = []

    def change(key, value, *values):
        keys(*values)
        expected[key] = value
        wait_for(lambda: all(all(applied(role).get(k) == v for k, v in expected.items())
                            for role in workers), f'{key} applied on both renderers')
        checks.append({key: value, 'both_cabinets': True})

    keys('F10')
    wait_for(lambda: all(sample(role)[2] == 3 for role in workers), 'both cabinets paused')
    time.sleep(1.2)
    paused = {role: sample(role) for role in workers}
    wait_for(lambda: all(sample(role)[1] > paused[role][1] for role in workers), 'both paused menus redraw')
    assert all(sample(role)[0] == paused[role][0] for role in workers), 'Guest advanced while paused'
    try:
        keys('Tab', 'Tab', 'Tab', 'Down')
        direction = 'Right' if before['scale'] < 4 else 'Left'
        change('scale', before['scale'] + (1 if direction == 'Right' else -1), direction)
        change('scale', before['scale'], 'Left' if direction == 'Right' else 'Right')
        change('fxaa', 1 - before['fxaa'], 'Down', 'Return')
        change('fxaa', before['fxaa'], 'Return')
        keys('Down')
        if before['cas_supported']:
            mode = (before['cas'] + 1) % 3
            change('cas', mode, 'Right')
            if mode == 0:
                mode = 1
                change('cas', mode, 'Right')
            direction = 'Right' if before['sharpness'] <= 90 else 'Left'
            change('sharpness', before['sharpness'] + (10 if direction == 'Right' else -10), 'Down', direction)
            change('sharpness', before['sharpness'], 'Left' if direction == 'Right' else 'Right')
            keys('Up')
            while mode != before['cas']:
                mode = (mode + 1) % 3
                change('cas', mode, 'Right')
        keys('Tab', 'Tab', 'Tab', 'Tab', 'Down')  # Rendering -> Cabinet -> File -> Display -> Performance
        change('vsync', 1 - before['vsync'], 'Return')
        change('vsync', before['vsync'], 'Return')
        direction = 'Right' if before['queue'] < 3 else 'Left'
        change('queue', before['queue'] + (1 if direction == 'Right' else -1), 'Down', direction)
        change('queue', before['queue'], 'Left' if direction == 'Right' else 'Right')
        for worker in workers.values():
            config = configparser.ConfigParser(interpolation=None)
            config.read(Path(worker['data_root']) / 'PCSX2x6/inis/PCSX2.ini')
            gs = config['EmuCore/GS']
            assert int(float(gs['upscale_multiplier'])) == before['scale']
            assert gs.getboolean('fxaa') == bool(before['fxaa'])
            assert int(gs['CASMode']) == before['cas']
            assert int(gs['CASSharpness']) == before['sharpness']
            assert gs.getboolean('VsyncEnable') == bool(before['vsync'])
            assert int(gs['VsyncQueueSize']) == before['queue']
        keys('Escape')
        wait_for(lambda: all(sample(role)[2] == 2 and sample(role)[0] > paused[role][0]
                            for role in workers), 'both cabinets resumed')
        if args.quit:
            keys('F10', 'Tab', 'Tab', 'Tab', 'Down', 'Down', 'Return')  # Performance -> Rendering -> Cabinet -> File/Quit
            wait_for(lambda: all('[TC3] VM: Stopped' in log(role) for role in workers), 'both cabinets quit')
        report = {'workers': workers, 'checks': checks, 'shared_pause_resume': True,
                  'pause_samples': paused, 'settings_restored': True, 'shared_quit': args.quit}
        (ROOT / '.local/cabinet-ui-validation.json').write_text(json.dumps(report, indent=2) + '\n')
        print('PASS live shared pause/resume, both renderers, persistence' + (' and shared menu quit' if args.quit else ''))
    except Exception:
        keys('Escape')
        raise


if __name__ == '__main__':
    main()
