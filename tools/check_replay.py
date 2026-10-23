#!/usr/bin/env python3
"""Check captured guest progress; generated/presented FPS cannot satisfy this gate."""
import argparse
import json
from pathlib import Path


def measure(trace):
    samples = trace['samples']
    if len(samples) < 2:
        raise ValueError('Need at least two progress samples')
    for sample in samples:
        for key in ('time_ns', 'guest_fields'):
            if type(sample[key]) is not int or sample[key] < 0:
                raise ValueError(f'{key} must be a nonnegative integer')
    previous = samples[0]
    last_progress = previous['time_ns']
    max_stall = 0
    for sample in samples[1:]:
        if sample['time_ns'] <= previous['time_ns']:
            raise ValueError('Capture timestamps must increase')
        if sample['guest_fields'] < previous['guest_fields']:
            raise ValueError('Guest field counter moved backwards')
        if sample['guest_fields'] > previous['guest_fields']:
            max_stall = max(max_stall, sample['time_ns'] - last_progress)
            last_progress = sample['time_ns']
        else:
            max_stall = max(max_stall, sample['time_ns'] - last_progress)
        previous = sample
    elapsed = (samples[-1]['time_ns'] - samples[0]['time_ns']) / 1e9
    fields = samples[-1]['guest_fields'] - samples[0]['guest_fields']
    return {'seconds': elapsed, 'guest_fields_per_second': fields / elapsed,
            'max_observed_stall_seconds': max_stall / 1e9}


def compare(reference, subject, minimum_speed=0.98, maximum_stall=0.5,
            nominal_guest_hz=60000/1001):
    if not 0 < minimum_speed <= 1 or not 0 < maximum_stall < float('inf'):
        raise ValueError('Invalid acceptance limits')
    if not 0 < nominal_guest_hz < float('inf'):
        raise ValueError('Invalid nominal guest clock')
    errors = []
    for key in ('game_sha256', 'initial_state_sha256', 'input_replay_sha256', 'scenario'):
        if not reference.get(key) or subject.get(key) != reference[key]:
            errors.append(f'Missing or mismatched {key}')
    r, s = measure(reference), measure(subject)
    if r['guest_fields_per_second'] <= 0:
        errors.append('Reference made no guest progress')
    if r['guest_fields_per_second'] < nominal_guest_hz * minimum_speed:
        errors.append('Reference itself is below nominal guest speed')
    if s['guest_fields_per_second'] < max(r['guest_fields_per_second'], nominal_guest_hz) * minimum_speed:
        errors.append('Guest execution is below reference speed')
    if s['max_observed_stall_seconds'] > maximum_stall:
        errors.append('Guest progress stalled or capture sampling is too sparse')
    # These must come from the game, not host window state or a test timeout.
    milestones = reference.get('completed_milestones', [])
    if not milestones or subject.get('completed_milestones') != milestones:
        errors.append('Required game milestones did not complete in reference order')
    if subject.get('completed') is not True or reference.get('completed') is not True:
        errors.append('Replay did not finish')
    return {'passed': not errors, 'errors': errors, 'reference': r, 'subject': s,
            'scope': 'Guest progress only; image/audio/input equivalence need separate checks'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('reference', type=Path)
    p.add_argument('subject', type=Path)
    args = p.parse_args()
    try:
        report = compare(json.loads(args.reference.read_text()), json.loads(args.subject.read_text()))
    except (KeyError, ValueError, TypeError) as e:
        report = {'passed': False, 'errors': ['Invalid trace: ' + str(e)]}
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
