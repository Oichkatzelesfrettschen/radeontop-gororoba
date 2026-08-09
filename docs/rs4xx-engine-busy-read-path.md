# RS4xx engine-busy read path, bit map, and duty-cycle estimator

RadeonTop reports R300-class (RS400/RS480/RS482/RS485) GPU utilization by
sampling `RBBM_STATUS` through the BAR2 PCI sysfs `resourceN` node and
accumulating set-bit counts into a duty-cycle estimate. The read-path model states the
read path, the bit-to-gauge map with the evidence behind each entry, the
statistical model the reported percentage obeys, and the hardware hazard that
bounds where the poller may read.

Evidence carries three independent axes, because one ordering cannot represent
them. A register readback names no field, and a kernel header establishes no
target behavior.

- **Field authority**: which register bit carries which name and meaning. Kernel
  and vendor register definitions hold this.
- **Target observation**: whether a bit was seen set or clear on RS482 silicon,
  under which workload, with which sampler.
- **Exposure policy**: whether radeontop renders a lane, which is a product
  decision that follows from the first two rather than a property of silicon.

## Why the read path forks by family

The radeon DRM `RADEON_INFO_READ_REG` ioctl is gated per ASIC by
`get_allowed_info_register`. For the whole pre-R600 family the callback is
`radeon_invalid_get_allowed_info_register`, which returns `-EINVAL` for every
register. The ioctl therefore carries no engine-busy signal on R300-class parts,
and no register whitelist entry can be added from userspace.

Two consequences follow. The libdrm path binds only on R600 and later, where
engine-busy is `GRBM_STATUS` (`0x8010`). R300-class engine-busy is `RBBM_STATUS`
(`0x0E40`), read directly from the register BAR.

`radeon.c::init_radeon` skips the probe outright when the resolved family is
below `R600`, because a probe the kernel rejects by design prints a misleading
`EINVAL` and proves nothing.

### BAR mapping and the aperture boundary

`detect.c::open_pci` maps BAR2 through
`/sys/bus/pci/devices/<domain>:<bus>:<dev>.<func>/resource2` rather than
`/dev/mem`. The `STRICT_DEVMEM` and `IO_STRICT_DEVMEM` kernel options refuse
`/dev/mem` access to driver-claimed device MMIO, so the legacy path dies on a
modern kernel. The `resourceN` file is the BAR aperture itself, so its file
offset is BAR-relative and carries no base-address term, and `MAP_SHARED` makes
reads reach the device.

The aperture size is measured. On the RS482 target sysfs reports BAR2 as
`0xc0100000-0xc010ffff`, so the register BAR is 64 KiB and the R600+ GRBM window
at `0x8000` lies inside it.

`CONFIG_REG_APER_SIZE` (`0x0110`) reports `0x00008000` for a different
aperture; it does not report the MMIO register BAR size. The direct BAR
measurement falsifies a 32 KiB register-aperture interpretation. The family
branch in `open_pci` rests on capability rather than addressability -- GRBM is
an R600 construct that R300-class parts do not implement -- and the size check
is an independent guard for a BAR too small to decode the mapped window.

`RBBM_STATUS` at `0x0E40` sits inside the window mapped at offset 0 for the SRBM
registers (`SRBM_MMAP_SIZE = 0xE54`), so `getgrbm_pci_r300` reads it from
`srbm_area` without a second mapping. Active `steinmarder-r300` findings report
completed direct reads and the idle word `0x00000140`, and the hazard policy
admits RBBM_STATUS offset `0x0E40`. Those findings lack a complete decision-grade run bundle,
so the candidate capture in `docs/open-work.md` owns the admission gate.

`radeontop -m` forces the family-validated direct MMIO path. With `-p`, the DRM
node selects an exact PCI BDF and `-m` forces the direct path for that same BDF.

### The read range is a safety boundary, not hygiene

On this platform an MMIO read that never completes is unrecoverable. The K8
on-die northbridge on the Dell Vostro 1000 reads `D18F3x44 = 0x0A100040`:
`WdogTmrDis` (bit 8) clear, so the northbridge watchdog is enabled, and
`SyncOnWdogEn` (bit 20) set, so a watchdog timeout floods every HyperTransport
link with sync packets. Per AMD BKDG #32559 sections 4.4.5.3 and 4.6.4.7 the
watchdog covers northbridge accesses awaiting a response, and this part is
single-socket, so the only HyperTransport link is the I/O link to the RS482 host
bridge. A non-posted read of a clock-gated RS482 register therefore times out
and floods.

