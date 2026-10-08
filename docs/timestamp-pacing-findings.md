# Timestamp pacing: findings and evaluation

Status (2026-10-08): built for Windows and Linux. Not yet run on a live
stream, and the Gamescope path has never run on SteamOS. Everything below
comes from offline replays of earlier captures through the current policy
(`timestamppacingpolicy.h`), not from live measurements.

Related:
- [timestamp-pacing-proposal.md](timestamp-pacing-proposal.md): the original
  design and the first pacing log's findings (section 5a).
- [architecture.md](../architecture.md), "Timestamp pacing": what is built.

Naming: "VRR Pacing Mode" means Moonlight's `VrrPacingWorker` path. Plain
"VRR" means display or compositor adaptive sync.

## 1. What the smoothing presets do

Smoothing only changes how closely frames follow the host's raw RTP
timestamps versus a steady cadence fitted to them. Buffer sizing, repeat
handling, V-blank placement and everything else are the same for every
preset.

Every preset except Off runs a two-gain phase-locked loop on the host
timeline. For each frame it predicts where the frame should land on the
current cadence and compares that with the actual timestamp:
- The phase gain moves this frame by that share of the error.
- The frequency gain corrects the estimated period by that share.

| Preset | Phase gain | Frequency gain | Roughly averages over | A stamp 2 ms early is shown |
|---|---|---|---|---|
| Off | 1.0 (raw) | none | 1 frame | 2 ms early |
| Light (default since 2026-10-08) | 0.25 | 0.01 | ~4 frames | ~0.5 ms early |
| Standard | 0.10 | 0.002 | ~10 frames | ~0.2 ms early |
| Strong (removed 2026-10-08) | 0.05 | 0.001 | ~20 frames | ~0.1 ms early |

All smoothed presets also share these safeguards:
- **Re-anchor:** an error over max(8 ms, half a period) snaps the loop to the
  raw timestamp (a stall or jump).
- **Rate change:** the period is replaced outright when the last 9
  single-frame gaps agree within 10% on a rate more than 15% away.
- **Snap back:** after a brief slowdown, the long-run period is restored as
  soon as the cadence matches it again.
- **Frame-number gaps** count as missed frames, not a slower cadence.

Off still tracks the period, which the stall threshold and the V-blank phase
lock use.

## 2. Captures available for evaluation

| Capture | Setup | Length | Limitations |
|---|---|---|---|
| `build/timestamp-pacing-captures/moonlight-pacing-20261007-184523-089.csv` (gitignored; SHA-256 `e88dea88…`) | Windows, D3D11, 110 FPS requested, 120 Hz, unpaced | 171 s, 17,166 frames | Host averaged ~101 FPS including stalls (~107 FPS without) |
| `build/review-gamescope-latest/Moonlight.vrrtrace.pacing.csv` (2026-09-27; duplicate in `build/deck-pacing-review/`) | Steam Deck, SteamOS Game Mode, Gamescope, 60 FPS on 60 Hz, VAAPI | 507 s, 28,594 frames | No host latency, so host repeats can't be detected. Recorded under an experimental Gamescope pacer whose queue could delay decoding |
| `%USERPROFILE%\Desktop\vrr-diagnostics\20260927-010659-373-…\Moonlight.vrrtrace` (decode with `scripts/decode-vrr-trace.py`) | Windows, 116 FPS requested, 240 Hz VRR panel | 36 s, 3,222 frames | Short. The game ran ~90 FPS. Recorded under VRR Pacing Mode, whose decode hold can shift arrival times. No host latency |

The `mailbox-test` branch's own Windows and Deck captures (2026-09-28) are not
on this machine.

## 3. Replay model

Each capture's (frame number, RTP timestamp, decode-complete time, decoder
queue time) is fed through `TimestampPacing::Policy`. "Pacing off" shows each
frame on arrival. Each frame is given 1 ms of render time.

**Fixed refresh:**
- V-blanks lie on a perfect grid.
- Paced frames get the `PhaseLock` V-blank for their target, or the next
  V-blank if they are ready too late.
- Frames on the same V-blank keep only the newest (mailbox).
- Results are averaged over 8 grid phases, since the real phase is unknown.

**VRR:** frames are shown at max(target, ready), never closer together than
the panel's maximum refresh rate.

**Metrics:**
- **Jerk over 2 ms:** the share of consecutive presented-interval pairs that
  differ by more than 2 ms.
- **Skipped:** frames never shown.
- **Held:** shown longer than the stream rate calls for.
- **Delay:** mean of present time minus decode completion.
- **Late:** ready after the frame's target.

