# RadeonTop architecture, evidence model, and validation frontier

Radeontop-gororoba forms one measurement instrument. The architecture map
names the control paths, state owners, evidence boundaries, equations, hardware
stop-lines, and validation gates that determine whether a reported number is
supportable.
`docs/rs4xx-engine-busy-read-path.md` owns the RS4xx register interpretation.
`docs/open-work.md` owns the executable roadmap.

## Component map

| Component | State or mechanism | Direct consumers | Validation surface |
|---|---|---|---|
| `device_model.c` | Family-to-register layout, device identity, clock-unit conversion | detection, capture, radeon backend | `tests/device_model_test.c` |
| `detect.c` | DRM discovery, PCI discovery, MMIO admission, mapping, reader binding | process entry, backend adapter | `tests/detect_path_test.c` legacy and modern enumeration variants, compiler matrix, family check, target run |
| `privileges.c` | Effective-UID entry and exit, permanent UID drop | process entry, direct MMIO setup | `tests/privileges_test.c` plus setuid target run |
| `radeon.c` | Radeon ioctl readers and kHz normalization | backend adapter | compiler matrix, unavailable-family runtime gate |
| `amdgpu.c` | libdrm_amdgpu readers and limits | backend adapter | compiler matrix, unavailable-family runtime gate |
| `collector_backend.c` | Reader-pointer capability projection and error classification | collector | compiler analysis, synthetic collector tests |
| `collector.c` | Absolute schedule, reads, accumulation, publication, shutdown | dump and UI | `tests/collector_test.c` |
| `capture.c` | Build/run provenance, JSON serialization, output locking and sync, exact estimator bounds | dump | C serializer tests plus Python JSON parse |
| `rs480_observation.c` | Strict three-key debugfs intake | RS480 direct path | `tests/rs480_observation_test.c` |
| `dump.c` | Generation wait, legacy rendering, evidence object, durable output | file or stdout | unit format tests plus target capture |
| `ui.c` | Whole-snapshot terminal presentation | operator | compiler analysis and interactive run |
| `tools/check-dist.sh` | Deterministic source export, external baseline admission, downstream identity regeneration | release source consumers | unanchored, clean, dirty, synchronized-mutation, and restoration fixtures |
| `tools/radeontop-source-intelligence.sh` | Bounded source maps and calibrated indexes | maintainers and audits | empty-output invocation plus hash manifest |

The Makefile enumerates production translation units. A new root-level C file
does not enter the binary until the `src` list names it. The unit binaries link
only the production modules their contract exercises. A test that textually
includes a production C file names that file as a prerequisite without adding
it to the linker command. `tools/check-test-dependencies.sh` derives every such
include and verifies the corresponding Make rule, so an implementation edit
cannot leave a stale harness binary.

## Startup and shutdown flow

The process follows one ownership chain:

```text
main
  -> drop effective privileges
  -> parse options
  -> init_pci
       -> explicit DRM path, automatic DRM search, or PCI fallback
       -> bind radeon, amdgpu, or validated direct-MMIO readers
  -> drop real, effective, and saved privileges permanently
  -> initbits
  -> collector_init
  -> collector_start
       -> collector_worker
            -> wait for one absolute sample deadline
            -> read each enabled signal once
            -> publish one whole snapshot at a window boundary
  -> dumpdata or present
  -> request stop
  -> wake and join worker
  -> destroy collector
  -> unmap MMIO, deinitialize amdgpu, close DRM descriptor
```

The join precedes every backend teardown. A worker therefore cannot load from a
BAR mapping after `munmap` or query a deinitialized device handle.
`collector_destroy` rejects a live thread. A join failure returns directly from
`main`, so process teardown reclaims the address space without unmapping a BAR
or destroying synchronization state under a worker whose termination remains
unproven.

The source-intelligence bundle supplies the lexical forward and reverse trees.
The callback binding report supplements cflow because a lexical graph does not
resolve reader function pointers, collector backend callbacks, or clock
callbacks by itself.

## Device identity and direct-MMIO admission

`struct radeon_device_identity` carries the complete PCI BDF, vendor and device
IDs, family enum, DRM driver and version, selected status source and register,
and direct resource index and size. One structure follows the selected device
from discovery into the capture header.

