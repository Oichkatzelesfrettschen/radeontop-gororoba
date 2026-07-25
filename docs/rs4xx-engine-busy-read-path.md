# RS4xx engine-busy read path, bit map, and duty-cycle estimator

RadeonTop reports R300-class (RS400/RS480/RS482/RS485) GPU utilization by
sampling `RBBM_STATUS` through the BAR2 PCI sysfs `resourceN` node and
accumulating set-bit counts into a duty-cycle estimate. This document states the
read path, the bit-to-gauge map with the evidence rank behind each entry, and the
statistical model the reported percentage obeys.

Evidence ranks follow `AGENTS.md`: rank 1 is silicon readback on the target part,
rank 2 is register documentation, rank 3 is kernel source, rank 4 is radeontop
source, rank 5 is documentation. Every claim below carries its rank.

## Why the read path forks by family

The radeon DRM `RADEON_INFO_READ_REG` ioctl is gated per ASIC by
`get_allowed_info_register`. For the whole pre-R600 family the callback is
`radeon_invalid_get_allowed_info_register`, which returns `-EINVAL` for every
register (rank 3: `radeon_asic.c`). The ioctl therefore carries no engine-busy
signal on R300-class parts, and no register whitelist entry can be added from
userspace.

Two consequences follow. The libdrm path binds only on R600 and later, where
engine-busy is `GRBM_STATUS` (`0x8010`). R300-class engine-busy is `RBBM_STATUS`
(`0x0E40`), read directly from the register BAR.

`radeon.c::init_radeon` skips the probe outright when the resolved family is
below `R600`, because a probe that the kernel rejects by design prints a
misleading `EINVAL` and proves nothing (rank 4).

### BAR mapping

`detect.c::open_pci` maps BAR2 through
`/sys/bus/pci/devices/<domain>:<bus>:<dev>.<func>/resource2` rather than
`/dev/mem`. The `STRICT_DEVMEM` and `IO_STRICT_DEVMEM` kernel options refuse
`/dev/mem` access to driver-claimed device MMIO, so the legacy path dies on a
modern kernel. The `resourceN` file is the BAR aperture itself, so its file
offset is BAR-relative and carries no base-address term, and `MAP_SHARED` makes
reads reach the device (rank 4).

`RBBM_STATUS` at `0x0E40` falls inside the window already mapped at offset 0 for
the SRBM registers (`SRBM_MMAP_SIZE = 0xE54`), so `getgrbm_pci_r300` reads it
from `srbm_area` without a second mapping. The R600+ GRBM window at `0x8000` is
not mapped on this family: it is an R600 construct, and a `resourceN` mmap at
`0x8000` fails outright when the R300 register BAR is smaller than that offset
(rank 4).

`radeontop -m` forces the direct MMIO path on any card, which is also the only
supported path under the Catalyst driver.

## RBBM_STATUS field map

The decode is `R_000E40_RBBM_STATUS` in `r300d.h` (rank 3), read from the
`radeon-custom` DKMS source at
`packaging/arch/radeon-unified-dkms/.../radeon/r300d.h`.

| Bit | `r300d.h` field | radeontop lane | UI label | Rank | Asserting load |
|---|---|---|---|---|---|
| 0-6 | `CMDFIFO_AVAIL` | none | none | 3 | count field, not a flag |
| 8 | `HIRQ_ON_RBB` | none | none | 3 | request queue occupancy |
| 9 | `CPRQ_ON_RBB` | none | none | 3 | request queue occupancy |
| 10 | `CFRQ_ON_RBB` | none | none | 3 | request queue occupancy |
| 11 | `HIRQ_IN_RTBUF` | none | none | 3 | request queue occupancy |
| 12 | `CPRQ_IN_RTBUF` | none | none | 3 | request queue occupancy |
| 13 | `CFRQ_IN_RTBUF` | none | none | 3 | request queue occupancy |
| 14 | `CF_PIPE_BUSY` | `cf` | Cmd Fetch | 1 | sustained 3D fill |
| 15 | `ENG_EV_BUSY` | `ee` | Event Engine | 1 | sustained 3D fill |
| 16 | `CP_CMDSTRM_BUSY` | `cp` | Command Stream | 1 | sustained 3D fill |
| 17 | `E2_BUSY` | `e2` | 2D Engine | 3 | unobserved |
| 18 | `RB2D_BUSY` | `rb2d` | 2D Backend | 3 | unobserved |
| 19 | `RB3D_BUSY` | masked off | none | 1 | clear under 3D fill |
| 20 | `VAP_BUSY` | `vgt` | Vertex Grouper | 1 | sustained 3D fill |
| 21 | `RE_BUSY` | masked off | none | 1 | clear under 3D fill |
| 22 | `TAM_BUSY` | masked off | none | 1 | clear under 3D fill |
| 23 | `TDM_BUSY` | masked off | none | 1 | clear under 3D fill |
| 24 | `PB_BUSY` | masked off | none | 3 | no documented block meaning |
| 25 | `TIM_BUSY` | masked off | none | 1 | clear under 3D fill |
| 26 | `GA_BUSY` | `pa` | Primitive Assembly | 1 | sustained 3D fill |
| 27 | `CBA2D_BUSY` | `rb2d` | 2D Backend | 3 | unobserved |
| 31 | `GUI_ACTIVE` | `gui` | Graphics Pipe | 1 | any engine running |

