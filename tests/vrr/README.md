# VRR deterministic tests and replay

The opt-in test project exercises the timing controller, source cadence, input
buffer, pacing worker, replay, and renderer policies. It is separate from the
application build. Run the suites relevant to a change, and follow `AGENTS.md`
for the complete Windows VRR build and deployment checks. On Linux, configure
the target inside the development container, for example:

```sh
mkdir -p build/vrr-tests
cd build/vrr-tests
qmake6 ../../tests/vrr/vrr.pro CONFIG+=tests
make -j2
```

`productionpolicybaseline.pro` records exact deterministic fingerprints across
production presets, smoothing modes, and presentation capabilities. These are
regression checks of the controller model, not measurements of screen output.
The input, source-gap, phase-trajectory, timing-controller, worker, profile,
replay-config, and performance-statistics tests cover their respective current
contracts. The worker fixture can export a fresh trace for replay verification:

```sh
MOONLIGHT_VRR_TEST_EXPORT_TRACE=fresh.vrrtrace ./tst_vrrpacingworker
vrrreplay fresh.vrrtrace --require-exact-baseline --output baseline.json
python3 ../../tests/vrr/check_fresh_trace_schema.py ./vrrreplay fresh.vrrtrace
```

Use the actual paths to the worker and replay binaries in your build tree.
The fixture is synthetic and does not establish live smoothness or scanout.

`tst_phasetrajectory` covers the policy-1 decode-buffer feedback regression:
late blocking/nonblocking completion checks, cold and warm GPU estimates,
retained startup protection, prompt real GPU stalls, actual readiness floors,
and completion arithmetic near the timestamp limit. `tst_vrrcacheidentity`
checks that the corrected estimator cannot load a prior saved startup delay.

## Capture and exact replay

The worker writes schema-6 captures as compressed `.vrrtrace` files or CSV.
Replay requires schema 6 and all current controller parameters; older captures
must be recaptured for controller replay. `scripts/decode-vrr-trace.py` still
decodes older compressed traces for inspection. Trace capture is asynchronous
and bounded, so inspect the clean-close footer, dropped rows and sequence
integrity before treating it as complete.

```sh
vrrreplay capture.vrrtrace --require-exact-baseline --output baseline.json
```

Check the process exit code and both `capture.recorded_sequence_integrity_valid`
and `fidelity.baseline_exact` in the result. Exact replay reproduces the
recorded controller and execution model; it is not a measurement of physical
scanout or proof that a changed policy would cause the same host, network, GPU,
or display events. Use a fresh matching capture and preserve its baseline
separately from candidate results. Re-enumerate captures before each analysis
as described in `AGENTS.md`.

The 2026-10-30 correction changes the existing policy-1 accounting in place.
Captures recorded before that correction can be rejected for divergent
diagnostics or original deadlines despite retaining the same policy fields.
Preserve an exact baseline from the matching old executable and use fresh
worker captures for the corrected controller's replay and stress gates.
Never waive an exact mismatch or introduce a second production policy merely
to reproduce the old bug.

## Current-policy scenarios

Replay configurations use `config_schema: 2` and `base: "session"` to derive
the current production policy from the capture's settings and presenter. The
recorded baseline retains its captured current parameters. Revision-specific
historical policies and schema-1 configs are unsupported. `smoothing_mode`
selects the current off (0), worker phase-locked (2), native tolerance-bounded (4)
or Absorb (7) mode; other values and retired revision selectors are rejected.
The production and interval policy baseline targets preserve 54 deterministic
pre-cleanup output fingerprints each. List available fields
with `vrrreplay --list-parameters`; `tests/vrr/replay-scenarios.example.json`
shows the configuration and assertion syntax.

```sh
vrrreplay capture.vrrtrace --config tests/vrr/configs/playout-delay-sweep-lowlatency.json --jobs 0 --output sweep.json
vrrreplay capture.vrrtrace --config tests/vrr/configs/production-stress.json --jobs 0 --output stress.json
```