The direct-MMIO classifier names every admitted family explicitly. The
contiguous intervals below summarize the allow-list; the implementation
uses switch cases, so a new enum remains rejected until its BAR layout enters
the list deliberately.

| Family interval | PCI resource | Status register | Source identity |
|---|---:|---|---|
| `RS480` | 2 | `RBBM_STATUS` at `0x00000e40` | `pci-resource-rbbm-status` |
| `R600` through `HAINAN` | 2 | `GRBM_STATUS` at `0x00008010` | `pci-resource-grbm-status` |
| `BONAIRE` through `BEIGE_GOBY` | 5 | `GRBM_STATUS` at `0x00008010` | `pci-resource-grbm-status` |

`UNKNOWN_CHIP`, negative enum values, the terminal family sentinel, and values
above the sentinel have no layout. Automatic PCI and DRM enumeration skip an
unknown AMD display device and continue to the first supported candidate. An
explicit path retains its exact BDF, and its unknown ID fails before an
effective-UID elevation, PCI resource open, or `mmap` occurs. An unknown device
inherits neither an R600 GRBM layout nor generic R600 masks. Process startup
also rejects an unknown family before `initbits`, so a DRM register reader
cannot expose a generic gauge map for an unclassified PCI ID.

An explicit DRM path establishes an exact BDF through `drmGetDevice2`, or
through the canonical `drmGetBusid` PCI string on the older-libdrm lane. The PCI
fallback binds to that same BDF. A missing DRM status reader selects direct
MMIO, and `-m` selects it even when the DRM reader exists; neither condition can
silently switch to another GPU. When the selected node uses a driver whose
sampling backend is absent from the build, `-m` retains the validated BDF,
closes the unused DRM descriptor, and admits only the same-BDF direct layout.

The BAR size check precedes privilege elevation and mapping. The offset-zero
window reaches through `SRBM_STATUS`, and the R600+ path separately proves that
the BAR reaches `GRBM_MMAP_BASE + MMAP_SIZE`. Compile-time assertions prove that
each polled register plus its 32-bit access width lies inside its mapping.

For a setuid-root binary invoked by an ordinary user, the privilege interval
contains only the exact sysfs resource open, the required mapping, and the
optional RS480 debugfs read. DRM enumeration, authentication, XCB
authentication, dynamic loading, option parsing, sampling, UI work, and output
run as the invoking user. `setresuid` removes the saved root identity before the
collector starts. A process invoked by root retains root as its invoking
identity; the Makefile installs mode `0755` and does not choose setuid policy.

## Backend and unit contracts

The global device readers provide a compatibility boundary with the upstream
code. `collector_backend_from_device` turns each non-null reader into one
capability and wraps its return convention in `COLLECTOR_READ_OK`,
`COLLECTOR_READ_TRANSIENT`, or `COLLECTOR_READ_FATAL`.

The radeon kernel ABI reports `RADEON_INFO_CURRENT_GPU_SCLK` and
`RADEON_INFO_CURRENT_GPU_MCLK` in megahertz in
`drivers/gpu/drm/radeon/radeon_kms.c`. RadeonTop stores clock samples and maxima
in kilohertz. `radeon_clock_mhz_to_khz` performs the checked conversion before a
current-clock value enters the collector. `RADEON_INFO_MAX_SCLK` multiplies the
kernel's internal 10-kilohertz value by 10 and therefore already uses kilohertz.
RS480 has a fixed memory clock, so its current value seeds `mclk_max`
in the same unit that later samples use. R600+ dynamic-clock parts retain the
absolute sample and leave the unavailable maximum at zero, which suppresses an
unsupported percentage instead of naming an idle sample as the maximum.

Linux `amdgpu_kms.c` returns the `GFX_SCLK` and `GFX_MCLK` sensors in
megahertz and `AMDGPU_INFO_DEV_INFO` clock limits in kilohertz. The amdgpu
readers apply the same checked megahertz-to-kilohertz conversion as radeon,
reject limits wider than the collector representation, zero
`struct amdgpu_gpu_info`, and check `amdgpu_query_gpu_info` before a maximum or
`ids_flags` field enters a capability decision. Runtime behavior on amdgpu
and R600 hardware remains `not run` because neither reachable host exposes those
parts.

