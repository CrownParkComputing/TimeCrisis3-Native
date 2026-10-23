# Guest-progress acceptance

`tools/check_replay.py reference.json subject.json` consumes captured progress and
exits unsuccessfully for a speed regression, a stall, missing stage milestones,
different input replay/initial state, or an incomplete run. It does not consume
host FPS and cannot be satisfied by interpolated frames.

The collector is **not yet connected to the game**. Current unit tests use
synthetic traces to check that 40 guest fields/s with 120 presented frames/s fails,
that a freeze fails even with a good average, and that stopping before the failing
section cannot pass. They do not establish that the game is fixed.

Trace fields:

* `game_sha256`, `initial_state_sha256`, `input_replay_sha256`: actual identities.
* `scenario`: versioned replay scenario ID (stage 1 must include the reported freeze
  point and the preceding wrong-camera section).
* `samples`: increasing monotonic `time_ns`, nondecreasing integer `guest_fields`.
  Collect at 100 ms or finer intervals. Do not reset counters mid-run.
* `completed_milestones`: ordered gameplay section completion IDs.
* `completed`: true only after the scenario's game completion condition.

Default gates also require the reference to reach at least 98% of 60000/1001 Hz,
so two equally slow captures cannot establish full speed. Subject gates require
at least 98% of the reference's measured guest speed and no
observed progress interval above 500 ms. Warm-up/loading must be an explicit,
identical scenario phase, never silently removed after a failing run. Match
hardware, clock settings, initial state and input stream. Choose a long gameplay
window; short samples alone cannot establish sustained speed.

Separate equivalence checks remain necessary: camera identity/frame/pose and final
matrices, gun hit positions, IRQ/DMA/VU ordering, audio sample counts and waveform,
and image comparisons. A progress pass alone is never the release gate.