A gated experiment cleared `SyncOnWdogEn` and re-read a confirmed wedge offset:
both cores still froze and the machine needed a manual power cycle. The sync
flood is therefore not the proximate cause, and the freeze survives its removal.
The operative conclusion for radeontop is direct: a read of the
wrong offset on this hardware ends the session with a cold power cycle, no log,
and no software exit.

A free-running poller reading a mapped BAR at 120 Hz creates the hazardous
access shape. Two consequences bind radeontop-gororoba. The RS482 collector
dereferences only `RBBM_STATUS` at `0x0E40`; mapping an aperture proves no other
offset safe. Any new lane reading an unproven offset belongs in a gated probe
with boot-ID capture in the evidence lane rather than in radeontop.

## RBBM_STATUS field map

Field authority is `R_000E40_RBBM_STATUS` in `r300d.h`, read from the
retained Linux source at
`steinmarder-r300:docs/external_sources/rs480_r300_registers_and_driver_sources/raw/source/linux/r300d.h`.
The adjacent `rs400d.h` carries identical definitions. The deployed kernel
revision remains a separate run-identity field.

Target observation is the finite histogram reported by the active,
noncanonical finding in `Provenance` below. "Set" means the finding records the
bit set in at least one sample; "clear" means the finding records it clear in
every sampled workload. The absent raw bundle bounds this to a claim input
rather than admitted decision-grade silicon evidence.

| Bit | `r300d.h` field | Field kind | Target observation | radeontop lane | Exposed |
|---|---|---|---|---|---|
| 0-6 | `CMDFIFO_AVAIL` | 7-bit count | varies per sample | none | no |
| 8 | `HIRQ_ON_RBB` | queue condition | set | none | no |
| 9 | `CPRQ_ON_RBB` | queue condition | clear | none | no |
| 10 | `CFRQ_ON_RBB` | queue condition | clear | none | no |
| 11 | `HIRQ_IN_RTBUF` | queue condition | clear | none | no |
| 12 | `CPRQ_IN_RTBUF` | queue condition | clear | none | no |
| 13 | `CFRQ_IN_RTBUF` | queue condition | set | none | no |
| 14 | `CF_PIPE_BUSY` | block busy | set under load | `cf` | yes |
| 15 | `ENG_EV_BUSY` | block busy | set under load | `ee` | yes |
| 16 | `CP_CMDSTRM_BUSY` | block busy | set under load | `cp` | yes |
| 17 | `E2_BUSY` | block busy | clear | `e2` | on assertion |
| 18 | `RB2D_BUSY` | block busy | clear | `rb2d` | on assertion |
| 19 | `RB3D_BUSY` | block busy | clear | masked off | no |
| 20 | `VAP_BUSY` | block busy | set, 1 of 250 samples | `vgt` | yes |
| 21 | `RE_BUSY` | block busy | clear | masked off | no |
| 22 | `TAM_BUSY` | block busy | clear | masked off | no |
| 23 | `TDM_BUSY` | block busy | clear | masked off | no |
| 24 | `PB_BUSY` | block busy name | clear | masked off | no |
| 25 | `TIM_BUSY` | block busy | clear | masked off | no |
| 26 | `GA_BUSY` | block busy | set under load | `pa` | yes |
| 27 | `CBA2D_BUSY` | block busy | clear | `rb2d` | on assertion |
| 31 | `GUI_ACTIVE` | aggregate busy | set under load | `gui` | yes |

### Lane predicates

A lane is a mask, and its sample value is a predicate rather than a counter:

```text
X_i = ((status_i & lane_mask) != 0)
```

`rb2d` masks bits 18 and 27 together, so its reported value is the fraction of
samples in which `RB2D_BUSY` or `CBA2D_BUSY` was set. It carries no per-block
resolution, and the two backends cannot be separated from its output.

### Lane labels

Three UI labels are inherited from the R600 lane set and overstate what the R300
field names establish. `vgt` renders as "Vertex Grouper + Tesselator" while the
R300 field is `VAP_BUSY`; `cf` renders as "Cmd Fetch" while the field names only
`CF_PIPE`; `rb2d` renders as "2D Backend" for a two-bit union. The precise
R300-side names are `VAP`, `CF pipe`, and `RB2D or CBA2D`, and the rendered
labels are interpretations layered on those.