## Collector state and estimator

One published window carries these exact counts:

```text
N = nominal slots
A = attempted slots
M = missed slots
V = valid reads for the lane's source register
F = failed reads for the lane's source register
B = busy predicates among the V valid reads
```

The schedule maintains `A + M = N`. A status-capable, nonfatal slot attempts the
status read first, so its status result increments either `V` or `F`. UVD, VCE,
clock, and endpoint signals keep independent validity because a fatal earlier
read can suppress a later query.

The displayed point estimate is conditional on a successful read:

```text
p_conditional = B / V
```

It is undefined when `V = 0`. A zero there would confuse an unreadable source
with a confirmed-idle source.

Read failures and missed slots can correlate with GPU state. The capture
therefore appends the exact `B`, `V`, and `N` and the assumption-free
missing-data interval:

```text
p_lower = B / N
p_upper = (B + N - V) / N
```

The lower endpoint assigns every unobserved slot idle. The upper endpoint
assigns every unobserved slot busy. The interval narrows to the point estimate
only when every nominal slot produces a valid read.

### Window dispersion and effective sample size

Both intervals above describe missing data. Neither describes how much
information the observed samples carry, because `sqrt(p(1-p)/V)` assumes the
samples are independent and a fixed grid against a periodic load makes them
anything but. The counts that settle it are already retained: each window
publishes its own `B` and `V`, so the scatter of the per-window duty figures
against what each window's denominator implies is Pearson's dispersion

```text
D = sum( (B_i - V_i*p)^2 / (V_i*p*(1-p)) ) / (W - 1)
```

over `W` windows, and `N_eff = N / max(1, D_high)` follows from its upper
confidence limit. Clamping at one keeps an underdispersed reading from narrowing
an interval on the assumption that the structure producing it holds for the whole
run. Excess scatter has several causes and bounds the sampler's contribution
rather than isolating it. A reading below one rules out a changing load and
correlated read loss, because both add variance and neither removes it, and
leaves two structures that a single capture does not separate: a grid whose phase
set is commensurate with a periodic load, and a load whose busy fraction varies
from slot to slot in a repeating pattern. A dithered pass over the same load
separates them.

`tools/capture-window-dispersion.py` computes this from a retained capture and
adds no sample-path code, so it changes neither the collector nor the packaged
binary. `docs/window-dispersion-and-effective-sample-size.md` derives the
estimator, decomposes the arc coverage that sets its value, and carries the
RS482 readings: 0.228 on a vertex-build load sampled on the exact grid, 0.969 on
a solid-fill load with no commensurate period, and 1.79 to 1.90 on the VAP-phase
loads.

### Dither selection effect

Let a slot have width `T`, let a seeded dither choose offset `D` uniformly in
`[0,T)`, and let scheduler wake lateness be `L`. The collector accepts the slot
only while its next grid boundary remains ahead:

```text
D + L < T
```

For fixed `L`, uniform `D` gives

```text
P(accepted | L) = max(0, 1 - L/T)
```

The accepted offsets occupy `[0,T-L)`, not the full slot. Dithering breaks a
fixed workload phase, but scheduler delay preferentially removes late-slot
phases. Coverage measures the amount of missingness; it does not prove that
accepted phases remain representative. The missing-data interval protects the
reported duty against arbitrary missing state. A future phase-inclusion model
requires attempted and accepted phase histograms before a narrower dithered
estimate earns support.

The rational grid contains slots of `floor(10^9/ticks)` or
`ceil(10^9/ticks)` nanoseconds. The dither bound follows each slot's exact
carried width. Splitmix64 output passes through rejection sampling, which
removes the modulo bias present when the slot width does not divide `2^64`.

## Capture format and provenance

Every accepted dump run begins with one line whose prefix is
`# radeontop_capture_v1 `. The remainder is JSON and carries:

- a fresh run UUID from `/proc/sys/kernel/random/uuid`;
- the boot UUID from `/proc/sys/kernel/random/boot_id`;
- readable version, one clean 40- or 64-hex source commit, production-source manifest
  and SHA-256, build manifest and SHA-256, and kernel release;
- realtime and monotonic start stamps;
- source and build manifest byte strings with `byte-u00xx` encoding;
- the JSON-escaped argument byte vector with `argv_encoding: byte-u00xx`;
- PCI BDF, vendor ID, device ID, family, DRM driver, and DRM version;
- status source, register name and offset, resource index, and resource size;
- ticks, window duration, dither seed, memory sizes, and clock maxima.

Argument validation precedes dump setup, so a rejected invocation emits no
capture stream. The boot UUID links captures produced during one host boot; a
published header therefore discloses a host-linkable identifier that remains
stable for that boot's lifetime.

The source manifest is the sorted `sha256sum` preimage for every production
input. The build manifest records its schema, source identity, admitted export
baseline digest, and the exact version, compiler, compiler version,
preprocessor flags, compiler flags, linker flags, libraries, and option string
as reversible hexadecimal byte fields. A Git checkout records `none` for the
export baseline. An authenticated exported build records the caller-supplied
64-hex digest. The capture embeds both manifest byte strings and their SHA-256
values, and installation retains the same bytes under `/usr/share/radeontop/`.
A reader therefore recomputes each digest without access to the build host. A
clean source commit supplies the content behind every source-manifest row.

`make dist` exports committed `HEAD` through `git archive` into a temporary
tree, then adds `include/radeontop-source-export.mk` with the exact Git
description, object ID, unknown state, and baseline path. The exported Makefile
loads that fragment and keeps `getver.sh` active, so each downstream build
recomputes the source manifest over the archive bytes and recomputes the build
manifest over its own compiler, flags, libraries, and options. The complete
baseline travels inside the archive, while its expected SHA-256 travels as a
separate sidecar. The in-tree baseline never authenticates itself. A caller
supplies `SOURCE_BASELINE_SHA256` from independently retained release metadata,
or computes it only after verifying an independently retained archive digest.
The baseline covers the fixed-path export metadata. `getver.sh` snapshots those
bytes, verifies their baseline and source-manifest rows, parses their exact
four-line schema, and binds the build version and source commit to their values.
No supplied digest leaves the state `unknown`; a matching digest admits row
comparison as `clean` or `dirty`; a digest mismatch stops the build before row
parsing. The distribution target never edits the checkout. The caller supplies
the output directory explicitly. The target normalizes path order, timestamp,
ownership, and gzip metadata. The `.tgz.sha256` manifest hashes the archive and
the source-baseline sidecar and publishes after both, so it is the complete-pair
commit marker when its bytes arrive through an independent channel.
`tools/check-dist.sh` proves two exports are byte-identical, a dirty tracked
Makefile survives unchanged and stays outside the archive, downstream flag
changes alter only build identity, source-only mutation reports dirty, and
synchronized source-plus-baseline mutation fails the original external digest.

Each legacy data line ends with an `evidence_v2` JSON object. It repeats the run
UUID and preserves exact slot accounting, capability bits, five signal
denominators, clock means, supported/failed endpoint state, nanosecond
timestamps, timing maxima, and per-exposed-lane `B`, `V`, `N`,
conditional fraction, and unconditional bounds. Existing human-readable fields
remain at the front of the line.

The collector holds one replaceable published snapshot and one separate
write-once terminal record. Publication commits a generation only after its
monotonic and realtime stamps succeed, and no later failure mutates that
generation. An unseen final snapshot reaches a consumer before the terminal
record whose `after_generation` names it. Dump mode uses the contiguous-wait
contract and fails when a delayed consumer observes a generation jump. A silent
missing record never enters a successful research capture.

The interactive UI copies the latest snapshot and terminal record under one
collector mutex acquisition. A terminal whose `after_generation` names N
therefore appears beside generation N, while an intermediate observation after
publication and before terminal recording presents generation N as active.