The model doesn't include:
- The renderer queue, or GPU and compositor latency.
- The Windows on-demand refresh behaviour found on `mailbox-test`
  (section 7).
- The present-call cost of real hardware.

## 4. Results

### 4.1 VRR display: share of frame pairs off by more than 2 ms

| | Windows 110/120 Hz | Deck 60/60 Hz* | Windows ~90 FPS, 240 Hz |
|---|---|---|---|
| Timestamp pacing off | 6.8% | 11.4% | 50.7% |
| Timestamp: Off | 12.1% | 12.1% | 84.2% |
| Light | 2.8% | 1.2% | 5.7% |
| Standard | 2.8% | 1.2% | 10.7% |
| Strong | 2.8% | 1.0% | 13.4% |
| Added mean delay vs pacing off (Standard) | +6.5 ms | +4.2 ms | +8.1 ms |

\*The Deck's panel doesn't do VRR at 60 Hz. This column only shows the timing.

### 4.2 Fixed refresh: skipped frames

| | Windows 110 FPS, 120 Hz | Deck 60 FPS, 60 Hz | ~90 FPS, 120 Hz | ~90 FPS, 240 Hz |
|---|---|---|---|---|
| Timestamp pacing off | 1.25% | 1.07% | 4.06% | 1.56% |
| Timestamp: Off | 3.04% | 0.61% | 1.34% | 0.64% |
| Light | 0.20% | 0.27% | 1.30% | 0.87% |
| Standard | 0.14% | 0.35% | 1.83% | 1.21% |
| Strong | 0.17% | 3.80% | 1.90% | 1.25% |
| Added mean delay (Standard) | +6.9 ms | +4.1 ms | +8.1 ms | +8.1 ms |

Frames held for an extra refresh fell by about 1 point. Overall jerk barely
changed, because the mismatch between frame rate and refresh rate dominates
it: 110 FPS on 120 Hz must hold one frame in eleven for two refreshes however
well it is paced.

### 4.3 Windows log, continuous model (no refresh grid), 99% target

| Preset | Jerk > 2 ms | Jerk p99 | Mean added delay | Late |
|---|---|---|---|---|
| Off | 30.43% | 7.81 ms | 8.21 ms | 0.61% |
| Light | 0.90% | 1.95 ms | 7.90 ms | 0.61% |
| Standard | 0.67% | 1.08 ms | 7.95 ms | 0.65% |
| Strong | 0.66% | 1.03 ms | 8.24 ms | 0.59% |

Light's extra jitter here is under 2 ms. On the uneven ~90 FPS capture it
followed the real cadence much better than Standard (section 4.1).

### 4.4 Maximum buffer (Windows log, Standard, 99%, continuous model)

| Maximum | Jerk p99 | Late | Mean added delay |
|---|---|---|---|
| 8 ms (≈ one refresh at 120 Hz) | 3.27 ms | 2.22% | 4.87 ms |
| 10 ms | 2.12 ms | 1.00% | 5.89 ms |
| 12 ms | 1.86 ms | 0.84% | 6.57 ms |
| 16 ms (default) | 1.08 ms | 0.65% | 7.95 ms |

`mailbox-test` found a one-refresh cap better on its captures; this log
disagrees. On a fixed-refresh grid, sub-refresh errors partly disappear when
frames snap to refreshes, which may explain the difference. Keep 16 ms until a
live capture decides.

### 4.5 Sensitivity of the other settings (Light, 2026-10-08)

Following these results, the target, buffer limits and margin were removed
from the settings page on 2026-10-08. Only test keys in the settings file can
change them now (architecture.md, "Timestamp pacing", Selection).

VRR model, share of frame pairs off by more than 2 ms / mean delay / late:

| Setting | Windows 110/120 | Deck 60/60 | Windows ~90 FPS, 240 Hz |
|---|---|---|---|
| Defaults (99%, 2–16 ms) | 2.8% / 8.0 ms / 0.9% | 1.2% / 8.7 ms / 6.3% | 5.7% / 9.6 ms / 1.2% |
| Target 95% | 3.8% / 4.4 ms / 8.0% | 1.5% / 7.9 ms / 13.3% | 7.8% / 7.3 ms / 3.7% |
| Target 99.9% | 2.5% / 11.5 ms / 0.2% | 1.0% / 10.4 ms / 3.4% | 5.3% / 10.7 ms / 1.0% |
| Minimum 0 or 5 ms | unchanged | unchanged | unchanged |
| Maximum 8 ms | 3.4% / 5.0 ms / 3.2% | 1.2% / 7.0 ms / 13.5% | 20.9% / 4.1 ms / 21.5% |
| Maximum 30 ms | 2.8% / 10.5 ms / 0.7% | 1.2% / 11.4 ms / 4.9% | 5.1% / 18.1 ms / 0.7% |