## Provenance

The rank-1 histogram report lives at
`steinmarder-r300:src/re/r300/findings/active/2026-06-10-rs482-rbbm-backend-busy-bits-nonlatching-under-load.md`,
marked `canonical: false`. The finding file's audit-snapshot SHA-256 is
`78956562f2264bc059886b83a94fe4040b57138f387cae69277d79b86563471e`.
That digest identifies the prose input rather than a silicon-run bundle. The
finding lacks a complete bundle hash, boot ID, exact command set, and binary and
kernel identities, so its measurements remain bounded supporting evidence.

| Field | Value |
|---|---|
| Target | RS482, PCI `1002:5974`, host `cachyos-vostro1000` |
| Sampler | `radeontool regmatch 0xe40`, 250 reads |
| Workload | `glmark2-es2` texture scene, 800x600 fill at 126 FPS, live X server on r300 |
| Samples | 157x `0x00000140`, 91x `0x8401C100`, 1x `0x84116100`, 1x `0x8401C12B` |
| Aggregate OR | `0x8411E16B` |

The four raw words matter individually. `0x8401C100` asserts
`GUI_ACTIVE|GA_BUSY|CP_CMDSTRM_BUSY|ENG_EV_BUSY|CF_PIPE_BUSY`. The rare
`0x84116100` sample asserts
`GUI_ACTIVE|GA_BUSY|VAP_BUSY|CP_CMDSTRM_BUSY|CF_PIPE_BUSY|CFRQ_IN_RTBUF` and
clears `ENG_EV_BUSY`. It is not the dominant busy word plus VAP and CFRQ. This
bitwise decomposition prevents a composite label from inventing an assertion
that the raw word does not contain.

The unretained `0x8411E17C` aggregate attributed to an uncapped `glxgears` run
differs from the retained
histogram in bits 0, 1, 2, and 4, all inside the `CMDFIFO_AVAIL` field, and no
raw capture backing it is present in the evidence lane. The read-path model cites the
traceable histogram only. The two aggregates agree on every busy bit above 13.

A citation that supports a measurement names the repository and commit, the
file, the target PCI ID, the boot or run ID, the sampler command, the workload
command, the sample count, and the digest. A pointer to a directory is a search
instruction rather than provenance.

## What the aggregate OR does and does not establish

The low seven bits are `CMDFIFO_AVAIL`, a count of free command-FIFO entries.
Bitwise OR across samples of an encoded count is not a maximum, a minimum, a
mean, or an occurrence test for any particular value. It establishes only which
encoding bits were set in at least one sample.

The retained aggregate `0x8411E16B` has low bits `0x6B`, and no single sample in
the histogram contains that value: the four observed samples carry FIFO fields
`0x40`, `0x00`, `0x00`, and `0x2B`. A general counterexample makes the point
without reference to this data set:

```text
observed counts:  64, 32, 16, 8, 4
bitwise OR:       124
maximum observed: 64
```

No sample equals 124. Reading a scalar FIFO depth out of an aggregate OR is an
invalid operation on a packed numeric field. FIFO behavior requires decoding
`CMDFIFO_AVAIL` in each raw sample and retaining the resulting distribution or
time series.

Bits 8 through 13 need a narrower correction. They report request-queue
conditions rather than block activity, so they are unsuitable as engine-busy
gauges. They are not informationless: a per-sample mean over those bits would
estimate the fraction of sampled instants at which each queue condition held.
Only their OR is uninformative, and for the same reason.

## What the clear back-end bits establish

`RB3D_BUSY`, `RE_BUSY`, `TAM_BUSY`, `TDM_BUSY`, and `TIM_BUSY` are clear in every
retained sample under a sustained textured fill that must exercise the
rasterizer, the texture units, and the 3D render backend.

That observation does not refute the register decode. The kernel header
establishes field positions, and a finite sampler that never observes a bit set
gives negative evidence about that bit's **observability on this target under
these conditions**. Establishing that the silicon does not aggregate those
signals additionally requires an independent witness that the blocks were active
at the sampled instants, and enough temporal resolution to bound missed pulses.
Neither exists in the retained record; the sibling finding preserves exactly
that falsifier and is marked non-canonical.