`# radeontop_run_end_v2` records the run UUID, logical exit reason, logical
status, record count, last consumed generation, last observed committed
collector generation, and a typed terminal cause. Only a device-read terminal
carries a backend read result; clock and schedule causes carry JSON `null`. The
logical fields describe the collection loop; final process status also includes
capture sync, unlock, and close plus collector join and destruction results and
remains an external bundle field. Backend and clock teardown calls expose no
checked result yet, so the synchronization and failure-injection roadmap owns
that wider contract.

An output path equals stdout only when it is exactly `-`. A path such as
`-capture.log` names a regular file. Regular descriptors carry an exclusive
advisory lock for the whole run and receive `fsync` at the end, including when
stdout redirects to a regular file. A nonempty regular descriptor receives one
newline after the lock and before the new header. The boundary terminates a
truncated prior record, so it cannot absorb the next run identity. Runtime
diagnostics use stderr. JSON strings escape quotes, backslashes, control bytes,
and non-ASCII bytes, so an argument cannot inject a second header record and a
consumer can recover each original argument byte.

Dump mode selects the C numeric locale before collector thread creation, so
serialization keeps its decimal contract without a process-global locale
mutation during concurrent collection.

The optional RS480 GART/MC line appears only when all three exact debugfs keys
parse once with complete unsigned 32-bit values. Prefix aliases, duplicates,
signed values, overflow, trailing junk, truncated lines, and incomplete input
invalidate the observation.

## Reproducible source intelligence

The command below writes one bounded analysis directory without cleaning or
overwriting an existing directory:

```sh
source_intelligence_dir=$(mktemp -d)
make source-intelligence SOURCE_INTELLIGENCE_DIR="$source_intelligence_dir"
```

The bundle contains:

- a sorted tracked C/H denominator with byte counts, line counts, SHA-256
  hashes, disjoint C/header partitions, and exact-union proof;
- known-bad denominator mutations for an ignored generated header, a duplicate
  path, and a missing tracked source;
- repository commit, branch, and worktree status;
- cflow runtime forward, reverse, DOT, and SVG call graphs;
- per-test forward and reverse cflow trees and graphs that include every
  production translation unit compiled separately or textually included by the
  corresponding harness;
- compiler-preprocessed project-origin translation units for the legacy bus-ID
  and modern `TEST_DRM_BUS_DISCOVERY` executables, with source and graph
  validators that reject a two-main source union and a raw `detect.c` graph
  union;
- a relocatable cscope database plus Universal Ctags and GNU Global databases;
- known-good `main` and known-bad missing-symbol calibration results;
- compiler include dependencies as Make syntax, DOT, and SVG;
- explicit callback-binding search results with known-good and missing-binding
  calibration artifacts;
- lizard and scc complexity inventories;
- calibrated text-and-binary rejection of the generating checkout and output
  directory paths;
- calibrated rejection of commit, branch, worktree-status, or source-byte
  drift that remains at the final live-checkout rescan;
- one read-only, hash-verified source snapshot shared by every analyzer, plus a
  separately hashed generated-header overlay for compiler dependency analysis;
- exact tool versions and a final internally verified `SHA256SUMS` manifest.

Universal Ctags 6.2.1 emits one options notice and two Cargo/TOML parser warnings
at startup even under `--options=NONE --languages=C`. The script accepts only
lines from that explained diagnostic set or empty stderr, rejects duplicates and
every other diagnostic, and retains the original bytes in `ctags.stderr`.
Positive and negative symbol calibrations separately determine whether the C
index is usable.

Cscope writes its build directory into the database header. The script uses the
complete non-quick database for this bounded corpus and builds it from the
tracked half of the analysis snapshot, which contains exactly the tracked
denominator. It replaces the generated header with a fixed `.` root, adjusts
the single trailer offset, and proves the result through
`cscope -d -P SOURCE_ROOT` from the output directory. A file-list query must
equal `source-files.txt`, so recursive include discovery cannot admit the
ignored generated version header. The fixed header removes the build path
without introducing quick-index offsets that refer to its former byte positions.

GNU Global uses its SQLite backend so repeated generation preserves identical
database bytes, and its complete path query must equal `source-files.txt`.
Ripgrep emits callback matches with one worker, and scc emits name-sorted
per-file records with one worker and one queue slot. These controls remove
scheduler-dependent record order from the retained bundle.

