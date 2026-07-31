# Open work

The packaged binary lags the tree. `PKGBUILD` pins `_commit`
`49d7a7affe1a8b9a8da83733aca4f0c65daefdd0` and derives
`pkgver=1.4.r53.g49d7a7affe1a`, while `master` carries the sample-attribution
and per-signal-validity change on top of it. An installed package therefore
still attributes a sample taken during a long read to the window that was open
when the read started, and still dates a window by the time its endpoint reads
finished. Advancing the pin is the delivery step, and the RS482 acceptance that
gates it has passed.

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
  after the next grid point rather than from read latency. That exercises the
  pre-read skip on silicon and leaves the post-read skip unexercised.
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

The first run's load comparison is void: it drove the load with a `glmark2-es2`
invocation that exits after one benchmark, so the load had ended before the test
binary ran and its windows recorded an idle device. The second run holds the
load with `--run-forever` and proves it alive on both sides of every device
pass. A load pass that does not record the load's liveness cannot distinguish an
idle device from a missing load.

## Ready now

1. Advance `_commit` to the accepted merge, recompute `pkgver` as
   `1.4.rN.g<abbrev-object-id>`, and reset `pkgrel` to 1. The pinned commit is
   reachable from `master` before it enters the recipe.
2. Prove the recipe's literal `pkgver` equals what `pkgver()` derives, with
   `makepkg --nobuild && git diff --exit-code PKGBUILD`.
3. A clean `makepkg` from a directory outside the checkout, the packaged
   binary's `--version` equal to `pkgver`, and a second clean build producing a
   byte-identical artifact at the pinned `SOURCE_DATE_EPOCH`.
4. Branch protection on `master`, which the GitHub API reports absent, so a
   merge can bypass the gates the workflows run. The required checks are the
   six compile-matrix jobs `minimal / gcc`, `minimal / clang`, `radeon / gcc`,
   `radeon / clang`, `full / gcc`, `full / clang`, plus `static analysis`,
   `collector integrity`, `command-line contracts`, and `arch package`.
5. The administrator-bypass posture, which is a policy choice rather than a
   defect: enforcing the checks for administrators closes the bypass and also
   removes the escape hatch for a broken required check.
6. The `late` counter reports every wake-up on this host at 120 and 500 samples
   per second, because a condition-variable wake arrives after its deadline
   essentially always. A count that saturates carries no signal; `max_lateness`
   carries it. Either report lateness as a distribution or state the counter's
   meaning where it is rendered.

## Gated on a run against RS482 silicon

7. Acceptance of the volatile MMIO reads on the affected family, which the
   validation table requires of a read-path change. The reads are the same
   registers at the same offsets, so the falsifier is a change in the busy
   figures or the read cost against the runs recorded above.
8. Acceptance of the dithered schedule: a seeded run and an unseeded run against
   one steady load agree on the duty figure within sampling error, and the
   seeded run's `attempted + missed` still equals `nominal`.
9. The permanent privilege drop, which needs a temporarily installed
   setuid-root binary invoked as the ordinary user. A `sudo` run cannot exercise
   it, because the real uid is already 0 and `Uid: 1000 0 0 0` on sudo's own
   process reads like a failed drop.
10. The post-read deadline skip, which has virtual-clock coverage only. No BAR
    read on this part has cost more than 59 microseconds against a 1000
    microsecond period, so the branch stays unexercised on silicon. A rate above
    16000 samples per second would bring the period under the observed read
    cost, which is the available falsifier.

## Ready without silicon

11. `makerepropkg` rebuilds a package against the `.BUILDINFO` package set from
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

12. The `libdrm_amdgpu` and radeon-ioctl read paths stay unexercised on silicon,
    because no amdgpu or R600 part is reachable from either available host.
    Their `-errno` handling is reasoned from API convention only, which places
    it at rank 5 until a part answers.
