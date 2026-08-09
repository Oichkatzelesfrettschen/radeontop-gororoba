# Open work

The source tree and the packaged source pin are separate publication frontiers.
A source change lands first; a follow-up package change pins the reachable
merged commit, derives `pkgver`, and resets `pkgrel` to 1. The workflows define
compiler, analyzer, unit, CLI, package-identity, and reproducibility gates. Live
branch-protection configuration remains external state and needs an API-backed
verifier before documentation treats it as a repository invariant.

Items below are grouped by the gate each one waits on. Ordering inside a group
carries no dependency; every item is independently landable.

## Landed mechanisms

The source tree carries these mechanisms, so a remaining item that appears to
restate one asks for a narrower contract or evidence class.

- The collector owns the sampling cadence on an absolute `CLOCK_MONOTONIC`
  deadline grid with a Bresenham remainder carry, publishes generation-numbered
  whole-value snapshots under a mutex, and joins before backend teardown.
- Each sample is attributed to the window containing its own timestamp. The
  schedule advances both before a device read, so an expired window closes
  empty rather than absorbing a later sample, and after it, so a read that
  overruns several periods skips their slots instead of firing a catch-up burst.
- Each measurement class counts its own valid and failed reads. A lane's
  denominator is the valid read count of its source register, never the nominal
  slot count, and `collector_lane_fraction` returns NaN when that count is zero
  so a caller renders N/A instead of 0.00%.
- A backend read classifies itself OK, TRANSIENT, or FATAL in the adapter, and a
  fatal result stops the slot rather than letting later reads in the same slot
  proceed against a device that is gone.
- `scheduled_end_realtime` derives the window's wall-clock end by subtracting
  the publication lag from a near-simultaneous monotonic/realtime pair, so the
  endpoint reads do not date the window.
- The dump serializes per-signal health and gates each clock on its own signal.
  A consumer that aggregates across runs still weights each window by its own
  denominator, because the denominators differ window to window.
- Every MMIO register load goes through `mmio_read32`, which reads through a
  `const volatile uint32_t *`, so the source requires a fresh device access
  rather than relying on the indirect-call structure to force one.
  `_Static_assert` proves each offset plus its access width lands inside the
  mapping it reads.
- `--dither-seed N` offsets each sample inside its own slot, so a workload
  periodic at a harmonic of the sampling rate meets a moving phase. Each bound
  uses the exact carried slot width, and rejection sampling removes modulo bias.
  Slot membership follows the grid, so slot count, window boundaries, and the
  attempted-plus-missed identity remain invariant. Omitting the flag keeps the
  exact grid, which is what makes two runs of one workload directly comparable.
- Six unit binaries cover capture serialization, collector scheduling and
  failure semantics, device admission and clock conversion, and strict RS480
  observation parsing. A Python standard-library gate parses complete header,
  evidence, and run-end JSON and round-trips arbitrary argument bytes. The
  workflow runs the suites under gcc, clang,
  ASan/UBSan/LSan, and ThreadSanitizer. Scheduler properties carry negative
  controls: mutating either schedule advance, giving up a slot whose sample
  point is still ahead, letting the dither exceed its slot, and discarding the
  drawn offset each make the suite fail.
- `.clangd` at the repository root and under `tests/` supplies an editor
  approximation of the language and include flags. The command-database row
  below remains the gate that makes editor and build preprocessing identical.
- Direct MMIO uses a pure family classifier. Unknown and out-of-range families
  have no BAR layout and stop before privilege elevation, PCI resource open, or
  mapping. The process elevates only for the validated resource mapping and
  optional RS480 debugfs intake, then removes the saved identity before
  collection.
- Radeon and amdgpu current clocks convert the kernel ABI's megahertz to the
  collector's kilohertz unit with overflow checks. Only fixed-clock RS480
  derives a memory-clock maximum from a current sample. The amdgpu path checks
  and initializes GPU information before reading clock limits or APU flags.
- Dump runs accept one clean 40- or 64-hex commit and bind version, source state,
  source manifest and digest, build manifest and digest, boot, device, and
  command bytes in a versioned JSON header. Each evidence
  object repeats the run UUID and distinguishes support, failure, clock means,
  endpoints, terminal state, and missing-data bounds. A run-end record names
  the logical outcome. Regular outputs carry a whole-run lock and final sync;
  a skipped collector generation fails the capture.
- The build-identity self-test recomputes both digests from retained canonical
  manifests. It proves that a source mutation changes the source digest, a flag
  mutation changes the build digest, a dirty tree cannot assert clean state,
  and an exported source can carry an explicit immutable commit. Capture tests
  reject dirty, unknown, and non-object source identities.