The generator captures the commit, branch, worktree status, complete source
denominator, and generated version header before analysis. It copies those
bytes into separate tracked and generated-input areas, verifies their hashes,
removes their write permissions, and runs every analyzer from the tracked
snapshot. The compiler dependency pass also reads the generated-input overlay.
A transient live-checkout edit after snapshot verification cannot mix analyzer
inputs. The final live-checkout rescan rejects commit, branch, status, tracked
source, or generated-input drift that remains at that boundary. Calibrated
commit, branch, status, source-denominator, and generated-input mutations
exercise the comparison edges.

The tracked denominator excludes `include/version.h`, whose ignored build
identity can survive while `git status --short` remains empty. Every source
index, lexical graph, callback inventory, and complexity consumer derives from
the validated denominator or its exact C or runtime subset. The Makefile
regenerates `include/version.h` before the compiler include graph records it as
a build dependency. The retained indexes identify sources relative to the
repository root. A calibrated final scan rejects generating checkout and output
paths before `SHA256SUMS` binds the bundle.

These products prove lexical structure, indexed reachability, and the exact
`TEST_DRM_BUS_DISCOVERY` branch selection represented by the two configured
test translation units. The compiler expands macros and the project-origin
filter omits system-header declarations while retaining GCC line markers for
`tests/detect_path_test.c` and its textual `detect.c` inclusion. These products
do not prove every runtime build-option combination, runtime callback selection,
a successful build, or a silicon behavior. The compiler matrix, unit suites,
and target captures remain separate evidence classes.

## RS482 evidence authority and stop-line

Linux `r300d.h` and `rs400d.h` establish the `RBBM_STATUS` field names and bit
positions. The retained copies are:

```text
steinmarder-r300:docs/external_sources/rs480_r300_registers_and_driver_sources/raw/source/linux/r300d.h
steinmarder-r300:docs/external_sources/rs480_r300_registers_and_driver_sources/raw/source/linux/rs400d.h
```

Their relevant decode slices are identical. Their audit-snapshot SHA-256 values
are `1185713e90bfd1af74c408487ea6d47bdf5e88a9ef0229ac714cfb9d49f5d0e0`
and `e11f91ed4911911a0651a8f9ba0b5cbc0b470472d4aeafe107552c51d355861f`.
These hashes identify source inputs, not silicon-run artifacts.

Commit `870b22da8cc3070186927e5fea22196f88dd7c76` on merged
`steinmarder-r300` `main` retains the decision-grade candidate capture and its
active finding:

```text
steinmarder-r300:src/re/r300/results/cachyos_vostro1000_rs482_rbbm_status_mmio_capture_20260809T185637Z
steinmarder-r300:src/re/r300/findings/active/rs482-rbbm-status-mmio-capture-observability.md
```

The SHA-256 of the outer `bundle_hashes.sha256` manifest is
`5ab48d99b4cebfe0d7183e5bf3478cd6147154d56e717cc8fba2e8ac02a193a7`.
The calibrated analyzer accepts the intact finite denominator and rejects six
known-bad mutation classes covering stale hashes, semantic slot accounting,
executed binary identity, post-map privilege, mapped DSO identity, and original
manifest preimages.

The retained run binds candidate
`f9d9e4719e3a25b7d8d3b3c7ff7bac99c2a2b189` to RS482 PCI `1002:5974`,
subsystem `1028:022a`, on boot
`da747e8a-2cff-47ac-bb5b-50d7e7ce7313`. The trace opens only resource2,
maps 3668 bytes read-only through `RBBM_STATUS` at `0x0e40`, and records the
permanent real, effective, and saved UID transition to 1000 before collection.
The kernel delta and hazard match set are empty, and the display and renderer
remain live. No GA, ZB, or broad `0x4xxx` register read enters the run.