Fixed-refresh skipped frames moved by at most 0.2 points for any of these.

- **Minimum buffer:** no effect. The buffer always sat above it.
- **Maximum buffer:**
  - 16 ms is a good fixed value.
  - 8 ms quadrupled uneven frames on the jittery capture.
  - 30 ms added up to 8.5 ms of delay for almost nothing.
- **Target:** the only real trade-off. 95% saves 1–4 ms but shows 4–13% of
  frames late; 99.9% costs 2–3.5 ms for a small gain.
- **V-blank margin:** not modelled. Its right value depends on the
  machine's render and compositor timing. Gamescope widens it automatically;
  Windows has no miss feedback yet, so the margin is still its only remedy.

### 4.6 Against the Frame Pacing toggle (fixed refresh, 2026-10-08)

The Frame Pacing toggle (`Pacer::handleVsync()`) ignores timestamps. At each
V-sync it hands the oldest waiting frame to the renderer, to be shown at the
following V-blank. With none waiting, it waits for one until 3 ms before the
next V-sync. The queue is trimmed to one frame, or three while the stream rate
can reach the display rate and recent V-syncs found the queue short. It was
modelled the same way as the others: same captures, 8 phases, 1 ms render.

| | Windows 110 FPS, 120 Hz | Deck 60 FPS, 60 Hz* | ~90 FPS, 120 Hz |
|---|---|---|---|
| Pacing off: skipped / held / delay | 1.25% / 12.2% / 5.2 ms | 1.07% / 3.5% / 9.3 ms | 4.06% / 35.3% / 5.2 ms |
| Frame Pacing | 0.18% / 11.0% / 7.4 ms | 0.00% / 1.8% / 14.4 ms | 0.91% / 31.2% / 7.6 ms |
| Timestamp: Light (defaults) | 0.20% / 11.2% / 12.0 ms | 0.27% / 2.3% / 13.5 ms | 1.30% / 32.3% / 13.6 ms |
| Timestamp: Light, 95%, max 8 ms | 0.48% / 11.5% / 7.1 ms | 0.25% / 2.4% / 11.4 ms | 1.63% / 32.8% / 8.1 ms |

\*Frame Pacing can't run in Steam Deck Game Mode. It needs a V-sync source,
which exists only for SDL's Windows and Wayland backends, and Moonlight runs
on Gamescope's Xwayland. This column is hypothetical.

On a fixed-refresh display with a V-sync source, Frame Pacing was as smooth as
timestamp pacing or smoother, with 4.6–6 ms less delay. Shrinking timestamp
pacing's buffer to match its delay skipped 2–3× more frames. The reason:
Frame Pacing delays a frame only when an earlier one already holds the coming
refresh, while the timestamp buffer delays every frame by the lateness
percentile. A refresh grid already absorbs sub-refresh jitter, so that buffer
mostly adds latency there.

Timestamp pacing's distinct value is therefore:
- Gamescope Game Mode, where Frame Pacing can't run.
- VRR displays used without VRR Pacing Mode (section 4.1).
- V-Sync off, which Frame Pacing doesn't support.

A grid-mode release that behaves like Frame Pacing's queue is worth replaying
before more live testing on fixed-refresh Windows displays. Caveats as in
section 3: neither model includes Windows' on-demand refresh, and real V-sync
wakeup lateness is not modelled.

## 5. Conclusions

- **The average frame rate is never changed.** It is whatever the host sends
  (~101, ~57 and ~90 FPS in the three captures). Timestamp pacing corrects
  per-frame timing: errors of about 1–8 ms on frames of 9–17 ms.
- **VRR displays gain the most:**
  - Uneven frame pairs fell from 7–51% to 1–6% (with Light), at 4–9 ms of
    added delay.
  - The gain is largest when arrival jitter is high and the game runs below
    the requested rate.
- **Fixed-refresh displays gain modestly:**
  - 20–85% fewer skipped frames, and slightly fewer extra holds.
  - The built-in judder from mismatched rates remains. Matching the stream
    rate to the refresh rate (120 on 120, 60 on 60 or 120) does more for
    smoothness than any pacing option.
