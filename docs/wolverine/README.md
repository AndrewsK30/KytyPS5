# Wolverine performance work (stacked branches)

Marvel's Wolverine (PPSA03671 v01.001.005) on KytyPS5. Everything here is measured on one machine
(Ryzen 7 7800X3D, RTX 4090) in two scenes: the INITIAL SETUP menu and the helicopter cutscene.
`WOLVERINE_SESSION.md` is the log (instructions, evidence, limits, next steps); the other
`WOLVERINE_*.md` files are the plans and reports it points to. Hypotheses and proven results are kept
apart there; a gain counts only after a same-scene comparison.

## Branch stack

Each branch builds on the previous one (the stack order is 00, 01 ... 09). Nothing is pushed anywhere; the base is
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
| `wp/07-structure-metrics` | Per-queue GPU-thread time, a Materialize category, per-buffer readback/upload stats, `VirtualProtect` and fault counters | none (off unless `KYTY_PERF_*` files) |
| `wp/08-host-pointer-import` | `KYTY_HOST_IMPORT=1` + `KYTY_HOST_IMPORT_RANGES`: selected guest buffers use the guest's own memory (VK_EXT_external_memory_host), no upload or copy back, pages left unprotected; import probe | opt-in, needs `KYTY_LABELS_AFTER_GPU`; the range list is per title |
| `wp/09-dispatch-prep-once` | A dispatch prepares its bindings once unless a recovery snapshot needs the earlier pass (`KYTY_DISPATCH_PREP_TWICE=1` restores it); materialization repeat/read counters | on |

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

## Later measurements

* Importing one 70 MB buffer (0x12c0000000, written by the CPU, little used by shaders): menu 17.26 to 19.99 FPS
  (+15.8%), helicopter cutscene by presented frames +21% / +22% / +13%. Importing the 97 MB, 22 MB, 3 MB and 2.7 GB
  buffers as well dropped the menu to ~9.8 FPS: shader-heavy buffers are slow over PCIe. The list is per title.
* Preparing a dispatch's bindings once: menu +1.8% (17.81 to 18.13 FPS).
* Import plus the readback queue: 20.01 to 20.76 FPS (+3.7%, one slow outlier in the baseline, not conclusive).
* Not worth it, measured: memoizing MaterializeResources (2.0% of results repeat), DISABLE_OPTIMIZATION pipelines
  (no effect on this driver), a second readback queue alone (+1%). Threads per queue and a staged translation were
  assessed without code: nearly all draw/dispatch work runs under one render lock, and the caches would need to
  become thread-safe first.