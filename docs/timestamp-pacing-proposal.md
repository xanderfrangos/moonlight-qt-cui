# Timestamp Pacing: a lite RTP-driven pacing mode (proposal)

Status: first implementation built (2026-10-07), not yet validated on a live
stream. architecture.md ("Timestamp pacing") describes what was built. It
differs from this proposal where the first pacing log required it:
smoothing is on by default; host repeat frames are shown on arrival; and the
queue is capped at three frames. Original source baseline: `7e642e63`;
the pacer sources are unchanged since the `vrr17` merge (`1b2ba09d`) that
[architecture.md](../architecture.md) describes.

Naming: "VRR Pacing Mode" means Moonlight's existing `VrrPacingWorker` path.
Plain "VRR" means display/compositor adaptive sync.

## Goal

Pace decoded frames from the host's RTP timestamps through a small rolling
jitter buffer (about 2–16 ms), with mailbox-style "latest due frame wins"
submission. It must work on both VRR and fixed-refresh displays. On fixed
refresh, OS-specific V-sync timing must place each frame on the correct
vblank.

## Recommendation

Build a **new, separate pacer mode** next to the legacy pacer and VRR Pacing
Mode, rather than trimming `VrrTimingController`. That controller is about
3,800 lines. Most of it is what this mode drops: latch and floor revisions,
reserves, calibration persistence and exact-replay compatibility.

Reuse the plumbing that already works:

- `IVrrFramePresenter` ([ivrrframepresenter.h](../app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h)):
  decode boundary, prepare, present, cancel
- D3D11 decode-to-render and present-ready fences ([d3d11va.cpp](../app/streaming/video/ffmpeg-renderers/d3d11va.cpp))
- `VrrTargetWaiter` for precise deadline waits ([vrrtargetwaiter.cpp](../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp))
- `D3D11CompositionPresenter` ([d3d11composition.cpp](../app/streaming/video/ffmpeg-renderers/d3d11composition.cpp))
- Existing V-sync sources: `DxVsyncSource`, `WaylandVsyncSource` ([pacer/](../app/streaming/video/ffmpeg-renderers/pacer/))

Estimated new code: roughly a 600–1,000 line worker plus a small display-clock
layer for each OS.

## 1. Core loop (same for VRR and fixed-refresh displays)

### 1.1 Map host time to client time

- `offset` = windowed minimum over about 3 s of `(readyTime − rtpUs)`, slewed
  slowly so host/client clock drift is followed without steps.
- The earliest-arriving frame defines zero jitter; lateness is measured from it.
- A discontinuity, wrap or large forward jump rebases the mapping.
- A host capture stall needs no special handling: the RTP gap and the arrival
  gap match, so the gap is shown faithfully.

### 1.2 Rolling buffer

- Per frame: `lateness = readyTime − (rtpUs + offset)`, where "ready" means
  decoded *and* rendered. Rendering starts on arrival, so a single histogram
  covers network, decode and render jitter.
- `delay = clamp(p99(lateness over ~3 s) + 0.5 ms, 2 ms, maxDelay)`.
- `maxDelay = min(16 ms, ~2 source periods)`, so a 120 FPS stream doesn't get a
  16 ms buffer it can't queue for.
- Growth is fast but needs a sustained miss rate (e.g. >0.5% of frames in the
  last 2 s), so one network burst doesn't latch the buffer high.
- Release is slow: about 0.25–0.5 ms/s after 2–3 s without misses.
- `target = rtpUs + offset + delay`.
- Decoded-frame queue depth: `ceil(maxDelay / period) + 2`. The decoder
  surface pool (`extra_hw_frames`) must reserve that many.

### 1.3 Mailbox-style release

- At each submit opportunity, the newest frame whose target has passed wins.
  Older due frames are dropped as presentation drops; codec references are
  unaffected and no IDR is needed.
- A late frame is shown immediately; the next frame keeps its own slot.
- This bounds latency without a growing queue.