- **Animation correctness:**
  - RTP timestamps are host capture times, not game simulation times.
    Timestamp pacing removes jitter added after capture (network, decoding),
    which genuinely improves motion consistency.
  - Smoothing assumes the host's timestamp noise isn't real. That holds for
    frame-capped games and is slightly wrong for uncapped games with
    wandering frame times.
  - Host-side stalls (p99 host intervals of 15–61 ms in these captures) are
    reproduced faithfully and cannot be fixed.
- **Presets:**
  - Off was worse than no timestamp pacing on every VRR case; it is mainly a
    diagnostic.
  - Light matched or beat Standard on every capture.
  - Strong was clearly worse on the Deck (3.8% skipped against 0.3%), which
    changed rate often between 56 and 60 FPS.
  - Applied 2026-10-08: Light is the default and Strong is removed. The
    tables in section 4 keep Strong's results for reference.
- **The Deck capture showed 6–8% of frames late,** against the 99% on-time
  target. Undetectable host repeats (no host latency in that trace) are the
  likely cause. This needs a capture from the current build.

## 6. Changes adopted from VRR Pacing Mode (2026-10-07)

Each was checked against the Windows log before and after the change.

**Applied:**
- **Gradual baseline:** targets follow the 3 s minimum arrival offset at up
  to 2.4 ms/s and at most 0.1 ms per frame. For the first 64 frames of a
  timeline, an earlier minimum is adopted at once. Jerk p99 went from 1.22 to
  1.08 ms with slightly less delay. This was nearly all of the gain.
- **Decoder overload:** while frames waited on average longer than a period
  to enter the decoder over the last second, their lateness doesn't size the
  buffer.
- **Stall cap:** lateness over three times the maximum buffer (48 ms by
  default) doesn't size the buffer, so one long network stall can't pin it at
  its maximum for about 30 s.
- **Re-anchored timelines** are left out of buffer sizing.
- **Learned wake-up lead:** the precise waiter wakes early by the 95th
  percentile of its last 19 oversleeps, capped at 0.5 ms.
- **Fix:** with no admitted frames in the window, the buffer used to freeze.
  It now releases toward the minimum.

**Rejected on the data:**
- VRR Pacing Mode's delivery-stall exclusion (gaps over max(25 ms, 1.5
  periods) plus their trailing backlog) and its host-burst exclusion
  (intervals under 0.75 period).
- Both cut 0.6–0.8 ms of delay but raised jerk p99 to 1.2–1.5 ms and late
  frames to about 0.77%. Frames late by more than the maximum still count
  against the on-time target, so any lower stall multiple than three did the
  same.

Net on the Windows log (Standard, 99%):
- Jerk p99: 1.22 → 1.08 ms
- Pairs off by more than 2 ms: 0.69% → 0.67%
- Mean added delay: 8.11 → 7.95 ms
- Late: 0.64% → 0.65%

**Not yet done:**
- Rendering before waiting, by running the policy inside VRR Pacing Mode's
  `IVrrFramePresenter` path.
- `Present(1,0)` for exclusive fullscreen.
- A `vrrreplay` scenario for this policy.

For windowed fixed refresh, `Present(1,0)` is not useful: it queues FIFO-style
instead of replacing a pending frame. Latched presents also measured slow on
the Radeon 890M (architecture.md, around line 910).

## 7. Lessons from the `mailbox-test` branch

`mailbox-test` (2026-09-28, not merged) has a "Smooth V-Sync" mode with the
same core idea, driven from the V-sync thread. Its hardware captures point
to three gaps here.

1. **Windows can skip refreshes with VRR off.**
   - After a refresh with no Present, the next V-sync came two periods later
     77% of the time. That skipped 8.9% of frames at 110 FPS on 120 Hz.
   - Re-presenting the last frame on empty refreshes (D3D11) cut it to 0.5%.
   - Timestamp pacing presents nothing on empty refreshes, and its V-blank
     grid assumes a steady cadence.
   - Highest priority to port, together with repeated/skipped/missed refresh
     counters to verify it.
2. **Gamescope display times don't need `ENABLE_GAMESCOPE_WSI=1`.** A Deck
   Game Mode capture got display times for 18,580 of 18,603 presents from
   the timing extension alone. Timestamp pacing required the variable, so
   it might never have got its V-blank grid on the Deck. Applied 2026-10-08:
   the requirement is dropped for timestamp pacing.
