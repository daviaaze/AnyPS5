# Tracing

Set a switch before starting the emulator. Unless noted, any value enables it. Most output goes to the emulator log or standard error. Performance captures are written to files as described in [Build](BUILD.md#performance-trace). Traces can be large and can affect timing.

## Kernel and libraries

| Switch | Output |
| --- | --- |
| `APS5_TRACE_AFFINITY` | Thread role, host thread ID, requested CPU mask and whether pinning succeeded. |
| `APS5_TRACE_AJM` | Submitted audio jobs, decoded output and unimplemented AJM calls. |
| `APS5_TRACE_APR` | APR file reads, including path, offset, size and destination. |
| `APS5_TRACE_AUDIOOUT` | Audio output port parameters when a port is opened. |
| `APS5_TRACE_AUDIOOUT2` | AudioOut2 context and port parameters, attributes, pushes, queue polls and periodic queue summaries. |
| `APS5_TRACE_CALLS` | On Windows and Linux, records calls to the imports selected by this comma-separated library list (or `all`); crash and abort reports (abort only on Linux) include the last 64 calls and most-called imports. The executable must be converted with `--trace-imports`; see [Usage](../user/USAGE.md#tracing-system-library-calls). |
| `APS5_TRACE_EXIT` | Guest `exit` and `_Exit` calls and process-exit backtraces. |
| `APS5_TRACE_FIBER` | Fiber initialization, switches and returns. |
| `APS5_TRACE_MEMORY` | Direct-memory allocation, mapping and protection operations. |
| `APS5_TRACE_PREPARATION_WAIT` | Shader preparation tasks moved to the front of the queue to satisfy a waiting caller. |
| `APS5_TRACE_SSE4A` | Each emulated SSE4A instruction once per guest instruction address (Windows). |
| `APS5_TRACE_SYNC` | Event-flag operations and their wait and wake results. |
| `APS5_TRACE_TIMERS` | Timing measurements for the host wait primitives used by the emulator. |
| `APS5_TRACE_USLEEP` | Every 2,000 guest `usleep` calls, with caller addresses and requested delays. |
| `APS5_TRACE_VIDEODEC` | Video decoder API calls and decode results. |
| `APS5_TRACE_WAITS` | Blocking waits aggregated by guest call site, including timeout counts and longest waits, every three seconds. |

## GPU and memory

| Switch | Output |
| --- | --- |
| `APS5_TRACE_BIGBUF` | Each copied or written-back guest-memory range of at least 1 MiB. |
| `APS5_TRACE_BDA` | Shader memory accesses that make a program use the buffer-device-address path. |
| `APS5_TRACE_BUFFER_WRITTEN` | Guest buffer elements classified as writable or read-only after descriptor population. |
| `APS5_TRACE_CAPSYNC` | Waits for GPU writes needed by CPU accesses, including submission and range details. |
| `APS5_TRACE_CONTEXT_STATE` | PM4 context-state operations and registers written while context state is pushed. |
| `APS5_TRACE_COPY_HLE` | Pending host-side copies, their ranges, GPU batches and known writers. |
| `APS5_TRACE_DCC_KEYS` | Display and storage-image DCC key reads, writes, fills and clears. |
| `APS5_TRACE_DISPATCH_CACHE` | Compute dispatch cache hits, misses and changed user-data words. |
| `APS5_TRACE_DISPATCH_IO` | Compute dispatch queue, submission serial, shader and resources; value `2` also prints small user-data buffers. |
| `APS5_TRACE_DRAW_CACHE` | Draw-cache lookups, hits, misses, and key differences, reported every ten seconds. |
| `APS5_TRACE_DRAWS` | Draw inputs and per-draw timing phases. With `APS5_PROFILE_DRAW`, also prints the detailed draw lines. |
| `APS5_TRACE_DRAW_PROGRAMS` | One line per draw listing selected shader stages, roles, code addresses and entry addresses. |
| `APS5_TRACE_FILL_COVER` | Initial fill/image coverage mismatches and refused image-layer clears. |
| `APS5_TRACE_FLUSH` | Storage images flushed to guest memory and ranges left for the CPU. |
| `APS5_TRACE_GPU` | Queue submissions, packet execution, waits and display-buffer scanouts. |
| `APS5_TRACE_HOST_IMPORT` | Host-import mapping attempts, memory-map details, failures and retirement. |
| `APS5_TRACE_IMAGE_TABLE` | Image-table resource materialization and descriptor selection. |
| `APS5_TRACE_INDIRECT_DRAWS` | Indirect draw packets that pass the draw precheck. |
| `APS5_TRACE_LABEL` | `RELEASE_MEM` packets that target the address in this hexadecimal value. |
| `APS5_TRACE_LATE_LABELS` | GPU label writes observed after their waiting submission was handled. |
| `APS5_TRACE_QUERY` | Every sixteenth guest-memory page query, including cost and caller address. |
| `APS5_TRACE_RESOURCE_KEY_MISSES` | Resource-content key changes that cause draw or dispatch resource-cache misses. |
| `APS5_TRACE_SHADOW` | Unit-shadow publish, seed, eviction and retirement events; a number limits publish output. |
| `APS5_TRACE_SKIPS` | GPU operations skipped by diagnostics, with a bounded number of recompile-request dumps per reason. |
| `APS5_TRACE_STAGING` | Distinct guest-buffer ranges admitted to the write-staging window. |
| `APS5_TRACE_TEXTURES` | Sampled texture addresses, dimensions, format and nonzero-byte counts. |
| `APS5_TRACE_UNREADABLE` | Call sites that check inaccessible guest-memory ranges or fail to read them. |
| `APS5_TRACE_UPLOAD` | Reasons storage-image data is uploaded again. |
| `APS5_TRACE_SLOW_VULKAN_CALLS=1` | Sparse diagnostics for `vkQueueSubmit` and `vkQueuePresentKHR` entrypoint calls taking at least 5 ms. Records monotonic entry/exit times, calling-thread CPU samples when available, PID/native TID, device/queue handles, batch or image identity, Vulkan result and a qualified in-flight count. |
| `APS5_TRACE_BARRIERS` | Set to a positive integer to print that many skipped barrier decisions and their ranges. |

## Performance captures

See [Build](BUILD.md#performance-trace) for the trace format, summary fields and capture limits.

| Switch | Output |
| --- | --- |
| `APS5_TRACE_FRAMES=<start>:<count>` | Records a window of presents by zero-based frame number. |
| `APS5_TRACE_SECONDS=<start>:<length>` | Records a window of presents by elapsed seconds after the first present. Cannot be set with `APS5_TRACE_FRAMES`. |
| `APS5_TRACE_SUMMARY_EVERY=<seconds>` | Writes periodic summary lines without recording trace events. Can be combined with either window switch. |
| `APS5_TRACE_CAUSAL=1` | Adds bounded GPU-mutex, resource-preparation and Vulkan API causal events to a frame/seconds trace. Summaries alone do not enable causal storage. |
| `APS5_TRACE_EVENTS=<count>` | Sets each thread's event-buffer capacity and the GPU-range capacity for a frame or seconds window (1–4,194,304; default 262,144). |
| `APS5_TRACE_DIR=<path>` | Writes performance capture and summary files to this directory instead of the working directory. |
| `APS5_VIDEOOUT_TELEMETRY_DIR=<path>` | Writes the presents and the pad changes of a run to `<path>/videoout.jsonl`; see [Usage](../user/USAGE.md#video-out-and-pad-telemetry). |

`python3 tools/trace_digest.py <trace.json.gz> --check-present-window 0 60000 33.3` checks every presentation gap covering a 60-second trace-relative window, including gaps crossing its boundaries. It exits 0 only when all gaps are at most 33.3 ms, 1 for a measured threshold failure, and 2 for invalid or incomplete evidence. A matching summary with no dropped events and consecutive presentation IDs is required. Window endpoints must be covered by recorded presentations; a summary's trailing duration is not a substitute. Select endpoints using independent scene evidence: this timing check cannot establish that the tutorial or any other scene is visible.

A window of N complete frame intervals emits N+1 `present` boundary witnesses with consecutive IDs and timestamps spanning both ends of the capture. The final witness records the closing boundary; it is not an additional completed frame. The summary's `presents` count remains N.

The digest uses the actual timestamp of the closing witness whose ID equals `window.first_present + presents`, rather than the rounded summary duration. Rounding that duration upward must not create a tiny extra frame interval. Historical captures without that witness retain the summary endpoint for digest reporting; strict presentation-window checks still require recorded endpoint coverage.

The compressed Chrome-trace JSON keeps the existing `traceEvents` array and adds a top-level `clock` object: `origin_ns` is the nanosecond count of the exact `steady_clock` origin subtracted from each event timestamp. `domain` is `CLOCK_MONOTONIC` on supported Linux standard libraries and `steady_clock` elsewhere. Each emitted `thread_name` metadata event includes `args.native_tid` when the registering platform exposes a native thread ID (Linux TID or Windows thread ID); unsupported platforms omit it. These fields let external perf records be joined without treating synthetic Chrome trace thread IDs as OS IDs.

With `APS5_TRACE_CAUSAL=1`, causal Chrome events use category `causal`, `object` (GPU mutex identity is `1`), `generation` (outer hold generation), `related` (exact preceding released hold generation or `0` when unknown/not applicable), and `operation` (attempt ID shared by attempt, wait, and acquisition; outer release uses `0`). `site` is the lock site, `recursion` is recursive depth, and `outcome` is the API result when applicable (`INT32_MIN` means unavailable/not applicable; zero is a real success result); `batch` retains its ordinary batch-ID meaning. `gpu_mutex.attempt` is emitted before trying the lock; `gpu_mutex.try_failed` records a failed nonblocking attempt; `gpu_mutex.wait_begin` is emitted only after the initial try has failed and immediately before blocking/spinning. `gpu_mutex.acquired` records recursion depth; `gpu_mutex.released` is a duration interval bracketing the actual outer unlock call, not an assumed point timestamp. A contended `gpu_mutex.wait` span starts at the failed try and ends after acquisition; its `related` field links only to a release generation observed to advance during that wait, otherwise it is `0`. The lock-release boundary proves mutex unlock, not GPU-fence completion. Each thread's `native_starttime_ticks` is Linux `/proc` start-time field 22 when available, or `null`; unsupported/missing identity remains unavailable. Causal events have separate bounded storage capped at the existing `APS5_TRACE_EVENTS` setting and at 262,144 entries per thread; they are never folded into short slices. `causal coverage lost` records the first overflow timestamp, count, configured capacity, and last retained timestamp; all causal coverage after that first loss through the capture-window end is incomplete, and the trace is marked dropped. GPU queue/fence completion does not identify a CPU releaser; absent explicit Vulkan producer evidence remains unresolved.

Completed `graphics.resources` spans retain the existing draw/dispatch preparation stages. `vulkan.queue_submit`, `vulkan.fence_wait`, `vulkan.timeline_wait`, `vulkan.fence_reset`, `vulkan.acquire_image`, and `vulkan.queue_present` bracket the actual resolved API calls on the same monotonic clock. Their nullable `thread_cpu_ns` values measure bracketed CPU time, not GPU execution. Each API attempt emits an immediate zero-duration boundary with unavailable outcome; a completed span with the same operation records the real `VkResult`. Boundaries are not completed durations, and overlapping or nested spans must not be summed.

`gpu.fence_submitted` ties a fence generation to its successful queue submission and batch; resets and reuse retain distinct generations. `gpu.fence_observed_complete` and timeline completion events identify the CPU observer, never a CPU signaler. Acquisition fences have no recorder batch (`batch=0`). Missing registered owner lanes, birth identity, producer/release boundaries or overflow keep a dependency unresolved. These events do not establish that a guest consumed input, loaded a map, or reached a playable scene.

Slow-Vulkan wall intervals include only the resolved Vulkan entrypoint call, excluding error handling, device-idle recovery and subsequent recorder bookkeeping. Thread CPU samples bracket the wall interval and also include the clock-read boundary overhead; unavailable readings are printed as `unavailable`, not zero. Neither interval is GPU execution time. The older periodic recorder submit aggregate has a wider timing boundary and is not interchangeable with these records.

Slow-Vulkan output has two process-wide rate lanes: at most one record for calls taking at least 5 ms but less than 100 ms, and one at-least-100 ms stall record per monotonic second, shared across threads and the two APIs. Later windows remain eligible instead of exhausting a startup-only record limit. Each record includes cumulative rate-limited counts and maximum suppressed wall times; this is sparse diagnostic evidence, not a complete API-call trace. Present in-flight counts describe the slot snapshot at blit submission, while recorder counts describe batches before submission. Logging is opt-in; the disabled path adds no clock reads or sample initialization.

### External Linux CPU sampling

For userspace-only `perf record` sampling, combine `-e cpu-clock:u --call-graph dwarf,4096` with `--user-callchains`. The event's `:u` modifier excludes kernel execution, but does not itself set `exclude_callchain_kernel`; a recorder that still requests kernel callchains can fail with `EACCES` under `perf_event_paranoid=2`. Explicit userspace callchains avoid that request without changing host permissions. Kernel stack collection is separate and may still be unavailable.

When parsing `perf script --ns -F pid,tid,time,event,period`, allow leading whitespace before the PID: shorter process IDs are left-padded. Require nonzero samples for the verified target PID inside the enable/disable ACK window; a nonempty data file or successful recorder exit does not establish usable samples.

To render the recorded user callchains as well as sample headers, use `perf script --ns -F pid,tid,time,event,period,ip,sym,dso`. Omitting `ip,sym,dso` can hide valid stored callchains. Samples contain no thread-birth identity; preserve that limitation rather than declaring a verified lifetime join from TID alone.

### Kernel scheduler captures

Task-scoped `sched:sched_switch` recordings need separate kernel permissions; userspace CPU sampling does not grant them. Pass an explicit unit to `perf record --max-size`, such as `32M` for a soft 32 MiB limit. A byte-count-only argument can be rejected by the CLI. Keep an independent deadline and report actual output bytes rather than claiming a hard file-size cap.

Scheduler validation must recognize both raw `prev_pid`/`next_pid` fields and formatted `comm:pid [priority] state ==> comm:pid [priority]` records, while still requiring a matching target PID/TID header. A collector-requested, identity-verified `SIGINT` can finalize perf with return code `-SIGINT`; that result is usable only after checking the finalized data, target scheduler events, kernel callchains and lost-event count. Captured kernel addresses may remain unsymbolized even when blocked-thread `/proc` stacks expose function names. A successful synthetic kernel capture is not title root-cause or gameplay proof.

## Shader registers

`APS5_TRACE_SHADER_REGS=all` prints register snapshots for every shader. Set it to a shader code address in hexadecimal to print one shader's registers.
