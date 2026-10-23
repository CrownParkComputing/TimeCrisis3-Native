import copy
import unittest
from tools.check_replay import compare


def trace(fps=60):
    return dict(game_sha256='game', initial_state_sha256='state',
                input_replay_sha256='input', scenario='stage1', completed=True,
                completed_milestones=['section1', 'section2', 'section3', 'section4'],
                samples=[dict(time_ns=i*100_000_000, guest_fields=i*fps//10,
                              presented_frames=i*12, generated_frames=i*6) for i in range(101)])


class ProgressTests(unittest.TestCase):
    def test_reference_speed(self):
        self.assertTrue(compare(trace(), trace())['passed'])

    def test_40fps_with_120_presented_fails(self):
        self.assertFalse(compare(trace(), trace(40))['passed'])

    def test_two_slow_runs_do_not_establish_full_speed(self):
        self.assertFalse(compare(trace(40), trace(40))['passed'])

    def test_freeze_despite_good_average_fails(self):
        subject=trace(90)
        for sample in subject['samples'][40:60]:
            sample['guest_fields']=subject['samples'][39]['guest_fields']
        self.assertFalse(compare(trace(), subject)['passed'])

    def test_wrong_starting_state_fails(self):
        subject=trace();subject['initial_state_sha256']='different'
        self.assertFalse(compare(trace(), subject)['passed'])

    def test_cannot_end_before_freeze_section(self):
        subject=trace();subject['completed_milestones'].pop()
        self.assertFalse(compare(trace(), subject)['passed'])

    def test_cannot_hide_reset(self):
        subject=trace();subject['samples'][-1]['guest_fields']=0
        with self.assertRaises(ValueError):compare(trace(), subject)

    def test_empty_metadata_rejected(self):
        reference=trace();del reference['input_replay_sha256']
        self.assertFalse(compare(reference, copy.deepcopy(reference))['passed'])

    def test_nonfinite_counter_rejected(self):
        subject=trace();subject['samples'][-1]['guest_fields']=float('nan')
        with self.assertRaises(ValueError):compare(trace(), subject)

    def test_duplicate_timestamp_rejected(self):
        subject=trace();subject['samples'][1]['time_ns']=0
        with self.assertRaises(ValueError):compare(trace(), subject)


if __name__ == '__main__':
    unittest.main()
