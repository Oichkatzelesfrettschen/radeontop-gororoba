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
| `detect.c` | DRM discovery, PCI discovery, MMIO admission, mapping, reader binding | process entry, backend adapter | `tests/detect_path_test.c`, compiler matrix, family check, target run |
| `privileges.c` | Effective-UID entry and exit, permanent UID drop | process entry, direct MMIO setup | `tests/privileges_test.c` plus setuid target run |
| `radeon.c` | Radeon ioctl readers and kHz normalization | backend adapter | compiler matrix, unavailable-family runtime gate |
| `amdgpu.c` | libdrm_amdgpu readers and limits | backend adapter | compiler matrix, unavailable-family runtime gate |
| `collector_backend.c` | Reader-pointer capability projection and error classification | collector | compiler analysis, synthetic collector tests |
| `collector.c` | Absolute schedule, reads, accumulation, publication, shutdown | dump and UI | `tests/collector_test.c` |
| `capture.c` | Build/run provenance, JSON serialization, output locking and sync, exact estimator bounds | dump | C serializer tests plus Python JSON parse |
| `rs480_observation.c` | Strict three-key debugfs intake | RS480 direct path | `tests/rs480_observation_test.c` |
| `dump.c` | Generation wait, legacy rendering, evidence object, durable output | file or stdout | unit format tests plus target capture |
| `ui.c` | Whole-snapshot terminal presentation | operator | compiler analysis and interactive run |
| `tools/radeontop-source-intelligence.sh` | Bounded source maps and calibrated indexes | maintainers and audits | empty-output invocation plus hash manifest |

The Makefile enumerates production translation units. A new root-level C file
does not enter the binary until the `src` list names it. The unit binaries link
only the production modules their contract exercises.

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
above the sentinel have no layout. The classifier rejects them before an
effective-UID transition, `open`, or `mmap` occurs. An unknown AMD display
device inherits neither an R600 GRBM layout nor generic R600 masks. Process
startup also rejects an unknown family before `initbits`, so a DRM register
reader cannot expose a generic gauge map for an unclassified PCI ID.

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
current-clock value enters the collector. `RADEON_INFO_MAX_SCLK` already uses
kilohertz. RS480 has a fixed memory clock, so its current value seeds `mclk_max`
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
still needs attempted and accepted phase histograms before a narrower dithered
estimate earns support.

The rational grid contains slots of `floor(10^9/ticks)` or
`ceil(10^9/ticks)` nanoseconds. The dither bound follows each slot's exact
carried width. Splitmix64 output passes through rejection sampling, which
removes the modulo bias present when the slot width does not divide `2^64`.

## Capture format and provenance

Every dump run begins with one line whose prefix is
`# radeontop_capture_v1 `. The remainder is JSON and carries:

- a fresh run UUID from `/proc/sys/kernel/random/uuid`;
- the boot UUID from `/proc/sys/kernel/random/boot_id`;
- readable version, one clean 40-hex source commit, production-source manifest
  and SHA-256, build manifest and SHA-256, and kernel release;
- realtime and monotonic start stamps;
- source and build manifest byte strings with `byte-u00xx` encoding;
- the JSON-escaped argument byte vector with `argv_encoding: byte-u00xx`;
- PCI BDF, vendor ID, device ID, family, DRM driver, and DRM version;
- status source, register name and offset, resource index, and resource size;
- ticks, window duration, dither seed, memory sizes, and clock maxima.

The source manifest is the sorted `sha256sum` preimage for every production
input. The build manifest records its schema, source identity, and the exact
version, compiler, compiler version, preprocessor flags, compiler flags, linker
flags, libraries, and option string as reversible hexadecimal byte fields. The
capture embeds both manifest byte strings and their SHA-256 values, and
installation retains the same bytes under `/usr/share/radeontop/`. A reader can
therefore recompute each digest without access to the build host. A clean source
commit supplies the content behind every source-manifest row.