The second sweep introduces decision, preparation/submission, and scheduler
faults and checks modeled interval safety and explicit latency ceilings. A
synthetic fault's latency is not normal operating latency. The low-latency and
smoothness sweeps vary the input buffer cap within current policy. Batch replay
runs independent scenarios concurrently, with an automatic cap of 16 jobs; use
`--jobs 1` when debugging. An assertion failure is an unsuccessful run.

For comparisons inspect presented-interval jerk and sender-spacing fidelity,
as well as latency, drift, interval violations, and any available raster bounds.
A saturated fixed-admission worker model cannot shed frames as the live worker
would; its latency is not a valid policy result. CPU submission spacing, OS
display reports, and physical panel output are separate evidence layers.
Compare gaps to the workload and actual source cadence rather than the requested
FPS alone; idle output and rate changes can legitimately produce long intervals.
Raster models describe stated timing assumptions and are not optical tear
measurements. See `AGENTS.md` for the complete replay workflow and limitations.

## Native macOS Metal and PyroWave integration smoke test

Build this target explicitly after the application has produced
`build/mac-client/pyrowave/libpyrowave.a`. `CONFIG+=pyrowave` opts into the GPU
codec cases; the ordinary native target does not compile the codec. Run from
the repository root:

```sh
nativeRoot="$PWD"
mkdir -p build/mac-metal-pyrowave-smoke
cd build/mac-metal-pyrowave-smoke
"$nativeRoot/build/vrr-hybrid/qt/6.11.1/macos/bin/qmake" \
  "$nativeRoot/tests/vrr/metalpresenter.pro" \
  QMAKE_APPLE_DEVICE_ARCHS=arm64 CONFIG+=release CONFIG-=debug \
  CONFIG+=pyrowave \
  "PYROWAVE_STATIC_LIBRARY=$nativeRoot/build/mac-client/pyrowave/libpyrowave.a"
make -j6
DYLD_LIBRARY_PATH="$nativeRoot/libs/mac/lib" \
GRANITE_VULKAN_LIBRARY="$nativeRoot/libs/mac/lib/libMoltenVK.dylib" \
  ./tst_metalpresenter "$PWD/native-metal-pyrowave.vrrtrace"
DYLD_LIBRARY_PATH="$nativeRoot/libs/mac/lib" \
  "$nativeRoot/build/mac-tests/vrr/vrrreplay" \
  "$PWD/native-metal-pyrowave.vrrtrace" --require-exact-baseline \
  --output "$PWD/native-metal-pyrowave-baseline.json"
```

The test opens a temporary native fullscreen window. On a variable display it
first verifies rejection of windowed and V-sync-disabled VRR, then exercises
the actual Metal presenter with software and VideoToolbox frames. The two
combined codec cases encode 128×96 patterns on the GPU and decode to exported
Metal textures in 4:2:0/8-bit and 4:4:4/10-bit. Their AVFrames have no CPU plane
data. The presenter waits for asynchronous decode, renders, cancels, prepares
again, frees the source frame and decoder, and presents the retained drawable.
A shared-frame fixed/CAMetalDisplayLink case also runs after VRR fallback.

The separate 100-frame worker fixture uses synthetic software frames at 116 FPS
on the 120 Hz panel and records real command-completion and presented-time
feedback for exact replay. On a fixed display the adaptive cases are skipped
after verifying fixed fallback. These tests establish native pipeline behavior;
the PyroWave GPU round-trip suite covers full-resolution plane equality, while
live streaming, external-display scanout and sustained performance require
separate validation.

## Timestamp pacing

`tst_timestamptrace` covers timestamp pacing's `.tstrace`, which shares the
VRR trace's file handling (`TraceFile`): paths, concurrent producers, footer
accounting, connection archiving, CSV output, and a real `TimestampPacer` on a
synthetic V-blank grid.

### Timestamp pacing replay