`initbits(RS480)` in `detect.c` sets exactly these masks and zeroes every lane
without an R300 analogue (`ta`, `tc`, `sx`, `sh`, `spi`, `smx`, `sc`, `cb`, `db`,
`cr`, `uvd`, `vce0`). `dump.c` and `ui.c` gate each lane's output on its mask, so
a family that lacks a block renders no gauge for it rather than a constant zero.

## Decomposing the observed histogram

The rank-1 observation is a raw `RBBM_STATUS` histogram taken with
`radeontool regmatch` on RS482 under an uncapped `glxgears` load. The ORed word
across samples is `0x8411E17C`. Decomposed against the `r300d.h` field map:

- `CMDFIFO_AVAIL` (bits 0-6) ORs to 124 of a 127 maximum.
- `HIRQ_ON_RBB` (8) and `CFRQ_IN_RTBUF` (13) set.
- Busy flags set: `CF_PIPE_BUSY`, `ENG_EV_BUSY`, `CP_CMDSTRM_BUSY`, `VAP_BUSY`,
  `GA_BUSY`, `GUI_ACTIVE`.
- Busy flags clear: `E2_BUSY`, `RB2D_BUSY`, `RB3D_BUSY`, `RE_BUSY`, `TAM_BUSY`,
  `TDM_BUSY`, `PB_BUSY`, `TIM_BUSY`, `CBA2D_BUSY`.

Three results follow from that decomposition.

The low seven bits are a saturating OR over a **count** field, not a busy flag.
`CMDFIFO_AVAIL` reports how many command-FIFO entries are free, so an OR across
samples yields the bitwise union of every depth observed, and its value carries
no duty-cycle meaning. Gauging any of bits 0-6 is a category error, and the
value 124 records only that the FIFO reached near-empty occupancy at some point
during the run. The same argument disqualifies bits 8 through 13, which report
request-queue occupancy rather than block activity.

A busy bit that stays clear under a load that must exercise its block refutes
the decode's applicability to this derivative. The rasterizer (`RE_BUSY`), the
texture units (`TAM`, `TDM`, `TIM`), and the 3D render backend (`RB3D_BUSY`)
cannot be idle during a sustained textured fill, so their permanent clearness
means this RBBM does not aggregate those back-end busy signals. That inference
is stronger than the kernel header, because the header documents the field
layout and not which signals a given derivative wires into it. Perpetual-zero
gauges mislead, so those lanes stay masked.

The 2D lanes are mapped from rank 3 and lack rank-1 confirmation. `E2_BUSY`,
`RB2D_BUSY`, and `CBA2D_BUSY` stay clear in this histogram, which the 3D
workload explains. A separate `x11perf -copywinwin500` run also left them idle
at `gpu`/`pa`/`cp` 90 percent, because the modern DDX routes copies through the
3D engine rather than the 2D block. The lanes therefore rest on the kernel
decode alone, and no observed load has yet asserted them.

### Falsifiers

- A masked-off lane reopens when a load that exercises its block asserts its bit
  on RS4xx silicon. `RB3D_BUSY` under a load that bypasses the 2D path, or
  `TAM_BUSY` under a texture-bound fill, refutes the non-aggregation finding.
- The 2D lanes confirm when a workload that reaches the 2D block asserts bit 17,
  18, or 27. A DDX build with 3D acceleration disabled is the candidate load.
- The `CMDFIFO_AVAIL` reading refutes the count interpretation if bits 0-6
  behave as independent flags, which a histogram of per-sample values rather
  than their OR would show.

## The reported percentage is a sampled duty cycle