Each legacy data line ends with an `evidence_v1` JSON object. It repeats the run
UUID and preserves exact slot accounting, capability bits, five signal
denominators, clock means, supported/failed endpoint state, terminal read state,
nanosecond timestamps, timing maxima, and per-exposed-lane `B`, `V`, `N`,
conditional fraction, and unconditional bounds. Existing human-readable fields
remain at the front of the line.

The collector holds one replaceable published snapshot. Dump mode therefore
uses the contiguous-wait contract and fails when a delayed consumer observes a
generation jump. A silent missing record never enters a successful research
capture.

`# radeontop_run_end_v1` records the run UUID, logical exit reason, logical
status, record count, last consumed generation, and final collector state. The
logical fields describe the collection loop; final process status also includes
capture sync, unlock, and close plus collector join and destruction results and
remains an external bundle field. Backend and clock teardown calls expose no
checked result yet, so the synchronization and failure-injection roadmap owns
that wider contract.

An output path equals stdout only when it is exactly `-`. A path such as
`-capture.log` names a regular file. Regular descriptors carry an exclusive
advisory lock for the whole run and receive `fsync` at the end, including when
stdout redirects to a regular file. JSON strings escape quotes, backslashes,
control bytes, and non-ASCII bytes, so an argument cannot inject a second
header record and a consumer can recover each original argument byte.

The optional RS480 GART/MC line appears only when all three exact debugfs keys
parse once with complete unsigned 32-bit values. Prefix aliases, duplicates,
signed values, overflow, trailing junk, truncated lines, and incomplete input
invalidate the observation.

## Reproducible source intelligence

The command below writes one bounded analysis directory without cleaning or
overwriting an existing directory:

```sh
make source-intelligence SOURCE_INTELLIGENCE_DIR=/path/to/empty/output
```

The bundle contains:

- a sorted C/H denominator with byte counts, line counts, and SHA-256 hashes;
- repository commit, branch, and worktree status;
- cflow runtime forward, reverse, DOT, and SVG call graphs;
- per-test forward and reverse cflow trees and graphs that include every
  production translation unit linked by the corresponding Makefile rule;
- cscope, Universal Ctags, and GNU Global databases;
- known-good `main` and known-bad missing-symbol calibration results;
- compiler include dependencies as Make syntax, DOT, and SVG;
- explicit callback-binding search results;
- lizard and scc complexity inventories;
- exact tool versions and a final internally verified `SHA256SUMS` manifest.

Universal Ctags 6.2.1 emits a Cargo/TOML parser warning at startup even under
`--options=NONE --languages=C`. The script retains that diagnostic in
`ctags.stderr`; the positive and negative symbol calibrations determine whether
the C index is usable.

These products prove lexical structure and indexed reachability. They do not
prove a preprocessor variant, runtime callback selection, a successful build,
or a silicon behavior. The compiler matrix, unit suites, and target captures
remain separate evidence classes.

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
| DRM/XCB work runs unprivileged | credential trace of the setuid installation | target run pending |
| RS482 reads `RBBM_STATUS` through resource2 | same-boot idle and named-load capture | target run pending for this tree |
| Radeon clocks use one unit | kernel ABI source plus conversion tests | source-backed and unit-tested |
| amdgpu and R600 behavior remains equivalent | runs on exact amdgpu and R600 parts | hardware unavailable |
| Capture records parse as intended | C formatter controls plus Python JSON parse and byte round trip | unit-tested; target replay pending |
| Missing-data bounds contain the true state under synthetic loss | correlated-failure mutations | unit-tested arithmetic; mutation gate pending |
| Dither removes phase lock without selection bias | attempted/accepted phase histograms under injected lateness | unresolved |
| Backend and 2D bits expose on RS482 | higher-rate, load-identified register capture | unresolved |

The next implementation work concentrates on the unresolved rows rather than
adding new gauges. `docs/open-work.md` orders the mechanism-specific actions and
keeps package, source-generation, metadata, and hardware evidence debt visible.