3. **A 2 ms margin is likely too small under Gamescope.** Frames handed over
   less than about 6 ms before a refresh mostly missed it, and the branch's
   learned lead settled near 4.5 ms. Timestamp pacing's self-widening margin
   recovers only after it has missed refreshes. Applied 2026-10-08: under
   Gamescope the margin starts at 4.5 ms.

Also from the branch:
- On that Deck capture, host timestamps were 99.9% regular, so its scheduler
  added 2–20 ms without improving on showing frames at arrival.
- Timestamp pacing can only help when arrival or timestamp jitter is real.

What timestamp pacing has that the branch lacks:
- Host repeat handling.
- Following Gamescope's VRR, Allow Tearing and frame limit mid-stream.
- V-Sync-off and no-grid operation.
- Tunable settings.
- A stronger smoother.
- Stall and overload exclusion.
- Real-time graphs.

## 8. Windows vs Gamescope

The policy, pacing thread and waiter are identical on both. They differ as
follows.

| | Windows | Gamescope (Game Mode) |
|---|---|---|
| Renderer | D3D11 | Vulkan, preferred automatically |
| V-blank times | `DxVsyncSource` wakeups, predictive | Gamescope's reported display times (`VK_GOOGLE_display_timing`); needs only the extension since 2026-10-08 |
| Without timing | Not expected | Frames released at their target |
| Refresh period | Display mode, refined ±3% | Gamescope's own (`vkGetRefreshCycleDurationGOOGLE`) |
| Frame replacement | DWM flip model, `Present(0)` | Mailbox present mode chosen by Moonlight |
| VRR, tearing, limiter | VRR measured from the V-blank rate since 2026-10-08 (not in exclusive fullscreen); tearing and limiter not applicable | Polled every 250 ms and followed |
| Missed V-blank feedback | From DXGI frame statistics (2026-10-08) | From Gamescope's display times |
| Starting submit margin | 2 ms | 4.5 ms (since 2026-10-08) |
| Margin after a miss | +0.5 ms per miss up to +6 ms; released after 5 s clean | Same |
| Exclusive fullscreen | Released at the V-blank | Not applicable |

SteamOS Desktop Mode:
- With SDL on native Wayland, the Wayland V-sync source is used, much like
  Windows.
- With SDL on X11, there is no grid.

## 9. Next steps, in priority order

1. Re-present the last frame on empty refreshes (Windows D3D11, V-Sync on).
   Missed V-blank detection on Windows was added 2026-10-08 (DXGI frame
   statistics; architecture.md, "Windows missed V-blank detection"). It shows
   whether frames miss their refresh, but not whether the display skipped a
   refresh nobody presented to.
2. ~~Drop the `ENABLE_GAMESCOPE_WSI` requirement for timestamp pacing's
   display times.~~ Done 2026-10-08.
3. ~~Start Gamescope at a submit margin of about 4.5 ms.~~ Done 2026-10-08.
4. ~~Make Light the default smoothing; reword or remove Strong.~~ Done
   2026-10-08.
5. Live captures with the current build, timestamp pacing on, a frame trace
   (Settings, "Trace paced frames for debugging"; `Moonlight.tstrace`, since
   2026-10-08) and the presented-smoothness graph visible:
   - Windows at 120 FPS on 120 Hz (matched rates, where phase lock matters
     most).
   - Deck Game Mode at 60 on 60 and 40 on 40.
   - One uncapped game on VRR.
6. Decide on a maximum-buffer cap in V-blank mode from those captures
   (section 4.4).
7. Check the replay scorer into the repo, so every future change is scored
   against the same captures. The 2026-10-07/08 scorers lived in session
   scratch.
8. Later: render before waiting, and `Present(1,0)` for exclusive fullscreen.

## 10. Open questions

- Does Windows' V-blank rate fall well below the nominal rate when G-Sync or
  FreeSync is engaged on a windowed or borderless flip-model swapchain? The
  measured-mode log line ("display measured as …: N% of … Hz refreshes")
  answers it. If it never falls, the measurement can't see VRR there, and a
  vendor query (NVAPI, ADL) would be the next option.

- Does `GetFrameStatistics()` report usable display times for a windowed
  flip-model swapchain that DWM composes, as well as for independent flip?
  The teardown log line "D3D11 display reports" answers this per session.

- Does Gamescope report display times for Moonlight in Game Mode with
  timestamp pacing's Mailbox swapchain? The branch measured FIFO presents.
- How often does Windows skip refreshes on other GPUs and displays, and does
  it happen in exclusive fullscreen?
- Does the Deck's 6–8% late share disappear once host repeats are detected?