## 2. Display adapters

### 2.1 VRR display

- Submit each frame at its target.
- One safety rule instead of the latch machinery: `Present(1, 0)`, i.e. V-sync
  on with VRR, the standard G-Sync/FreeSync setup. The FPS picker already keeps
  the stream below the refresh ceiling.
- The panel follows the frames, and a frame never flips inside the panel's
  minimum period.

### 2.2 Fixed-refresh display: phase-locked V-sync assignment

The legacy pacer ([pacer.cpp](../app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp)
`handleVsync()`) releases frames in *arrival order* at each vblank, so network
jitter becomes repeats and drops. Assigning frames by target time fixes that:

```text
vblank grid:   V_k = phase + k * refreshPeriod     (estimated and filtered from OS vblank timestamps)
assign:        frame i -> first vblank V_k >= target_i
submit at:     V_k - latchLead                     (learned: grows when a frame lands one vblank late)
phase lock:    slowly add a small trim to `delay` so targets sit mid-interval,
               away from the latch deadline
```

- **Phase lock:** without it, a 60 FPS stream on a 60 Hz display with a few ppm
  of clock drift keeps flipping between two vblanks, which looks like constant
  judder.
- **Drift:** with the trim, drift builds up slowly and costs exactly one
  deliberate repeat or drop when it wraps, applied with hysteresis.
- **Latency:** the vblank wait adds 0 to 1 refresh on top of `delay`. The trim
  can use that slack to keep the buffer smaller.
- **Non-integer ratios** (e.g. 50 FPS on 60 Hz) judder inherently. The same
  assignment still keeps frames away from latch deadlines.

### 2.3 Per-OS timing sources

| OS | Vblank grid / feedback | Submission |
|---|---|---|
| Windows 11 | Composition presenter statistics (real display events) | `IPresentationManager` target time. The OS aligns to vblank and already cancels older pending presents, which is native mailbox behaviour. |
| Windows fallback | `D3DKMTWaitForVerticalBlankEvent` (already in `DxVsyncSource`) or `GetFrameStatistics` SyncQPC. Fine for the grid phase, not for per-frame display proof (see architecture.md §10.3). | DXGI `Present(1, 0)` at `V_k − latchLead` |
| Linux Wayland | `wp_presentation` feedback (refresh + timestamps) | Vulkan Mailbox; FIFO plus timed submit when Mailbox isn't available |
| Linux (other) | `VK_KHR_present_wait` / `present_id`; `VK_EXT_present_timing` where drivers have it | Vulkan Mailbox at the target |
| macOS | `CADisplayLink` timestamps | `presentAtTime:` |

## 3. What gets dropped from VRR Pacing Mode

- Cadence smoothing (Reduce judder)
- The interval-quality observer
- Readiness and smoothing reserves
- Latch revisions and the flip-queue anchor
- Calibration persistence (`vrr13-calibration.json`)
- Capacity telemetry
- Schema compatibility for exact replay

## 4. Caveat: pure timestamp fidelity copies host jitter

Host RTP stamps can jitter by several milliseconds from frame to frame, for
example from WGC stamping at DWM composition. Pacing strictly to RTP shows that
jitter faithfully, which is part of why the full mode has Reduce judder.

Ship pure RTP first and measure **presented jerk**. If the host stamps are
noisy, add one optional smoothing step: a simple period-tracking filter with a
single gain. Host-side fixes (Vibeshine's 1000 Hz virtual display and
Present-event stamps, architecture.md §3.3) attack the same problem at the
source.

## 5. Validation before live testing

1. **Offline simulator.** Existing `.vrrtrace` captures already contain, for
   every frame, the RTP timestamp, arrival, decode-complete and render times.
   Run the lite policy over those traces and compare presented jerk and
   latency against the `session-policy` row. This shows whether the lite mode
   competes before the renderer is touched. The usual counterfactual limits
   apply (architecture.md §13.3): the simulator can't predict compositor or
   scheduling changes.
