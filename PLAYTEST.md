# Play TC3

Run from the repository root after following [build and asset setup](docs/BUILD.md):

```sh
python3 tools/run_tc3.py
```

To leave the linked arcade pair open from an automation/terminal command:

```sh
python3 tools/run_tc3.py --background
```

The launcher records `.local/cabinet-pair-run.json`, `.local/cabinet-left.log`
and `.local/cabinet-right.log`. It opens two adjacent windows with isolated
SRAM/cards. Start on the **left** and shoot **LINK PLAY**. Once the intro reaches
gameplay, the uncredited right cabinet displays the genuine second camera.
F10 pauses both cabinets; rendering settings and Quit apply to both.
The current presentation is two windows, rather than a single combined window.

Use `python3 tools/run_tc3.py --single` for the original isolated playtest;
that mode records `.local/tc3-live.pid` and `.local/tc3-live.log`.

| Control | Action |
| --- | --- |
| `5` | Insert coin; the current cabinet requires two coins for a credit |
| `Enter` | Start |
| Mouse / left button | Aim / fire; shoot LINK PLAY on the left for both screens |
| Hold `Space` | Pedal / attack; release to take cover and reload |
| `F10` | Open/close Nuklear menu and pause/resume |
| `Esc` while menu is open | Resume |
| `F9` | Cabinet test mode |
| `F1` / `F3` | Single-cabinet diagnostic save/load only; disabled for the linked pair |

Normal boot takes roughly half a minute on the test machine. Movie sequences
may report 29.97 internal FPS while the machine runs at 100% speed; gameplay was
observed at approximately 59.94 internal FPS. The display shows game FPS and video
field rate separately inside the Performance menu. PCSX2 overlays and game patch
loading are disabled. Resolution (1–4x), FXAA, CAS sharpening/strength, VSync and
queue depth now control the real Vulkan renderer on both screens. Frame
generation is disabled.

FXAA, CAS and sharpness preview immediately on the paused image. They rebuild
from a retained clean GPU image, so repeated edits do not accumulate filtering.
Internal resolution needs fresh scene draws: each change runs two video fields
on both cabinets and then automatically pauses again, with the menu still open.
VSync and queue depth affect pacing and have no static-image effect.

For Sinden borders and detection, see [Sinden setup](docs/SINDEN.md). The new
[Cabinet/Test page](docs/CABINET_TEST.md) provides the original maintenance inputs
for both TC3 and TC4.

## What has been checked

The linked executable cold-boots the supplied TC3 media with isolated calibrated
SRAM and dongle storage. Input tests verified the pedal raises the view, ammunition
decreases when firing, and hits register. The Nuklear menu pauses and resumes the
game. A Cubeb audio stream is active at 48 kHz. Bounded runs exit normally through
VM shutdown. Eight CTest cases pass, along with the headless menu check.

The original arcade GAME OPTIONS screen reports `LINK: ON 2` with the native
local transport. A solo linked run showed different left/right stage-one views
while the right still had zero credits. The live desktop regression also verifies
both guests stop advancing while either menu is open, both renderers apply the
six settings, Escape resumes both, and menu Quit shuts both down cleanly.
Run `tools/test_cabinet_ui.py --quit` only against a fresh diagnostic pair; it
restores its settings and closes both windows.

`tools/test_paused_preview.py` compares actual right-window pixels with no menu
or mouse in that view. It checks visible FXAA/CAS/sharpness changes, exact image
restoration when effects are undone, unchanged guest fields for post-processing,
and exactly two fields per resolution change. Original user settings are restored.

`tools/sample_guest_progress.py --seconds 60 --camera` reads the development
binary's guest counters at 100 ms intervals without stopping either process.
Its optional bounded camera reads are diagnostic observations, not atomic
savestates or proof of completed stage milestones. The output cannot pass the
release replay gate without matched input/state identities and real completion.

This is the PCSX2-derived execution baseline inside a new Linux executable:
upstream EE/IOP/VU execution remains active. The scalar native translation proof is
not enabled for this playtest. There is no external PCSX2 process or Qt frontend.
The full stage-1 freeze point, precise aim alignment, audio equivalence and later
stages are not yet validated. DLSS/FSR/frame-generation controls remain disabled
until their renderer integrations work.

## Old savestate limitation

Do not start a new process directly from the old stage-1 reference state. Pinned
upstream `SaveState.cpp` stores JVS coins but omits the `ACJV::enabled` latch.
That latch starts false and is set by the guest's boot-time `ACJV_CTR_START` MMIO
command. Skipping boot shows the scene while GunCon2 pedal/trigger forwarding is
disabled. Input traces confirmed that USB button values changed but ACJV stayed
disabled. `run_tc3.py --stage1` now rejects this known-broken path.

Normal boot executes the actual board initialization. Proper versioned arcade
state serialization remains required before deterministic fresh-process replay
can be treated as valid. No forced board flag or guest RAM input patch is used.

## Rebuild

```sh
python3 tools/prepare_playtest.py
python3 tools/build_core.py --playable
```

Private preparation preserves existing mutable storage. The original reference
and old ground-up port are untouched. The current development binary still uses
local shared build dependencies; standalone packaging is not completed.
