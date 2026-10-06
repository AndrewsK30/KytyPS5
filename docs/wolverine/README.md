# Wolverine performance work (stacked branches)

Marvel's Wolverine (PPSA03671 v01.001.005) on KytyPS5. Everything here is measured on one machine
(Ryzen 7 7800X3D, RTX 4090) in two scenes: the INITIAL SETUP menu and the helicopter cutscene.
`WOLVERINE_SESSION.md` is the log (instructions, evidence, limits, next steps); the other
`WOLVERINE_*.md` files are the plans and reports it points to. Hypotheses and proven results are kept
apart there; a gain counts only after a same-scene comparison.

## Branch stack

Each branch builds on the previous one. Nothing is pushed anywhere; the base is
`wolverine-performance` (`1e2185db`).

| Branch | What it adds | Default behaviour change |
|---|---|---|
| `wp/00-docs` | These documents | none |
| `wp/01-diagnostics` | `PerformanceDiagnostics` (waits by call site, submit reasons, GPU-thread categories, causality probe, label-wait CSV), `tools/performance`, tests and an offscreen renderer benchmark | none (off unless `KYTY_PERF_CSV`) |
| `wp/02-label-flush` | `KYTY_LABEL_FLUSH_EVERY` (labels submitted at the end of the PM4 slice, not per label) | default 0 |
| `wp/03-label-pending-visible` | A label published at GPU completion is visible to the GPU thread's own `WAIT_REG_MEM` once recorded | on; `KYTY_LABEL_PENDING_VISIBLE=0` turns it off |
| `wp/04-async-submit` | `KYTY_ASYNC_SUBMIT=1`: a submit thread for finished command buffers | opt-in |
| `wp/05-pipeline-cache` | Kyty's own pipeline cache also in dirty builds (`KYTY_PIPELINE_CACHE_NO_DIRTY=1` restores the refusal); `KYTY_PIPELINE_NO_OPT` (measured: no effect); slow pipeline log | on for the cache |
| `wp/06-readback-queue` | `KYTY_READBACK_QUEUE=1`: guest readbacks of finished GPU data on a second queue; GPU write tick tracking; `KYTY_READBACK_UPPER_BOUND=1` (unsafe, measurement only) | opt-in |

All the label work only matters with `KYTY_LABELS_AFTER_GPU=1`, which `tools/performance/capture.ps1`
sets in every capture; the numbers in the logs are for that mode.

## Headline measurements (same scene, same binary, only the environment differs)

* Menu: about 12.8 FPS before, about 18 FPS with the defaults of this stack.
* Title screen: 9.9 to 12.5 FPS.
* Helicopter cutscene, compared at equal presented-frame counts: roughly +30% to +64%
  (2 runs per side; the frame-count equivalence is an assumption).
* Cold start: about 76 s shorter with the pipeline cache (one run per side).

Not measured: gameplay after the cutscene and other games. `kernel_file_system` fails in CTest and
did before this work.