`tsreplay` (`tsreplay.pro`, library in `tsreplay.h`) replays a `.tstrace`
through `TimestampPacing::Policy`. `vrrreplay` cannot read these traces.
Compressed and plain CSV traces both work, including those expanded by
`decode-vrr-trace.py`.

```powershell
.\build\ci-tests\vrr\release\tsreplay.exe Moonlight.tstrace --require-exact-baseline `
    --output .\build\ts-baseline.json
.\build\ci-tests\vrr\release\tsreplay.exe Moonlight.tstrace `
    --config tests\vrr\configs\tsreplay-buffer-sweep.json --output .\build\ts-sweep.json
# After a policy change, rebuilt:
.\build\ci-tests\vrr\release\tsreplay.exe Moonlight.tstrace `
    --compare .\build\ts-baseline.json --output .\build\ts-candidate.json
```

- **Exact baseline.** The policy is pure, and every `scheduled` row records
  its inputs: frame number, RTP timestamp, readiness, host latency and
  decoder queue time. Replaying the session's settings must therefore
  reproduce every recorded decision. The fields checked are paced, admitted,
  repeat, restarted, reanchored, both targets, buffer, lateness, correction,
  host jerk, source period, and the pacer's late flag (target against
  readiness plus recorded render lead).
  - The baseline and the trace's integrity are checked together: footer,
    clean shutdown, no dropped rows, decoded SHA-256, and a complete sequence.
  - `--require-exact-baseline` exits 2 unless both hold. The JSON reports
    `fidelity.baseline_exact`, `fidelity.strict_ab_valid`, per-field mismatch
    counts and the first mismatch.
  - A capture recorded under an older policy reports which field diverged
    first. Its results are exploratory, not a strict A/B.
- **Scenarios.** Every run scores `session-policy` (the capture's settings),
  `pacing-off` (each frame shown once rendered) and `pacing-stock`
  (Moonlight's own pacer, the baseline timestamp pacing replaces). `--config` adds named
  scenarios, and `--set KEY=VALUE` adds one called `candidate`. Each starts
  from the session's settings. The keys (`--help` lists them):
  - pacing, smoothing, target_per_mille, min_buffer_ms and max_buffer_ms
    (the ranges `TimestampPacingOptions::resolved()` allows)
  - display (session, fixed or vrr), display_hz
  - render_us (default "recorded": each frame's recorded render lead)
  - phases (grid phases averaged on fixed refresh, default 8)
  - buffer_lead_us (default "recorded"): the render lead given to the
    policy, which sizes its buffer from readiness plus the lead. The pacer
    gives it each frame's recorded lead. 0 sizes the buffer from readiness
    alone, as the policy did before 2026-10-18. The model judges misses by
    the same recorded lead, so the gain from the lead is partly by
    construction; confirm it on the display.
  - hold_ms (default "level": 1000 at Light, 2000 at Standard, 0 at Off):
    under a frame limit, how long a refresh of delay may be held instead of
    replacing a frame. 0 replaces, as before 2026-10-22.
    `configs/tsreplay-fifo-hold.json` compares both at each level.
  Unknown keys and out-of-range values are errors.
- **Presentation model.**
  - **Fixed refresh:** a paced frame takes the V-blank its target falls on,
    through the policy's `PhaseLock`. Ready plus render time too late for
    it, the frame takes the next V-blank. A frame due no later than the one
    before replaces it (mailbox). The model is averaged over grid phases.
  - **VRR:** a frame is shown at max(target, ready + render), no sooner than
    one refresh after the previous one.
  - **Frame-limit hold:** in spans the trace recorded as frame limited
    (FIFO), a paced frame due no later than the one before is shown at the
    next V-blank instead of replacing it, as
    `TimestampPacer::holdsFrontLocked()` does, until it would be two
    refreshes late or the hold has lasted `hold_ms`. The frame is then
    dropped, which gives the refresh back. Held frames are counted as
    **held**, and the timeline marks them.
  - **Stock** (`pacing: stock`, fixed refresh only; on VRR it is `off`):
    Moonlight's own pacer, which `off` is not. `off` shows only the newest
    frame at each V-blank, so two frames due at one V-blank always cost a
    skip; the pacer's queues often show both a refresh apart instead, for a
    refresh of latency. With a V-sync source (the trace has `vsync` rows) it
    follows `Pacer::handleVsync`: one queued frame per V-blank, waiting up to
    a refresh less 3 ms for one, dropping only to catch up after half a
    second of a deeper queue. Without one (Gamescope), it follows the
    recorded display mode: under a frame limit (FIFO) every frame takes its
    own refresh in order, otherwise the newest ready frame is shown. The
    queue bound is the pacer's three frames, without the swapchain's.
  - **Not modeled:** the release thread, learned margins, late shift,
    Gamescope frame limits (outside the stock model), the display-mode
    probe, GPU or compositor. So
    the scores rank policies on one capture; they don't predict display
    timing. On the 2026-10-08 Deck captures, modeled jerk over 2 ms matched
    the recorded display reports on two captures and ran 2–5x low on
    captures recorded under a Steam frame limit.
- **Metrics,** per scenario:
  - **Presented jerk:** the change in presented interval between
    consecutive scored pairs, and the share over 2 ms. This is the headline
    smoothness measure.
  - **Cadence residual:** presented interval minus the host's RTP interval.
    It measures fidelity to host stamps, which jitter, so on a fixed grid
    it stays high even when the cadence is smooth.
  - **late:** the pacer's definition, ready plus render after the target.
  - **missed_plan:** ready plus render after the planned V-blank, so shown
    later.
  - **skipped:** replaced before shown.
  - **held:** shown a refresh late by the frame-limit hold.
  - **phase_wraps_per_phase:** fixed refresh only. How often the phase
    lock's trim wrapped, moving every following frame a refresh, per grid
    phase. Drift alone calls for about one wrap per beat (1 / rate
    mismatch). More than that is chatter or wraps driven by the buffer.
  - **delay and buffer:** shown minus ready, and the policy's buffer.
- **Scoring rules.**
  - Intervals are judged host step by host step across skipped frames. A
    step over 25 ms (`long_source_intervals`) leaves its interval out of
    the scores, as `vrrreplay` does. That is a metric exclusion, not a
    finding that the host stalled: idle or lower-rate output produces such
    steps legitimately.
  - Steps touching host repeats or invalid timestamps are
    `uncomparable_intervals`.
  - `source` summarizes the host cadence on its own.
  - `recorded` scores the capture's own display reports, matched to frames
    through their release time. These are OS display reports, not optical
    measurements.
- **Late shift.** Under Gamescope (the session's `compositor_probe`), the
  summary's `late_shift` replays `TimestampPacing::LateShift` from the
  releases, display reports and display mode changes, in sequence order.
  - Each release's prediction is rebuilt from readiness, its recorded
    release lead, and its V-blank less the refresh a recorded shift added.
  - `current_rule` must match a capture made with the confirmation rule.
    `prediction_only` matches captures made before it, and shows where the
    new rule would not have shifted.
  - After the first point where the two differ, outcomes still come from
    the recorded run, so later entries are indicative only.
- **Compare.** `--compare` matches scenarios by name and refuses a summary
  of a different trace (by file SHA-256). `--timeline` writes per-frame CSV
  for every scenario at grid phase 0.

`tst_tsreplay` records a trace from a real `TimestampPacer` and requires an
exact baseline. The stream it uses has arrival jitter, stamp noise,
unreported latency, host repeats, a host clock step, an invalid timestamp
and a frame-number gap. The test also checks:
- a different setting is not exact;
- an edited decision is found at its sequence, and breaks the hash;
- the model on steady, late, VRR-floor and long-source-step streams;
- config validation.