The exposure decision stands on its own footing and is a fail-closed product
policy rather than a silicon claim:

> A lane that has never asserted on the target under any recorded workload
> renders as a permanent `0.00%`, which a reader cannot distinguish from a
> confirmed-idle block. Such lanes stay hidden until a sample captures an
> assertion.

`initbits(RS480)` implements that policy by zeroing those masks, and `dump.c`
and `ui.c` suppress any lane whose mask is zero.

### The 2D lanes have three live explanations

`E2_BUSY`, `RB2D_BUSY`, and `CBA2D_BUSY` are clear in the retained histogram,
which the 3D workload explains on its own. A separate `x11perf -copywinwin500`
run at 235 copies per second drove `gui`, `pa`, and `cp` to 90 percent and left
the 2D lanes idle.

The categorical claim that the modern DDX routes copies through the 3D engine
is false as a general claim about
`xf86-video-ati`: `src/radeon_exa_funcs.c` implements `RADEONCopy` over the
hardware 2D engine and submits `Emit2DState(pScrn, RADEON_2D_EXA_COPY)`, and
that legacy path is the one selected for R300-class parts. The statement can
still hold for the specific tested server if it selected glamor rather than EXA,
which the retained record does not establish.

Three explanations therefore remain live:

1. The tested acceleration path bypassed the hardware 2D engine, for example
   through glamor.
2. RS482 does not aggregate the 2D block busy signals into `RBBM_STATUS`.
3. The 2D assertions were shorter than the sampling interval or phase-correlated
   with it.

The discriminator is a run that records the X server's acceleration method, from
the Xorg log or the configured `AccelMethod`, alongside a register or
command-stream trace. Until that runs, the zero 2D lanes select no explanation.

## Falsifiers

- A masked-off lane reopens when any workload or configuration asserts its bit
  on RS4xx silicon: a legacy userspace driving longer per-draw rasterization
  windows, a different clock-gating configuration through `SCLK_CNTL` dynamic
  stop latches, or a kernel that programs RBBM differently.
- A higher-rate sampler that catches sub-millisecond assertions the 120 Hz
  monitor and the `regmatch` loop both alias over overturns every
  non-observation above.
- The 2D lanes confirm when a run with EXA acceleration recorded asserts bit 17,
  18, or 27.
- The FIFO count interpretation refutes if a per-sample decode shows bits 0
  through 6 behaving as independent flags rather than an encoded count.

## The reported percentage is a sampled duty cycle

`collector.c::collector_worker` samples a level signal and counts predicate
hits. It reads no hardware performance counter, so the reported percentage is a
statistical estimate rather than a direct occupancy counter.

One scheduled window contains `N = ticks * dumpinterval` nominal slots. The
collector attempts `A` slots, gives up `M` slots whose sample points have passed,
validates `V` reads of the lane's source register, records `F` failed reads, and
observes `B` busy predicates. The schedule maintains `A + M = N`. A lane reports

```text
p_conditional = B / V
```

over successful reads, and renders `N/A` when `V = 0`. The dump appends the
exact `B`, `V`, and `N` plus the assumption-free interval

```text
p_lower = B / N
p_upper = (B + N - V) / N
```

The interval assigns every unobserved slot idle at the lower endpoint and busy
at the upper endpoint. It remains valid when missed or failed reads correlate
with load. The point estimate and both bounds describe sampled instants;
equating them with elapsed-time duty still requires representative sample
timing. Defaults are `ticks = 120` and `dumpinterval = 1`, giving `N = 120`.

### Sampling correlated with the workload biases the estimate

Each `X_i` is a Bernoulli draw whose expectation equals the true occupancy only
when the sample instants are uncorrelated with the workload's phase. Commensurate
periods bias it in either direction, and a frame-locked renderer against a
fixed-rate sampler is exactly such a pairing. The failure mode is phase locking
at any commensurate frequency rather than activity above any particular rate;
Nyquist governs reconstruction of a band-limited waveform and does not set a
threshold for estimating the mean of a binary level process. A 1 Hz workload
sampled at 1 Hz with fixed phase is maximally biased, and a 1 kHz pulse train
sampled at randomized phase is estimated well.

### The IID error bound is a reference value, not the instrument's uncertainty

For independent, fully observed samples the standard error is
`sqrt(p(1-p)/N)`, which is 2.7
percentage points at `p = 0.9` and `N = 120`. GPU busy states carry temporal
autocorrelation: adjacent samples fall inside the same frame, command batch, and
scheduler episode. For a stationary binary series the variance is

