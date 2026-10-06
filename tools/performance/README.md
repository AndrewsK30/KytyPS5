# Native renderer diagnostics

Set `KYTY_PERF_CSV` to a writable CSV path before launching the emulator. Unset it to disable collection. No synchronization behavior or shaders are changed by diagnostics. The current experimental interrupt batching stays disabled in the capture harness.

The CSV combines `present` rows with one-second `sample` rows so stalled rendering can still be observed. Counters are **deltas since the preceding row**, aggregated from one-writer thread shards. Sum both row types for totals; count only `present` rows for presentation rate. Concurrent snapshots are approximate at interval boundaries; totals remain monotonic. A successful presentation is not necessarily a distinct guest frame. Completed guest flips are counted separately.

All `*_ns` fields measure **host wall time**. They can overlap on different threads and must not be added as a frame-time breakdown. Completed scopes are attributed at completion, so an in-flight wait only appears once it returns. This first version does not include GPU timestamps, per-shader GPU costs or exact ownership of each counter by a guest frame. It measures buffer upload/readback and image memory readback bytes, not every internal GPU copy.

Origin counters split buffer readbacks into requesting CPU reads/writes, requesting GPU-thread reads/writes, color metadata, cache GC and unknown. The source is captured before SendCommandSync and restored around the GPU-thread callback, so CPU requests remain attributed to CPU. Byte counters include packed-download padding, consistent with `buffer_readback_bytes`. A snapshot boundary can split the total and source increments into adjacent rows. Fault request size can be one byte because the handler receives only a faulting address; it does not describe the instruction's full footprint.

Semaphore timings are also mirrored into GPU-thread, completion-worker and other-thread categories with the same timer. Completion-worker waits are asynchronous relative to command recording, so a large accumulated completion wait is not equivalent to GPU-thread blocking. RELEASE_MEM counters distinguish no-data, dword, qword, timestamp and GDS packets.

Readback phase counters separate dispatch (caller start to GPU callback entry), preparation
(resource lookup and recording copies, including any internal download-ring waits), the
scheduler wait, and waiting for priority callbacks to publish the downloaded bytes. CPU
ownership marking and wakeup overhead also appear in the enclosing CPU sync time. Requests
are counted even if a previous request has already downloaded their data.

`KYTY_PERF_READBACK_CSV` enables bounded interval samples together with `KYTY_PERF_CSV`:
one in 64 requests per requesting host thread, at most 20,000 rows per process. The harness
sets both paths and disables both with `-DisableDiagnostics`. Samples include addresses,
buffer/window bounds, scheduler ticks, phases, actual payload bytes, padded bytes and copy
counts; they never include guest memory contents. Sampling has thread-dependent bias and
sample sums are not full-run totals. Scheduler ticks are not last-writer versions or guest
frame IDs, so repeating an address/tick does not establish that a cached result is reusable.

```powershell
python tools/performance/readbacks.py _Build/windows/benchmarks/native-TIMESTAMP/readbacks.csv --start-seconds 120 --end-seconds 180 --output readbacks.json
```

Capture from PowerShell:

```powershell
./tools/performance/capture.ps1 -Game 'D:\path\to\game' -Scenario 'Opening cinematic, no input; exploratory startup capture' -DurationSeconds 180
```

The harness launches its own process and stops that process at the end. Results are under `_Build/windows/benchmarks/native-*`. It restores the caller's environment. Record a matching scene/time window for an A/B comparison; elapsed time since process launch alone does not identify gameplay. Use `-DisableDiagnostics` to measure collection overhead with the same executable and otherwise identical conditions. Do not compare instrumentation-on and off runs unless their scene matches.

Analyze a selected 60-second interval (Python 3, standard library only):

```powershell
python tools/performance/analyze.py _Build/windows/benchmarks/native-TIMESTAMP/renderer.csv --start-seconds 120 --end-seconds 180 --output report.json
```

The analyzer excludes partial deltas crossing the requested start/end. It reports the actual interval included. Shader compilation counts reveal whether the selected interval still contains compilation; a warm/stable label requires checking that and the scene.

Build and test:

