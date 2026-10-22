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
| `APS5_TRACE_EVENTS=<count>` | Sets each thread's event-buffer capacity and the GPU-range capacity for a frame or seconds window (1–4,194,304; default 262,144). |
| `APS5_TRACE_DIR=<path>` | Writes performance capture and summary files to this directory instead of the working directory. |
| `APS5_VIDEOOUT_TELEMETRY_DIR=<path>` | Writes the presents and the pad changes of a run to `<path>/videoout.jsonl`; see [Usage](../user/USAGE.md#video-out-and-pad-telemetry). |

`python3 tools/trace_digest.py <trace.json.gz> --check-present-window 0 60000 33.3` checks every presentation gap covering a 60-second trace-relative window, including gaps crossing its boundaries. It exits 0 only when all gaps are at most 33.3 ms, 1 for a measured threshold failure, and 2 for invalid or incomplete evidence. A matching summary with no dropped events and consecutive presentation IDs is required. Window endpoints must be covered by recorded presentations; a summary's trailing duration is not a substitute. Select endpoints using independent scene evidence: this timing check cannot establish that the tutorial or any other scene is visible.

A window of N complete frame intervals emits N+1 `present` boundary witnesses with consecutive IDs and timestamps spanning both ends of the capture. The final witness records the closing boundary; it is not an additional completed frame. The summary's `presents` count remains N.

The digest uses the actual timestamp of the closing witness whose ID equals `window.first_present + presents`, rather than the rounded summary duration. Rounding that duration upward must not create a tiny extra frame interval. Historical captures without that witness retain the summary endpoint for digest reporting; strict presentation-window checks still require recorded endpoint coverage.

The compressed Chrome-trace JSON keeps the existing `traceEvents` array and adds a top-level `clock` object: `origin_ns` is the nanosecond count of the exact `steady_clock` origin subtracted from each event timestamp. `domain` is `CLOCK_MONOTONIC` on supported Linux standard libraries and `steady_clock` elsewhere. Each emitted `thread_name` metadata event includes `args.native_tid` when the registering platform exposes a native thread ID (Linux TID or Windows thread ID); unsupported platforms omit it. These fields let external perf records be joined without treating synthetic Chrome trace thread IDs as OS IDs.

Slow-Vulkan wall intervals include only the resolved Vulkan entrypoint call, excluding error handling, device-idle recovery and subsequent recorder bookkeeping. Thread CPU samples bracket the wall interval and also include the clock-read boundary overhead; unavailable readings are printed as `unavailable`, not zero. Neither interval is GPU execution time. The older periodic recorder submit aggregate has a wider timing boundary and is not interchangeable with these records.

Slow-Vulkan output has two process-wide rate lanes: at most one record for calls taking at least 5 ms but less than 100 ms, and one at-least-100 ms stall record per monotonic second, shared across threads and the two APIs. Later windows remain eligible instead of exhausting a startup-only record limit. Each record includes cumulative rate-limited counts and maximum suppressed wall times; this is sparse diagnostic evidence, not a complete API-call trace. Present in-flight counts describe the slot snapshot at blit submission, while recorder counts describe batches before submission. Logging is opt-in; the disabled path adds no clock reads or sample initialization.

### External Linux CPU sampling

For userspace-only `perf record` sampling, combine `-e cpu-clock:u --call-graph dwarf,4096` with `--user-callchains`. The event's `:u` modifier excludes kernel execution, but does not itself set `exclude_callchain_kernel`; a recorder that still requests kernel callchains can fail with `EACCES` under `perf_event_paranoid=2`. Explicit userspace callchains avoid that request without changing host permissions. Kernel stack collection is separate and may still be unavailable.

When parsing `perf script --ns -F pid,tid,time,event,period`, allow leading whitespace before the PID: shorter process IDs are left-padded. Require nonzero samples for the verified target PID inside the enable/disable ACK window; a nonempty data file or successful recorder exit does not establish usable samples.

### Kernel scheduler captures

Task-scoped `sched:sched_switch` recordings need separate kernel permissions; userspace CPU sampling does not grant them. Pass an explicit unit to `perf record --max-size`, such as `32M` for a soft 32 MiB limit. A byte-count-only argument can be rejected by the CLI. Keep an independent deadline and report actual output bytes rather than claiming a hard file-size cap.

Scheduler validation must recognize both raw `prev_pid`/`next_pid` fields and formatted `comm:pid [priority] state ==> comm:pid [priority]` records, while still requiring a matching target PID/TID header. A collector-requested, identity-verified `SIGINT` can finalize perf with return code `-SIGINT`; that result is usable only after checking the finalized data, target scheduler events, kernel callchains and lost-event count. Captured kernel addresses may remain unsymbolized even when blocked-thread `/proc` stacks expose function names. A successful synthetic kernel capture is not title root-cause or gameplay proof.

### Bounded workbench reproduction snapshot

[The 2026-10-22 source checkpoint 011](verified-scripts-20261022.patch) preserves the external Linux workbench used to verify the bounded launcher, authenticated process leases, headless capture ownership, ACK-bounded CPU collection and opt-in causal diagnostics. This ordinary UTF-8 new-file patch restores seventeen source/manifest files; no game assets, compiled libraries or encoded binary content are included. Patch SHA-256: `cda1e5acebd9ec7138dde57035e308111a28cfab55c17d52c1efb1d063cd90a7`. Its restored file bytes match external checkpoint 011, SHA-256 `d79d2e5721205f2352b6010dcce1118ca9918d85561e2c7494e4b27a95922ce0`.

Each committed, hash-pinned version is an immutable reproduction artifact, not an emulator component, standalone installer or supported general-purpose launcher. Apply it only in an empty scratch directory using `git apply /absolute/path/to/verified-scripts-20261022.patch`, then compare with the matching prepared workbench; do not apply over a live workbench. It requires that workbench's existing `shell.nix`, external tools and a legitimately supplied runtime. Its manifest pins the experimental library and exact executable; reconstructing a different library or runtime does not inherit verification. Never bypass a refused identity or permission check.

The optional `--wait-idle-backoff` path is separate from the earlier job-getter experiment and remains disabled unless explicitly selected. It preserves the original queue rescans, stop handling and watchdog path while using raw, bounded-request idle sleeps without retrying interruption. Native controlled worker/producer runs, register/flag sentinels with deliberate corruptions, TLS/nesting, interruption, cancellation, incompatible-executable refusal, real lease admission/drain and a userspace perf capture were exercised before checkpointing. Actual guest producers, transitive locks, upper AVX state and full guest unwind behavior were not established. This evidence permits only a short opt-in hypothesis test; it establishes neither title compatibility nor an FPS improvement. A partial perf window is reported as partial, not as coverage of its requested duration. No kernel-permission or global host configuration change is implied.

Checkpoint 007 extends only the bounded profile's maximum event window from 60 to 130 seconds, starting at the first present, so later scene loading need not fall outside the trace. Shorter runs retain the existing runtime/stop-margin clamp; CPU, memory, event capacity and ordered-stop policies are unchanged. The normal and capture24 policies matched the prior checkpoint in 721 cases; all 180 bounded durations were checked. An actual no-game native capture closed a `0:130` window and retained both boundary witnesses. Its deliberately controlled 130.1-second gap was correctly rejected for frame time, not missing coverage. This proves capture behavior, not FPS. A later present must still close the window before shutdown, and tutorial correlation plus the strict uninterrupted 60-second check remain required.

Checkpoint 008 adds opt-in `headless-input-step.py --capture-after-release`. Only after saving the real released input ACK does it open a pidfd, recheck the exact compositor identity, request a screenshot with `pidfd_send_signal(SIGUSR2)`, and close the fd. A separate `screenshot_request` failure preserves the saved `released` input status and makes the CLI exit 1. A returned signal request is not an image receipt or scene proof: obtain a fresh receipt from the exact owned compositor log, copy its image from the required private namespace, and inspect it separately.

The no-game `agc_driver_visual_test` smoke exercised the external screenshot sender against its owned gamescope and refused a wrong process birth identity. An exact new own-log receipt was copied 77,349,194 ns after syscall return; its actual triangle image was visually inspected. The 25-second bounded command timeout and helper exit 124 were intentional. A later actual game run (`native-game-4712f9009875`) exercised four real input ACK steps with the post-release request/copy branch. Its first immediate image still showed the old root menu, with Nexus visible later: a successful request is not a UI transition or FPS claim.

Checkpoint 009 archives screenshots as `<six-digit-log-line>-<source-name>` because gamescope source filenames have only second precision. In the real no-game collision reproduction, two same-second SIGUSR2 requests produced two owned log receipts with the same source path. Before the fix their shared archive path made the unchanged frame validator refuse with `Frame lacks exactly one compositor receipt`; after the fix, distinct `000079-` and `000080-` archive paths passed the same validator. The decoded triangle was visually inspected. This exercised standalone validation of the actual owned visual target, Wayland and bridge, not native game leases or input ACK dispatch. It does not guarantee a distinct exposure for every log receipt if gamescope overwrites the source before collection. Earlier fixture attempts failed for missing bridge environment and missing Vulkan library; the corrected Nix bridge fixture reproduced the collision. The final bounded helper ended with code 1 after the fixture service reached its 40.198-second runtime timeout and SIGTERM; the collision proof and frame validation were collected while it was live. No timeout success or FPS result is inferred from that collision proof.

Checkpoint 010 adds `causal-analyze.py`, `causal-analyze-fixture.py` and `causal-native-smoke.py`, strict causal admission guards, and explicit owned headless slice inheritance. Source usage is `nix-shell shell.nix --run 'python3 causal-native-smoke.py evidence/causal-followup/SDK_RUN'`; run it inside an owned initial-namespace user unit with NoNewPrivileges, the shared engineering slice capped at four CPUs, 14 GiB memory high/16 GiB maximum, zero swap and a 60-second bound. The script itself never starts systemd. Analyze with `python3 causal-analyze.py RUN_DIR --start-ns START --end-ns END` using exact CLOCK_MONOTONIC boundaries and a trusted perf executable from PATH or `--perf`; never execute an executable supplied by an artifact.

The real no-game SDK smoke retained three perf samples over 1.614417510 seconds of an ACK-bounded partial requested 20-second interval, exited zero and drained its target. Corrected analysis verified distinct owner and waiter OS birth identities (PIDs/TIDs 245624/245627; starttime ticks 30980687/30980689), their namespace/resource joins, and the actual wait/release pair with callchains (600132934 ns wall, 185640 ns thread CPU). Perf sample birth identity itself remains unproven. Resource coverage and perf ACK admission remain strict; fixture validation covers nineteen semantic cases including missing/conflicting event-PID rejection. A separate actual no-game source-SDK triangle capture retained causal acquire/present, resource, queue and fence events for frames 0:1 and a unique compositor image receipt; its eight-second command timeout exited 124 intentionally. Admission was exercised with a real lease and worker smoke. These are native diagnostic and artifact checks, not game/load/FPS proof; the older missing-birth campaign remains unproven and closed, with no new game, marker or A/B run.

## Shader registers

`APS5_TRACE_SHADER_REGS=all` prints register snapshots for every shader. Set it to a shader code address in hexadecimal to print one shader's registers.