- The distribution target exports committed `HEAD` without editing the
  worktree, carries source identity in a generated Makefile fragment, regenerates
  build identity under downstream flags, normalizes archive metadata, and
  publishes a SHA-256 sidecar. Its calibrated self-test proves deterministic
  output, dirty-worktree preservation, and rejection of archive and source-ID
  mutations.
- The source-intelligence target emits hashed runtime and linked-test cflow,
  cscope, ctags, Global, compiler-include, callback, complexity, and tool-version
  products from one exact C/H denominator.
- The CLI rejects every positional operand without permuting the command vector.
  The CLI documentation verifier derives long names, short names, and argument
  requirements from `getopt_long`, compares the binary, man source, and generated
  man page, and rejects a fixture with one missing option. The man generator
  treats every diagnostic as a failure, carries explicit author and revision
  metadata, and exposes `--dither-seed` and `-T`.
- AppStream metadata names the fork, exact GPL3 license, executable, developer
  collective, and OARS rating. Installation places it under `share/metainfo`,
  and the analysis workflow runs the pedantic offline validator.

## Admitted RS482 target evidence

Commit `870b22da8cc3070186927e5fea22196f88dd7c76` on merged
`steinmarder-r300` `main` retains the exact candidate run at:

```text
steinmarder-r300:src/re/r300/results/cachyos_vostro1000_rs482_rbbm_status_mmio_capture_20260809T185637Z
```

`docs/architecture-and-evidence.md` owns the exact claim-to-evidence mapping,
outer manifest digest, and bounded measurements. The bundle closes these two
target gates for candidate `f9d9e471`:

- The setuid-origin trace opens and maps only resource2 through
  `RBBM_STATUS` at `0x0e40`, then proves real, effective, and saved UID 1000
  with zero permitted and effective capabilities in both live process threads.
- The same-boot control and candidate captures retain run and boot UUIDs, exact
  BDF and subsystem identity, source and build manifests, executed binary
  hashes, command vectors, live-load witnesses, kernel before/after data,
  hazard search, JSON records and footers, locks, synchronization, and nested
  and outer SHA-256 manifests.

The 120 Hz paths cover every slot under the sustained texture load. The 1000 Hz
dithered path services 4107 of 5000 slots, so its missing-data interval remains
the high-rate result. The broad glmark2 control remains mixed, and the signed
publication offset remains a naming and consumer-semantics follow-up. These
bounded residuals do not reopen the admitted direct path or clock-unit result.

## Target reports awaiting evidence admission

The older numerical reports below come from target-local runs. Their raw
bundles remain absent from the canonical evidence repository, and the related
`steinmarder-r300` findings are active with `canonical: false`. The reports
define reproduction targets and falsifiers; they do not constitute admitted
silicon evidence until a retained bundle supplies the exact binary and source
digests, boot identity, commands, load liveness, kernel delta, and artifact
hashes.