```powershell
cmake --build _Build/windows --target kyty_emulator performance_diagnostics_tests --parallel 16
./_Build/windows/performance_diagnostics_tests.exe _Build/windows/benchmarks/diagnostics-test.csv
./_Build/windows/performance_diagnostics_tests.exe --off
python tools/performance/test_analyze.py
```

The capture manifest records binary hash, tracked patch, untracked sources, Git revisions, configuration, arguments and environment. A dirty build can disable the pre-existing Vulkan driver cache; inspect startup logs and treat cache state as part of the comparison. Keep collected game paths and generated artifacts local.


## Deterministic offscreen 3D renderer benchmark

Build `shader_recompiler_compute_tests`, then run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/performance/renderer.ps1 -Frames 180 -Draws 256 -Runs 3
```

No game files or intro are needed. The workload renders a fixed perspective/depth grid of
64 cubes at 512x512 through production pipeline/binding/target/scheduler paths. It runs
three profiles: one rendering submit, submits every four draws, and a 4 MiB image readback
using the production download ring. Draw count must be a multiple of 64; increase it to
4096 for 1024 renderer submits per frame in the submits profile. Geometry/upload/compiler
setup is excluded, each profile warms for at least 16 frames and 2 seconds, and one frame
is in flight. A native vertex fixture and RDNA2 pixel shader are used; guest CPU/PM4/fault
handlers and game's rendering complexity are outside this integration workload.

The wrapper records exact source and binary hashes, archives the executable and adjacent
DLLs, and restores diagnostics environment variables. Use a fresh output directory; never
build/run another GPU test or game during a capture. JSON reports include per-frame
distributions, median ranges across repeated runs, actual warmup count, GPU metadata,
workload version and image checksum. `scene.png` is the actual rasterized preview.

`gpu_span_ms` includes queue idle gaps between multiple submits. CPU waits overlap GPU;
do not add those values or label synthetic throughput as game FPS. Version `cube-grid-v2`
uses the production download ring; the earlier v1 staging fixture is a different workload.
For A/B, use matching signatures and inspect variability/checksums before attributing a
change to the emulator. Whole-game performance still needs a matching scene.

CTest `renderer_benchmark_smoke` verifies the three profiles render the same nonblank
image and emit the expected measured frames. The Python tests reject incomplete,
nonfinite, mismatched and mixed-workload evidence. No noisy FPS threshold is asserted.

## Wait sites, submit reasons and buffer churn

`renderer.csv` now ends with `wait_<site>_<field>` columns. Fields: `calls` (every wait,
including ones whose tick/predicate was already satisfied), `blocking`, `ns` (blocked host
time), `lag_ticks` (submitted-but-incomplete ticks when a semaphore wait started) and
`drains` (waits that first had to submit the open command buffer). Semaphore waits take
their site from a thread-local `WaitSiteScope`; untagged ones fall back to the readback
source (`readback_cpu_read`, `readback_cpu_write`, ...). Condition-variable waits
(`publish_wait`, `readback_dispatch`, `command_sync`, `gpu_label_blocked`, `videoout_*`)
use `WaitTimer`. With Tracy connected, each blocking semaphore wait is a `Wait` zone named
after its site.

`submit_<reason>` splits renderer submits (label, processor, flip, drain, finish, shutdown,
other). `gpu_process_ns`, `gpu_command_ns` and `gpu_idle_ns` budget the GPU thread.
`buffer_create*`, `buffer_join*`, `buffer_delete`, `vma_buffer_create*`, `dedicated_alloc`,
`bda_table_upload`, `image_create` and `tiler_scratch_alloc` count allocation churn.

```powershell
# Ranked wait sites for a scene bounded by presentation ids rather than launch time.
& G:/kity_work/tools/python313/python.exe tools/performance/waits.py CAPTURE/renderer.csv --start-flip 1200 --end-flip 1800 --table
# Calibration run: window screenshots named t<seconds>s-p<presentation>.png.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/performance/capture.ps1 -Game GAME -Scenario 'calibration' -ScreenshotEverySeconds 5
```

Blocked times of different threads overlap; rank sites, do not add them into a frame cost.
`analyze.py` also accepts `--start-flip/--end-flip`.