```text
Var(p_sample) = (1/N^2) * [ N*g_0 + 2 * sum((N-k) * g_k for k in 1..N-1) ]
```

with `g_k` the lag-`k` autocovariance. Positive autocorrelation shrinks the
effective sample size, so the IID figure is a lower bound on the true spread. The
88 to 90 percent band the command-stream lane occupies in the recorded run is
consistent with that figure but is not explained by it; separating them needs an
autocorrelation estimate or repeated independent windows, neither of which is
retained.

### The sample grid is absolute, and its shortfall is reported

A relative sleep after each read adds read time to every period, so the achieved
rate falls below the requested `ticks` and the offset accumulates as drift.
Scheduler delay can correlate with the workload or with its submission thread, so
that delay is not independent of what is being measured.

The collector instead waits on absolute `CLOCK_MONOTONIC` deadlines laid on a
fixed grid, carrying the division remainder so a rate that does not divide a
second exactly still lands exactly one second later after `ticks` slots. A
wake-up that arrives late gives up the slots it overran rather than firing
catch-up reads, and a read that outlasts its own period gives up the deadlines it
crossed, so a slow device returns to the grid instead of reading back to back.
Each published window retains its scheduled start and end, the attempted and
missed slot counts, the late wake-up count, the maximum lateness, and the maximum
read latency, which makes the achieved sampling process observable rather than
assumed.

A sample measures the device at the instant it is taken, so it is attributed to
the window containing that instant. A wake-up that crosses one or more window
boundaries publishes those windows first, with no attempt of their own, before
the read runs.

Sampling on a fixed grid remains a periodic process, so a workload periodic at a
harmonic of the sampling rate can still be sampled at a fixed phase.
`--dither-seed N` moves each deadline by a reproducible offset inside its own
slot. Let slot width be `T`, dither offset be `D`, and wake lateness be `L`.
The sample survives only when `D + L < T`, so a uniform dither gives
`P(accepted | L) = max(0, 1 - L/T)` and accepted offsets occupy `[0,T-L)`.
Dithering breaks fixed phase while scheduler delay selects against late-slot
phases. Coverage reports the amount of selection, and the unconditional bounds
cover its missing state; neither establishes uniform accepted phase. Each slot
uses its exact carried integer width, and rejection sampling removes modulo bias
from the splitmix64-to-slot projection.

## Reproduction

```sh
./radeontop -m -d - -t 120 -i 1          # sample the gauges, one line per second
radeontool regmatch 0xe40                # raw register histogram, sibling tool
```

Loads used for the retained observations:

```sh
glmark2-es2 --benchmark texture          # sustained textured fill, 126 FPS
x11perf -copywinwin500                   # 2D copy, 235 copies per second
```

Retained bundles, probe scripts, hazard policy, and the verdict assigned to each
run live in `steinmarder-r300` under `src/re/r300/`. Radeontop-gororoba carries the
citation, not the bundle.

## Reported observations pending bundle admission

The active finding reports the values below on the RS482 target at 120 Hz. It
does not retain the complete boot, command, binary, kernel, and artifact-hash
surface required for canonical admission. The values guide reproduction and
carry the uncertainty discussed above; they do not become tighter than the IID
reference or decision-grade through repetition in the read-path model.

| Load | gpu | cp | ee | pa | vgt | Memory |
|---|---|---|---|---|---|---|
| Idle | 0 | 0 | 0 | 0 | 0 | none |
| `glmark2-es2` texture, 126 FPS | 90 | 88-90 | 89 | 90 | 0.8-5.8 | VRAM 31 to 56 percent |
| `x11perf -copywinwin500`, 235 copies/s | 90 | 90 | not recorded | 90 | not recorded | VRAM 97.8 percent, GTT 814 MB |

`ta`, `sc`, `cb`, and `db` stayed at zero across every sample of three runs
(idle, a 136-case deqp texture-filtering run, and the sustained fill).

### Unretained on-target run report

A prior local report names `cachyos-vostro1000`, RS482 PCI `1002:5974`, kernel
7.1.3-2-cachyos, a merged source revision, and `radeontop -m` at 120 Hz. Its raw
bundle is absent from `steinmarder-r300`, so the table remains an unadmitted
reproduction target.