Two runs on the RS482 target (PCI `1002:5974`) compared a control worktree at
the pre-change merge with a test worktree at `master`. Reads stayed confined to
`RBBM_STATUS` (`0x0E40`) through the BAR2 `resourceN` window, and the boot
identifier was identical at the start and end of both runs, so the K8
northbridge watchdog (`D18F3x44`, AMD BKDG #32559) was not disturbed.

- Idle reports every lane at 0.00% with 100% coverage and no missed slot at 120
  samples per second, at a read cost of 2 to 50 microseconds.
- Under a sustained `glmark2-es2` texture load the GUI, EE, and PA lanes report
  87% to 100% busy and the CP lane 79% to 100%, with the VGT lane clear. Control
  and test agree, interleaved twice against one load held alive across every
  pass.
- `attempted + missed == nominal` holds in every window at 120, 500, and 1000
  samples per second, including windows at 1000 that give up 69 to 96 slots.
- Wake-up lateness reaches 0.5 to 2.6 milliseconds at every rate while a read
  costs at most 59 microseconds, so the misses come from the wake-up arriving
  after the next grid point rather than from read latency, which is the pre-read
  skip. The fourth run below reaches the post-read skip by raising the rate.
- Publication precedes the scheduled window end by about one period at each rate
  -- 8.25 milliseconds at 120, 1.91 at 500, 0.71 to 0.92 at 1000 -- which is
  where the last slot of a window sits.
- The dump timestamp's sub-second offset varies by 0 and 1 microsecond across
  eight windows on the test binary against 607 and 679 microseconds on the
  control, which is the `scheduled_end_realtime` derivation removing the
  endpoint-read lag from the window's date.
- Both scheduler negative controls fail on the target with the same counts they
  produce on the development host, so the calibration travels with the run.
- SIGINT closes the dump and exits in 64 milliseconds, SIGTERM exits cleanly,
  and a dump directed at `/dev/full` exits nonzero.

A third run compared the accepted tree with the volatile MMIO reads and the
dithered schedule on top of it, against one load held alive across every pass.

- The volatile reads change neither figure. Mean GUI busy is 99.90 and 99.58
  percent on the control against 99.48 and 99.06 on the test, and the worst read
  cost is 86 and 47 microseconds on the control against 9 and 6 on the test.
  Both are within the spread the interleaved control passes show between
  themselves, so the fold the qualifier forbids was not happening at this
  optimization level and the qualifier costs nothing.
- A dithered pass and an exact pass agree on the duty figure at 120 samples per
  second: 99.21 and 98.54 percent seeded against 99.17 unseeded.
- Dithering costs coverage, because a sample placed late in its slot has less
  room before the slot ends. At 120 samples per second the seeded runs give up
  76 and 70 slots of 960 while the unseeded run gives up none. At 1000 the
  unseeded run gives up 735 of 5000 and the seeded run 1586, and the duty figure
  separates to 95.63 percent against 98.67. The ratio holds as the rate climbs:
  at 2000, 4000, and 8000 samples per second the seeded runs give up 2195, 6673,
  and 18825 slots against 568, 2093, and 6786 unseeded.
- `attempted + missed == nominal` holds in every window of every pass, seeded
  and unseeded, at both rates.

Dithering therefore loses more slots as wake-up lateness approaches the period.
The per-window coverage figure reports loss magnitude, and the unconditional
missing-data interval bounds the unobserved state. Neither proves that accepted
sample phases remain representative; the phase-inclusion row below owns that
discriminator.

A fourth run drove the rate up until the slot period fell under the read cost,
which is the falsifier for the post-read deadline skip. `max_read_latency_ns` is
measured inside `sample_once`, bracketing the backend calls alone, so a window
reporting a read cost above its own period had deadlines behind it when the read
returned.

- The BAR read cost has a long tail on this part under load, independent of the
  schedule. Worst costs per pass run 341 to 913 microseconds across 1000, 2000,
  4000, and 8000 samples per second, seeded and unseeded alike, against typical
  costs under 60. An unretained reading that paired a 588 microsecond cost with a
  seeded pass is consistent with that tail rather than the dither, which changes
  when a sample is taken and not how long the load takes. No retained artifact
  tests the cause of that individual reading.
- At 4000 samples per second, a 250 microsecond period, 4 of 5 unseeded windows
  and 2 of 5 seeded windows report a read cost above their own period, so the
  post-read deadline skip fires on silicon rather than under the virtual clock
  alone.
- `attempted + missed == nominal` holds in every window up to 8000 samples per
  second, where a seeded window gives up 47 percent of its slots.

The read costs recorded here come from a later boot than the three runs above,
and the same rates report costs an order of magnitude lower there, so the tail
is a property of a boot's conditions rather than a fixed figure for the part.

The first run's load comparison is void: it drove the load with a `glmark2-es2`
invocation that exits after one benchmark, so the load had ended before the test
binary ran and its windows recorded an idle device. The second run holds the
load with `--run-forever` and proves it alive on both sides of every device
pass. A load pass that does not record the load's liveness cannot distinguish an
idle device from a missing load.

## Ready without target hardware

### Collector dither phase inclusion

The collector records dither seed and accepted sample counts but no attempted
or accepted phase distribution. Add stable phase bins to the accumulator and
capture object. A virtual-clock case injects a known lateness distribution and
a periodic square-wave backend. The gate proves the attempted distribution,
accepted distribution, inclusion probability, and reported duty against their
analytic values. A mutation that discards late-slot attempts fails the gate.

### Collector wake-up lateness distribution

The `late` count saturates when nearly every wake occurs after its exact
deadline. Add fixed nanosecond bins or mergeable quantiles whose schema remains
stable across runs. Unit cases populate every boundary, and the dump serializes
the distribution with `max_lateness_ns`. The target interpretation compares the
distribution with slot width rather than treating a saturated count as a
severity measure.

### Safe MMIO register registry

Static map bounds prove addressability, not silicon read safety. Add a
machine-readable registry keyed by PCI ID, resource index, byte offset, width,
access mode, evidence artifact, and boot identity. A source gate finds every
poller MMIO load and rejects an offset absent from the registry. Calibration
uses `RBBM_STATUS` as the known-good row and an in-range GA `0x42d0` mutation as
the known-bad row. The registry does not promote the active noncanonical wedge
finding to canonical evidence.

### PCI identity exact-set verifier

`familycheck.sh` proves one-way enum-name membership only. Replace or supplement
it with a parser that proves exact enum coverage, unique PCI IDs, R300/R600 table
disjointness, deterministic ordering, and no orphan family string. Fixtures
delete one ID, duplicate one ID, cross the table boundary, and add an orphan
enum; every mutation fails.

### AMDGPU identity generator transaction

`getamdgpuids.sh` can exit success on invalid input, mask producer failure in a
pipeline, and mutate the destination before validation. Generate a complete
temporary block under `set -eu`, retain upstream input identity, validate exact
sets, and replace atomically. Known-good, malformed, removed-family, and failed
preprocessor fixtures calibrate the generator.

### Packaged-source frontier

The package workflow proves that `_commit` is a reachable ancestor, but any
older production commit also satisfies that property. Add a frontier rule: a
production-path commit after `_commit` requires a pin advance or a named
deferral record. A production edit after the pin fails the negative control; a
documentation-only edit leaves package identity unchanged.

### Build command database

`.clangd` approximates the Makefile with fixed include paths. Generate
`compile_commands.json` from every compiler and option lane, and run
`clangd --check` on representative production and test translation units. The
database becomes the authority for include paths and preprocessor features;
`.clangd` retains only semantic editor settings.

### Synchronization primitive failure contract

The collector checks condition-wait and thread-join results, while several
mutex lock, unlock, broadcast, and destruction calls still rely on the POSIX
success contract without an injected failure surface. Route the production
operations through one narrow synchronization adapter, define which failures
make the collector terminal and which make safe recovery impossible, and test
every transition. A join-failure case already proves that backend teardown
stops while worker termination remains unknown; the complete gate extends that
lifecycle invariant to mutex and condition-variable failures.

### Capture durability fault injection

The run-end object describes the collection loop before flush, `fsync`, unlock,
and close complete. The process exit status carries later durability failures,
but the file cannot revise an already written footer after one of those
operations fails. Add an injected stream and descriptor adapter that proves
header, record, logical footer, sync, unlock, and close ordering for each
failure. A committed-footer protocol enters only if an in-file claim needs to
cover post-footer durability; until then, consumers treat process status and
the enclosing evidence-bundle manifest as the commit verdict.

### Direct-MMIO failure-path injection

The explicit-path test proves the same-BDF `-p -m` success path in a build
without amdgpu support. Extend the injected operating-system boundary across
resource-size parsing, privilege elevation, `open`, first mapping, second
mapping, permanent credential removal, and cleanup. Each mutation proves that
no wrong-BDF resource opens, a partial mapping unwinds exactly once, and no
collector starts while a saved privileged identity remains available.

### Reproducible environment replay

`makerepropkg` rebuilds against the `.BUILDINFO` package set from the Arch Linux
Archive. It needs a privileged chroot and exact archive resolution unavailable
in the container job. It remains `not run` for that reason. The existing
workflow proves only same-job equality at a pinned `SOURCE_DATE_EPOCH`.

## Gated on RS482 target hardware

### Read-latency discriminator

Costs of 341 to 913 microseconds appear against typical costs below 60 and vary
by an order of magnitude between boots. Pin the collector to one CPU, repeat at
ordinary scheduling, then use a controlled real-time policy with the same load
and rate. A tail that survives both supports device latency; a tail that shrinks
supports preemption. The run retains full distributions rather than maxima
alone.

### RS482 2D lane discriminator

Record the X server acceleration method from its log or configuration, hold an
EXA copy workload alive, and capture `RBBM_STATUS` at a rate that bounds pulse
loss. A bit 17, 18, or 27 assertion exposes the corresponding lane. Continued
non-observation with confirmed EXA command submission narrows the remaining
silicon and sampling explanations without proving universal absence.

### Backend-bit higher-rate observation

Repeat the textured workload with a calibrated high-rate sampler and an
independent witness that rasterizer, texture, and render-backend work overlaps
the read window. Record per-sample raw words, rate, lateness, read cost, and load
liveness. Any assertion of bits 19, 21 through 25 reopens its display policy.

## Hardware unavailable

The `libdrm_amdgpu` and radeon-ioctl paths remain unexercised on silicon because
neither reachable host exposes an amdgpu or R600 part. Their compiler and
synthetic-adapter results remain distinct from runtime evidence. A future run
uses the same capture schema and verifies that direct-MMIO changes leave those
paths behaviorally identical.

## Evidence retained elsewhere

`docs/rs4xx-engine-busy-read-path.md` owns the register-mapping model. Retained
probes, result bundles, hazard policy, and target-silicon verdicts live in
`steinmarder-r300` under `src/re/r300/`. The admitted candidate bundle above
contains `run_manifest.json`, `bundle_manifest.json`, `bundle_hashes.sha256`,
boot continuity, exact commands, and off-box capture. Future target runs satisfy
the same contract before radeontop-gororoba cites them as decision-grade
evidence.