`ticks.c::collector` samples a level signal and counts set bits. It carries no
hardware performance counter, so the reported percentage is a statistical
estimate rather than a measurement of occupancy.

The collector sleeps `sleeptime = 1e6 / ticks` microseconds between samples and
accumulates over `N = ticks * dumpinterval` samples per report. `dump.c` and
`ui.c` normalize with `k = 1.0f / ticks / dumpinterval`, so a lane's reported
value is

```text
p_hat = (1 / N) * sum(b_i for i in 1..N) * 100 percent
```

where `b_i` is the lane's bit in sample `i`. Defaults are `ticks = 120` and
`dumpinterval = 1`, giving `N = 120` samples at a nominal 8333 microsecond
interval.

Three properties follow, and each bounds how the number may be read.

**The estimator is unbiased only under sampling independent of the workload.**
Each `b_i` is a Bernoulli draw from the block's true duty cycle `p`. The mean of
`p_hat` equals `p` when the sample instants are uncorrelated with the workload's
period. A workload whose period is commensurate with the 8333 microsecond
interval biases the estimate in either direction, and a frame-locked renderer is
exactly such a workload.

**Precision is bounded by `N`, not by the sampling hardware.** The standard
error is `sqrt(p(1-p)/N)`. At `p = 0.9` and `N = 120` that is 2.7 percentage
points at one sigma, which accounts for the 88 to 90 percent band the command
stream lane occupies in the rank-1 fill run. Reading a one-point difference
between two lanes as a real difference overreads the estimator. Raising `ticks`
narrows the interval as `1/sqrt(N)` and raises polling cost linearly.

**Activity faster than half the sample rate aliases.** The sampler resolves duty
cycles of processes slower than 60 Hz at the default rate. A block toggling near
120 Hz folds to an arbitrary apparent duty cycle, so a lane that hovers at an
implausible steady value under a varying load is a candidate alias rather than a
finding.

`usleep` guarantees a lower bound on the sleep, not an exact interval, so the
true interval carries scheduler jitter and exceeds the nominal value under load.
That lengthens the accumulation window rather than skewing individual draws, and
it leaves `N` intact because the loop counts samples rather than elapsed time.

## Reproduction

Sample the gauges, dump one line per second:

```sh
./radeontop -m -d - -t 120 -i 1
```

Loads used for the rank-1 observations:

```sh
glxgears                                  # uncapped 3D, the histogram load
glmark2-es2 --benchmark texture           # sustained textured fill, 126 FPS
x11perf -copywinwin500                    # 2D copy, routed through the 3D engine
```

Capture the raw register histogram with the sibling tool:

```sh
radeontool regmatch RBBM_STATUS
```

Retained bundles, probe scripts, hazard policy, and the verdict assigned to each
run live in `steinmarder-r300` under `src/re/r300/`. This repository carries the
citation, not the bundle.

## Rank-1 observations on record

Measured on the RS482 target. Loads are named; unnamed conditions are not
measured.

| Load | gpu | cp | ee | pa | vgt | Memory |
|---|---|---|---|---|---|---|
| Idle | 0 | 0 | 0 | 0 | 0 | none |
| `glmark2-es2` texture, 126 FPS | 90 | 88-90 | 89 | 90 | 0.8-5.8 | VRAM 31 to 56 percent |
| `x11perf -copywinwin500` | 90 | 90 | not recorded | 90 | not recorded | VRAM 97.8 percent, GTT 814 MB |

Values are percentages from the estimator above and carry its 2.7 point standard
error at `N = 120`. The `cf` lane postdates these runs and has its assertion
recorded in the histogram rather than a percentage.

## GART and MC observation

`detect.c::init_rs480_gart_observed` reads `AGP_BASE_2`, `GART_FEATURE_ID`, and
`GART_BASE` once from
`/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs` before privileges drop,
and `dump.c` emits them as a single `#` header line when all three parse. The
debugfs file comes from the `steinmarder` RS480 candidate-regs lane rather than
stock upstream radeon, so an absent file leaves the header out and the run
otherwise unchanged (rank 4).

## Open work

- The 2D lanes (`e2`, `rb2d`) need a load that reaches the 2D block for rank-1
  confirmation.
- `PB_BUSY` (bit 24) carries no block meaning in `r300d.h` and stays unmapped
  until a source names it.
- The per-block cache status registers in the `0x4xxx` window are the readable
  alternative to the non-aggregating back-end busy bits. They belong to a gated
  probe lane rather than a free-running poller, because a poller reading them
  adds bus traffic on every tick.