| Load | gpu | pa | cf | ee | cp | vgt | e2 | rb2d |
|---|---|---|---|---|---|---|---|---|
| Idle, 3 samples | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `glxgears`, vsync-locked 59.4 FPS, 5 samples | 30.00-44.17 | 30.00-44.17 | 30.00-44.17 | 24.17-41.67 | 7.50-10.83 | 2.50-6.67 | 0 | 0 |

`gpu`, `pa`, and `cf` carry bit-identical values in every sample of the run, so
`GUI_ACTIVE`, `GA_BUSY`, and `CF_PIPE_BUSY` assert together under this load
rather than tracking separable work. Distinguishing them needs a load that
drives geometry setup and command fetch at different rates.

`e2` and `rb2d` stay at zero here as well, which adds a third workload to the
2D-lane non-observation without discriminating among its three explanations.

### The same run demonstrates the low-N sampling limit

Six single-shot `radeontool regmatch 0xe40` reads taken during that load all
returned `0x00000140`, the idle word, with every busy bit clear -- while the
120 Hz sampler concurrently measured `gpu` at 36.67 percent. At `p = 0.3667` the
probability of six consecutive misses is `(1 - p)^6 = 0.064`, so the outcome is
unremarkable rather than contradictory.

The two instruments disagree because one takes 120 samples per report and the
other takes six across the whole window. A handful of instantaneous register
reads is not evidence of an idle block, and a non-observation constrains a duty
cycle only through its sample count.

## GART and MC observation

`detect.c::init_rs480_gart_observed` reads `AGP_BASE_2`, `GART_FEATURE_ID`, and
`GART_BASE` once from
`/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs` before privileges drop,
and `dump.c` emits them as a single `#` header line when all three parse. The
debugfs file comes from the `steinmarder` RS480 candidate-regs lane rather than
stock upstream radeon, so an absent file leaves the header out and the run
behaviorally identical.

## Collector architecture and its acceptance

The collector is an injectable object: a backend supplies the register reads and
their failure classification, a clock supplies time, and the worker owns the
cadence. `include/collector.h` names libc and pthread only, so
`gcc -Iinclude -MM collector.c` lists exactly one project header and the
accumulator, the schedule, and the publication path are exercised without a GPU.
`tests/collector_test.c` drives a scripted backend and a single-stepping virtual
clock that can consume time inside a read, so backend latency and multi-window
lateness are both observable.

Two scheduler properties rest on negative controls rather than on passing
alongside the implementation. Reintroducing the pre-read timestamp in the
post-read deadline skip makes the latency case read five times where the grid
allows four; sampling before rolling the windows a wake-up overran makes the
attribution case count two attempts in a window that saw one. Both mutations
fail the suite, and the implementation passes it.

The target-local report names `valid 500/500, missed 0, failed 0` for a sustained
500 Hz window and describes a relative-sleep control drifting against the
absolute grid. Its exact commands, boot identifier, window counts, binary hash,
and raw output remain absent from `steinmarder-r300`. The figures therefore rest
on the report rather than a retained artifact and establish no admitted rate.
The `1000000` parser bound remains arithmetic and carries no silicon claim.

Not run: the permanent privilege drop, which needs a setuid-root installed binary
because a process launched through `sudo` already has real uid 0 and can only
drop to 0; and the `libdrm_amdgpu` and radeon-ioctl paths, for which no part is
reachable, leaving their `-errno` classification reasoned from API convention and
exercised only by the synthetic backend.

## Open work

Named slices, each independently landable.

- **Evidence**: a run recording the X acceleration method plus a trace, to
  discriminate the three 2D explanations. A higher-rate sampler to bound missed
  assertions on the clear back-end bits. A per-sample `CMDFIFO_AVAIL`
  distribution rather than an aggregate.
- **Collector, MMIO, packaging, and CI**: `docs/open-work.md` carries these,
  grouped by the gate each item waits on, because they span the whole tool
  rather than the modeled read path.
- **Unmapped**: `r300d.h` names `PB_BUSY` at bit 24, while the retained RS482
  samples do not observe it. It stays masked until target evidence supports a
  useful exposure. Active GA and ZB reads in the `0x4xxx` window remain outside
  the poller because the retained `0x42d0` GA read under GUI activity deep-wedged
  the host and ZB active-read behavior remains unobserved.
