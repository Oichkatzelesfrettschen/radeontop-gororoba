# Open work

The packaged binary lags the tree. `PKGBUILD` pins `_commit`
`49d7a7affe1a8b9a8da83733aca4f0c65daefdd0` and derives
`pkgver=1.4.r53.g49d7a7affe1a`, while `master` carries the sample-attribution
and per-signal-validity change on top of it. An installed package therefore
still attributes a sample taken during a long read to the window that was open
when the read started. The pin advances once the change passes a run on RS482
silicon, so the target's availability is the critical path for delivery, not
only for evidence.

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
- The unit suite runs 551 checks against an injected backend and a virtual
  clock, under gcc, clang, ASan/UBSan/LSan, and ThreadSanitizer. The two
  scheduler properties carry negative controls: mutating either schedule advance
  makes the suite fail.
- `.clangd` supplies the include path and standard the Makefile passes, so
  editor diagnostics match the build.

## Gated on a run against RS482 silicon

The validation table requires a read-path or gauge change to carry a build plus
a run on the affected family. These items need the target powered on. Reads stay
confined to `RBBM_STATUS` (`0x0E40`) through the BAR2 `resourceN` window, the
offset carrying a retained same-boot safe-read observation, because a wrong MMIO
offset on this platform is a cold power cycle (K8 northbridge watchdog,
`D18F3x44`, AMD BKDG #32559). The boot identifier is captured around every
device pass.

1. Acceptance of the sample-attribution change: a control worktree at the
   pre-change merge and a test worktree at `master`, each run at idle and under
   a sustained textured fill. An all-zero idle reading is equally consistent
   with a broken read path, so every idle reading is paired with a load.
2. A rate ladder at 120, 500, and 1000 samples per second under load, recording
   `attempted`, `missed`, `late`, `maxlate`, and `maxread` per window. The
   prediction is `attempted + missed == nominal` at every rate, with `missed`
   rising only where the read latency exceeds the period.
3. Grid-offset stability: the published sub-second offset holds window over
   window. A relative sleep drifts forward monotonically by the read time each
   period, so a drifting offset refutes the absolute grid.
4. The negative controls re-run on the target, so the calibration that proves
   the scheduler tests detect the defect travels with the run rather than being
   asserted from the development host.
5. Shutdown paths on the target binary: SIGINT, SIGTERM, and the line limit each
   close the dump and exit within a bounded interval.
6. Output-failure propagation: a dump directed at `/dev/full` exits nonzero.
7. The permanent privilege drop, which needs a temporarily installed
   setuid-root binary invoked as the ordinary user. A `sudo` run cannot exercise
   it, because the real uid is already 0 and `Uid: 1000 0 0 0` on sudo's own
   process reads like a failed drop.
8. The post-read deadline skip has virtual-clock coverage only. No BAR read on
   this part has stalled in any run to date, so the branch is unexercised on
   silicon and its 250 ms synthetic delay stands in for a latency never
   observed. The rate ladder at 1000 samples per second is the closest available
   falsifier.

## Gated on the acceptance above passing

9. Advance `_commit` to the accepted merge, recompute `pkgver` as
   `1.4.rN.g<abbrev-object-id>`, and reset `pkgrel` to 1. The pinned commit is
   reachable from `master` before it enters the recipe.
10. Prove the recipe's literal `pkgver` equals what `pkgver()` derives, with
    `makepkg --nobuild && git diff --exit-code PKGBUILD`.
11. A clean `makepkg` from a directory outside the checkout, the packaged
    binary's `--version` equal to `pkgver`, and a second clean build producing a
    byte-identical artifact at the pinned `SOURCE_DATE_EPOCH`.

## Ready to author, merge gated on a family run

The source change is writable now; the pass claim waits on item 1's run,
because these touch the read path.

12. An `mmio_read32()` helper taking a `const volatile uint32_t *`, so the
    source requires a fresh device read rather than relying on the indirect-call
    structure to force one.
13. The four MMIO loads in `detect.c` -- `getgrbm_pci`, `getgrbm_pci_r300`,
    `getsrbm_pci`, and `getsrbm2_pci` -- routed through that helper.
14. `_Static_assert` proofs that each register offset plus its access width
    lands inside the mapping it reads: `RBBM_STATUS`, `SRBM_STATUS`, and
    `SRBM_STATUS2` within `SRBM_MMAP_SIZE`, and the `0x10` offset within
    `MMAP_SIZE`.
15. Correct the file comment in `collector_backend.c` stating that the MMIO
    readers cannot fail. The load always yields a word and reports no error, and
    a removed device answers all-ones, so the classification rests on the return
    convention rather than on device presence.
16. Dithered or randomized deadlines, so a workload periodic at a harmonic of
    the sampling rate is not sampled at a fixed phase. The dither preserves
    `attempted + missed == nominal`, which is what makes a dithered window still
    comparable with an undithered one.

## Ready without silicon

17. Branch protection on `master`, which the GitHub API reports absent, so a
    merge can bypass the gates the workflows run. The required checks are the
    six compile-matrix jobs `minimal / gcc`, `minimal / clang`, `radeon / gcc`,
    `radeon / clang`, `full / gcc`, `full / clang`, plus `static analysis`,
    `collector integrity`, `command-line contracts`, and `arch package`.
18. The administrator-bypass posture, which is a policy choice rather than a
    defect: enforcing the checks for administrators closes the bypass and also
    removes the escape hatch for a broken required check.
19. `makerepropkg` rebuilds a package against the `.BUILDINFO` package set from
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

20. The `libdrm_amdgpu` and radeon-ioctl read paths stay unexercised on silicon,
    because no amdgpu or R600 part is reachable from either available host.
    Their `-errno` handling is reasoned from API convention only, which places
    it at rank 5 until a part answers.
