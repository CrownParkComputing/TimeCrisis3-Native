#!/usr/bin/env python3
"""Compare actual paused TC3 pixels while changing the paired rendering menu.

Run against a fresh pair with its menus closed. Temporarily normalizes rendering
for pixel comparisons and restores the user's settings. Pillow only reads pixels.
Checks reversible effects, unchanged guest progress for post-processing, and
bounded scene refresh for internal resolution. Restores settings and resumes.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def wait_for(predicate, description, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.05)
    raise RuntimeError('Timed out: ' + description)


def main():
    pair = json.loads((ROOT / '.local/cabinet-pair-run.json').read_text())
    workers = pair['workers']
    evidence = ROOT / '.local/paused-preview'
    evidence.mkdir(exist_ok=True)
    binary = (ROOT / '.local/core-build/bin/tc3-native').resolve()
    for worker in workers.values():
        assert Path(f"/proc/{worker['pid']}/exe").resolve() == binary

    def log(role):
        return Path(workers[role]['log']).read_text()

    def sample(role):
        matches = re.findall(r'guest_fields=(\d+) present_calls=(\d+).*?state=(\d+)', log(role))
        return tuple(map(int, matches[-1])) if matches else (0, 0, 0)

    def applied(role):
        lines = re.findall(r'\[TC3 rendering applied\] ([^\n]+)', log(role))
        return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[-1])} if lines else {}

    def client(role):
        clients = json.loads(subprocess.check_output(['hyprctl', 'clients', '-j'], text=True))
        return next(c for c in clients if c['pid'] == workers[role]['pid'])

    def keys(*values):
        subprocess.run(['hyprctl', 'dispatch', 'focuswindow', 'address:' + client('left')['address']],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['xdotool', 'key', '--clearmodifiers', *values], check=True)
        time.sleep(.15)

    def change(key, value, *values):
        keys(*values)
        wait_for(lambda: all(applied(role).get(key) == value for role in workers), f'{key} on both cabinets')
        if key == 'scale':
            wait_for(lambda: all(sample(role)[2] == 3 for role in workers), 'resolution preview paused again')
        time.sleep(.2)

    def pixels(name):
        # The right view has no menu or mouse, so UI highlights cannot fake an
        # image change. No image editing or synthetic image generation is used.
        c = client('right')
        x, y = c['at']; width, height = c['size']
        path = evidence / (name + '.png')
        subprocess.run(['grim', '-g', f'{x},{y} {width}x{height}', str(path)], check=True)
        with Image.open(path) as image:
            data = image.convert('RGB').tobytes()
        return data, hashlib.sha256(data).hexdigest()

    wait_for(lambda: all('Original socket 1 connected' in log(role) for role in workers),
             'linked scene available', timeout=50)
    original = applied('left')
    assert original == applied('right'), 'Start from matching cabinet settings'
    keys('F10', 'Tab', 'Tab', 'Tab', 'Down')
    wait_for(lambda: all(sample(role)[2] == 3 for role in workers), 'both paused')
    try:
        while applied('left')['scale'] > 1:
            change('scale', applied('left')['scale']-1, 'Left')
        keys('Down')
        if applied('left')['fxaa']:
            change('fxaa', 0, 'Return')
        keys('Down')
        while applied('left')['cas']:
            change('cas', (applied('left')['cas']+1)%3, 'Right')
        keys('Up')
        time.sleep(1.2)
        initial = {role: sample(role)[0] for role in workers}
        baseline, baseline_hash = pixels('baseline')
        checks = {'baseline_sha256': baseline_hash, 'original_settings': original}
        change('fxaa', 1, 'Return')
        fxaa, checks['fxaa_sha256'] = pixels('fxaa-on')
        assert fxaa != baseline, 'FXAA did not change the paused image'
        change('fxaa', 0, 'Return')
        restored, checks['fxaa_restored_sha256'] = pixels('fxaa-off')
        assert restored == baseline, 'FXAA off did not recover the clean frame'
        change('cas', 1, 'Down', 'Right')
        cas, checks['cas_sha256'] = pixels('cas-on')
        assert cas != baseline, 'CAS did not change the paused image'
        original_sharpness = applied('left')['sharpness']
        sharp_key = 'Right' if original_sharpness <= 90 else 'Left'
        test_sharpness = original_sharpness + (10 if sharp_key == 'Right' else -10)
        change('sharpness', test_sharpness, 'Down', sharp_key)
        sharp, checks['sharpness_sha256'] = pixels('cas-sharpness-adjusted')
        assert sharp != cas, 'Sharpness did not change the paused image'
        change('sharpness', original_sharpness, 'Left' if sharp_key == 'Right' else 'Right')
        cas_restored, _ = pixels('cas-sharpness-restored')
        assert cas_restored == cas, 'Sharpness changes accumulated instead of restoring the source'
        change('cas', 0, 'Up', 'Left')
        restored, checks['cas_restored_sha256'] = pixels('cas-off')
        assert restored == baseline, 'CAS off did not recover the clean frame'
        assert all(sample(role)[0] == initial[role] and sample(role)[2] == 3 for role in workers), \
            'Post-processing advanced gameplay'

        change('scale', 2, 'Up', 'Up', 'Right')
        wait_for(lambda: all(sample(role)[2] == 3 and sample(role)[0] == initial[role]+2
                            for role in workers), 'two-field resolution refresh')
        resolution, checks['resolution_sha256'] = pixels('resolution-2x')
        assert resolution != baseline, 'Internal resolution did not refresh the paused scene'
        change('scale', 1, 'Left')
        wait_for(lambda: all(sample(role)[2] == 3 and sample(role)[0] == initial[role]+4
                            for role in workers), 'native resolution refresh')
        fields = {role: sample(role)[0] for role in workers}
        time.sleep(1.2)
        assert all(sample(role)[0] == fields[role] for role in workers), 'Resolution preview kept running'
        checks['guest_fields_initial'] = initial
        checks['guest_fields_final'] = fields
        checks['post_processing_does_not_advance_game'] = True
        checks['resolution_exactly_two_fields_per_change'] = True
        checks['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        (evidence / 'validation.json').write_text(json.dumps(checks, indent=2) + '\n')
        print('PASS actual paused pixels: FXAA, CAS, sharpness, reversibility and bounded resolution refresh')
    finally:
        try:
            keys('Home')
            while applied('left')['scale'] != original['scale']:
                current = applied('left')['scale']; step = 1 if current < original['scale'] else -1
                change('scale', current+step, 'Right' if step > 0 else 'Left')
            keys('Down')
            if applied('left')['fxaa'] != original['fxaa']:
                change('fxaa', original['fxaa'], 'Return')
            keys('Down')
            while applied('left')['cas'] != original['cas']:
                change('cas', (applied('left')['cas']+1)%3, 'Right')
            keys('Down')
            while original['cas'] and applied('left')['sharpness'] != original['sharpness']:
                current = applied('left')['sharpness']; step = 1 if current < original['sharpness'] else -1
                change('sharpness', current+step*10, 'Right' if step > 0 else 'Left')
        finally:
            keys('Escape')
        path = evidence / 'validation.json'
        if path.exists():
            result = json.loads(path.read_text())
            result['settings_restored'] = all(applied(role) == original for role in workers)
            path.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