2. **Unit tests** for the clock mapper and rolling buffer with synthetic
   jitter, and for the fixed-refresh phase lock with a synthetic vblank grid
   that has drift.
3. **Live tests:** VRR display, then 60 FPS on 60 Hz fixed refresh (the worst
   case for phase lock), comparing presented jerk with the legacy pacer and
   VRR Pacing Mode.

## 5a. Findings from the first pacing log (2026-10-07)

Capture: `moonlight-pacing-20261007-184523-089.csv`, 171 s, 110 FPS stream,
120 Hz display, D3D11VA, frame pacing off (`pacing=unpaced`, so no vblank
rows), 17,166 frames, no network loss. Lateness is measured at decoder output.

1. **Host repeat frames carry backdated timestamps.** All 169 frames that
   arrived >40 ms late against their RTP timestamp, and only those, report a
   host processing latency of 0. They appear when the game stops producing
   frames. Each one's timestamp is a few ms after the previous frame, but it
   arrives ~55–60 ms later; following repeats are ~62 ms apart, and the next
   real frame is back on time. This is the ~60 ms step seen in the stats
   graph. The pacer must not learn from these frames or hold them to their
   timestamps: show them on arrival. This needs a fallback for hosts that
   never report host latency.
2. **Clock drift is ~40 ppm.** The minimum arrival offset rose 6.8 ms over
   171 s, so the baseline must be a rolling minimum (3 s is enough).
3. **Real-frame jitter is small.** Lateness against a rolling 3 s minimum:
   p50 2.1, p95 6.0, p99 9.2, p99.9 19.4 ms.
4. **Host RTP stamps are noisier than delivery.** About one stamp in ten is
   displaced ~2 ms early, followed by a compensating ~11 ms interval. Those
   frames still arrive on schedule, and their host latency is unchanged
   (3.06 vs 3.02 ms). So the stamp moves while capture and send don't.
   Adjacent-interval jerk over 2 ms: 30% in RTP spacing, 9.5% in arrival
   spacing.

Simulated on the same real frames (rolling buffer clamped to 2–16 ms, fast
growth, 0.5 ms/s release; jerk is on present-call times, not scanout):

| Policy | Jerk > 2 ms | Jerk p99 | Added delay (mean) | Shown late |
|---|---|---|---|---|
| Present on arrival (current) | 9.4% | 6.3 ms | 0 | — |
| Strict RTP, buffer p99 | 30.3% | 7.8 ms | 8.4 ms | 0.6% |
| Smoothed RTP, buffer p99 | 0.7% | 1.0 ms | 9.4 ms | 0.9% |
| Smoothed RTP, buffer p95 | 1.6% | 3.4 ms | 6.3 ms | 2.5% |

"Smoothed RTP" is a causal phase-locked loop on the RTP timeline (gains
0.05 phase, 0.002 period; re-anchors on errors over 15 ms). With this host,
**strict RTP pacing makes cadence worse than presenting on arrival**. The lite
pacer should follow a smoothed host timeline, so smoothing is required rather
than optional. Whether the ~2 ms stamp displacement reflects real game timing
(which would make strict RTP motion-correct) can't be determined from client
data; a visual A/B would settle it.

Still needed: a capture with frame pacing on (V-sync source rows) for the
fixed-refresh phase lock, and one with a long idle stretch.

## 6. Open decisions

1. **Alongside or replacing?** Recommended: alongside, as a third option in
   Settings, so the modes can be compared on the same machine.
2. **Platform order:** recommended Windows first, because the composition
   presenter makes the fixed-refresh case nearly free there, then Linux Vulkan.
3. **Smoothing:** the first pacing log (section 5a) shows it is needed with
   this host. Strict RTP is worth keeping only as a visual A/B option.

Suggested first step: the offline simulator against existing traces.