At 120 Hz, each default, explicit `-p`, and explicit `-p -m` texture capture
attempts all 600 nominal reads. Default discovery and explicit `-p -m` report
100.000 percent `GUI_ACTIVE`; explicit `-p` reports 99.667 percent. `VGT_BUSY`
spans 12.500 to 13.167 percent while the load remains live at 104 to 105 frames
per second. At 1000 Hz, the dithered run attempts 4107 of 5000 slots, so its
conditional `GUI_ACTIVE` coefficient of 99.464 percent carries the
unconditional interval of 81.700 to 99.560 percent. The candidate records
400000 kHz SCLK and 200000 kHz MCLK; the control rounds the same kernel MHz
inputs to zero.

The finding remains active and `canonical: false`. Its retained bundle admits
the exact candidate, target, boot, and workload result; it makes no universal
claim for R600, amdgpu, unobserved backend bits, or the RS482 2D lanes. The
broad glmark2 control is mixed because three other scenes fail and several
remain unknown without a pre-run baseline. The signed publication offset also
remains a source-semantics follow-up: the 120 Hz records lie 7.245 to 8.229 ms
before the nominal half-open window end, while the 1000 Hz offsets span 0.634 ms
before to 0.007 ms after it.

The June 10 backend-bit finding is active and `canonical: false`:

```text
steinmarder-r300:src/re/r300/findings/active/2026-06-10-rs482-rbbm-backend-busy-bits-nonlatching-under-load.md
```

Its audit-snapshot SHA-256 is
`78956562f2264bc059886b83a94fe4040b57138f387cae69277d79b86563471e`.
The recorded raw-word summary does not carry a decision-grade bundle hash,
complete manifest, exact command set, or boot identity. It supports bounded
non-observation under the named loads and sample rates, not a universal claim
that a field never asserts.

The June 15 GA-gap finding records one detailed active-state deep wedge at
offset `0x42d0` and remains active and `canonical: false`:

```text
steinmarder-r300:src/re/r300/findings/active/2026-06-15-rs482-ga-block-gap-engine-busy-read-wedge.md
```

The finding's audit-snapshot SHA-256 is
`d56d4e7f3e24480fe062e82b54a14e6f4c9ca588a8f33c3d030874bacf740a8d`.
It names one result path but omits the bundle hash and other required run
identity. The observed GA wedge and the unreached ZB uncertainty keep active
GA/ZB and broad `0x4xxx` reads outside RadeonTop. Radeontop-gororoba polls only
the established status offsets.

## Validation frontier

The source and unit gates establish implementation structure. They do not
substitute for a target result.

| Claim | Required gate | Current class |
|---|---|---|
| Unknown PCI IDs cannot reach direct MMIO | pure layout rejection plus source path audit | unit-tested source |
| RS482 collection runs without saved privilege | credential trace of the setuid installation | target-verified for the direct path |
| RS482 reads `RBBM_STATUS` through resource2 | same-boot idle and named-load capture | target-verified for candidate `f9d9e471` |
| Radeon clocks use one unit | kernel ABI source, conversion tests, and same-boot control | source-backed, unit-tested, and target-verified on RS482 |
| amdgpu and R600 behavior remains equivalent | runs on exact amdgpu and R600 parts | hardware unavailable |
| Capture records parse as intended | C formatter controls plus Python JSON parse and byte round trip | unit-tested and target-replayed |
| Missing-data bounds contain every missed-slot assignment | arithmetic mutations plus a lossy target run | unit-tested and retained as a bounded target interval |
| Dither removes phase lock without selection bias | attempted/accepted phase histograms under injected lateness | unresolved |
| The reported duty carries its own effective sample size | window dispersion over a run long enough to resolve it | source-backed, unit-tested, and target-computed on retained RS482 captures |
| The sub-binomial regime appears on this part | window dispersion over a run long enough to resolve it | target-computed at D = 0.228 on the vertex-build load |
| The exact grid rather than the load produces that regime | a dithered pass over the same load | unresolved |
| Backend and 2D bits expose on RS482 | higher-rate, load-identified register capture | unresolved |
| Publication timing names its signed endpoint relation | virtual-clock oracle plus signed target offsets | target deviation retained; source contract unresolved |

The next implementation work concentrates on the unresolved rows rather than
adding new gauges. `docs/open-work.md` orders the mechanism-specific actions and
keeps package, source-generation, metadata, and hardware evidence debt visible.
