# Open work

The packaged binary carries the tree. `PKGBUILD` pins the merge that holds the
sample attribution fix, the volatile MMIO reads, and the dithered sample phase,
each of which passed acceptance on RS482 silicon. `master` requires the ten
workflow checks of every merge, administrators included, so the gates the
workflows run are a boundary rather than a convention.

Items below are grouped by the gate each one waits on. Ordering inside a group
carries no dependency; every item is independently landable.

## Landed mechanisms

The tree at `master` already carries these, so a remaining item that appears to
restate one is asking for something narrower.

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
  periodic at a harmonic of the sampling rate meets a moving phase. The offset
  lies in `[0, period)` and slot membership follows the grid, so slot count,
  window boundaries, and the attempted-plus-missed identity are unchanged.
  Omitting the flag keeps the exact grid, which is what makes two runs of one
  workload directly comparable.
- The unit suite runs 634 checks against an injected backend and a virtual
  clock, under gcc, clang, ASan/UBSan/LSan, and ThreadSanitizer. Five scheduler
  properties carry negative controls: mutating either schedule advance, giving
  up a slot whose sample point is still ahead, letting the dither exceed its
  slot, and discarding the drawn offset each make the suite fail.
- `.clangd` at the repository root and under `tests/` supply the include paths,
  the language, and the standard the Makefile passes, so editor diagnostics
  match the build.

## Silicon evidence carried

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

Dithering is therefore usable where the period is large against the wake-up
lateness the host produces, and it is not usable at rates where the two are
comparable. The per-window coverage figure reports the cost, so a capture
carries its own evidence either way.

A fourth run drove the rate up until the slot period fell under the read cost,
which is the falsifier for the post-read deadline skip. `max_read_latency_ns` is
measured inside `sample_once`, bracketing the backend calls alone, so a window
reporting a read cost above its own period had deadlines behind it when the read
returned.

- The BAR read cost has a long tail on this part under load, independent of the
  schedule. Worst costs per pass run 341 to 913 microseconds across 1000, 2000,
  4000, and 8000 samples per second, seeded and unseeded alike, against typical
  costs under 60. An earlier reading that paired a 588 microsecond cost with a
  seeded pass reflects that tail rather than the dither, which changes when a
  sample is taken and not how long the load takes.
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

## Ready now

1. The `late` counter reports every wake-up on this host at 120 and 500 samples
   per second, because a condition-variable wake arrives after its deadline
   essentially always. A count that saturates carries no signal; `max_lateness`
   carries it. Either report lateness as a distribution or state the counter's
   meaning where it is rendered.
2. The dither draws from the whole period, so an offset near the top of the
   range leaves a sample less slack than the host's wake-up lateness consumes,
   and the slot is given up. A bound below the period trades phase coverage for
   slot coverage, and the exchange rate is measurable: sweep the bound against a
   fixed load and rate, and read the coverage figure the window already reports.

## Gated on a run against RS482 silicon

3. The permanent privilege drop, which needs a temporarily installed
   setuid-root binary invoked as the ordinary user. A `sudo` run cannot exercise
   it, because the real uid is already 0 and `Uid: 1000 0 0 0` on sudo's own
   process reads like a failed drop.
4. The read-cost tail itself. Costs of 341 to 913 microseconds appear against
   typical costs under 60 on the same pass, and they vary by an order of
   magnitude between boots, so the cause lies outside the sampler. A read
   bracketed by `sample_once` covers the MMIO load and the scheduling around it,
   so preemption during a read and a slow BAR access are not yet separated. The
   discriminator is a run pinned to one CPU with the sampler at a real-time
   policy: a tail that survives both is the device.

## Ready without silicon

5. `makerepropkg` rebuilds a package against the `.BUILDINFO` package set from
   the Arch Linux Archive and needs a privileged chroot plus exact archive
   resolution the container job cannot supply. It reads `not run` with that
   reason, and the workflow proves build-to-build determinism at a pinned
   `SOURCE_DATE_EPOCH` instead.

## Carried elsewhere

Register-mapping and silicon-evidence work stays under Open work in
`docs/rs4xx-engine-busy-read-path.md`, which owns the read-path model those
items constrain. Retained probes, result bundles, and the verdict assigned to a
target-silicon run live in `steinmarder-r300` under `src/re/r300/`, and this
repository carries the citation.

6. The `libdrm_amdgpu` and radeon-ioctl read paths stay unexercised on silicon,
   because no amdgpu or R600 part is reachable from either available host.
   Their `-errno` handling is reasoned from API convention only, which places
   it at rank 5 until a part answers.
