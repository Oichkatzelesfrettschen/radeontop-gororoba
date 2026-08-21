# Window dispersion and the effective sample size of a duty estimate

A lane percentage is the mean of a binary level signal over the instants a
sampler reached. `docs/rs4xx-engine-busy-read-path.md` establishes that estimate
and states the reference error `sqrt(p(1-p)/N)` that would hold if the samples
were independent, then names what the retained evidence could not settle:
separating the true spread from that reference "needs an autocorrelation estimate
or repeated independent windows, neither of which is retained." Captures now
retain 45 and 60 windows, and each window publishes its own busy count and valid
read count per lane, so the second of those two routes is open. This document
derives the estimator that walks it, decomposes what sets its value, and reports
what it reads on retained RS482 captures.

The result the estimator delivers is a per-run number that says how much
information the sampler actually collected. On the retained vertex-build load the
exact 120 Hz grid reaches the precision an independent sampler would need about
4.4 times as many samples to reach. On a load periodic at about one sample slot
the same grid loses by a comparable factor in the other direction. One statistic
covers both, computed from counts the capture already carries.

## The statistic

Index the windows of one capture by `i`. Window `i` publishes `B_i` busy
predicates over `V_i` valid reads of the lane's source register. Pool them:

```text
N = sum(V_i)          K = sum(B_i)          p = K / N
```

Under the null that every sample is an independent draw at one rate `p`, each
`B_i` is binomial with mean `V_i * p` and variance `V_i * p * (1 - p)`, so

```text
X2 = sum( (B_i - V_i*p)^2 / (V_i * p * (1-p)) )
D  = X2 / (W - 1)
```

is Pearson's dispersion over `W` windows with `W - 1` degrees of freedom. `D`
reads near one when the null holds. It reads above one when the windows scatter
more than their own denominators allow, and below one when they scatter less.

When every window validates the same number of reads, `D` is exactly the square
of the scatter ratio the RS482 target reports carry. Both forms were computed
independently over `dither-seed-grid.dump`: the sample standard deviation of the
per-window duty is 0.03624 against a binomial 0.03083, a ratio of 1.1756, whose
square is 1.3821, which is the Pearson `D` to five figures. The measured ratios
in `docs/open-work.md` therefore map onto this statistic directly, 3.31 to
`D = 10.96`, 1.26 to `D = 1.59`, 0.25 to `D = 0.0625`, and 0.47 to `D = 0.221`.
The general form divides each window by its own denominator, which the exactly
equal case does not need but a capture that misses slots does.

### Reading the interval, not the point

`X2` is approximately chi-square on `W - 1` degrees of freedom, so

```text
D_low  = X2 / chi2(1 - alpha/2, W-1)
D_high = X2 / chi2(alpha/2, W-1)
```

bounds `D` at coverage `1 - alpha`. The width of that interval is what decides
whether a run can answer the question at all. At `W = 5`, the length of every
capture in the admitted MMIO bundle, a lane reading exactly `D = 1.0` carries the
interval `[0.36, 8.27]`. At `W = 45` the same reading carries `[0.69, 1.60]`. A
five-window capture resolves nothing about its own sampler, and the estimator
says so through the interval rather than by refusing an answer.

### The interval widens and never narrows

The effective sample size follows from the inflation the upper limit admits:

```text
N_eff = N / max(1, D_high)
```

and the duty interval is `p +/- z * sqrt(p*(1-p)/N_eff)`. Taking the upper limit
rather than the point estimate keeps a run from claiming more precision than its
own degrees of freedom support. Clamping at one keeps an underdispersed reading
from narrowing anything: a grid that samples a periodic load evenly does collect
more information than an independent sampler would, and saying so as a narrower
confidence interval would rest a precision claim on the load staying periodic for
the whole run. The dispersion figure reports the gain; the interval does not
spend it.

### What excess scatter does and does not identify

`D` above one means the windows disagree more than one rate explains. A sampler
whose grid meets a preferred phase of a periodic load produces that. So does a
load whose own duty changes from window to window, and so does read loss that
correlates with the load. A single capture bounds the sampler's contribution and
does not isolate it, and the discriminator is a paired exact and dithered pass
over one load session, interleaved so a drifting load reaches both grids in the
same proportion.

`D` below one carries a stronger conclusion, because the alternatives run one
way. A nonstationary load adds variance across windows and correlated read loss
adds variance across windows; neither removes it. Under-dispersion requires the
within-window sample sequence to be negatively correlated or the phase coverage
to be more even than random, and on a fixed grid against a periodic load those
are the same statement. An underdispersed reading therefore attributes itself to
the grid's phase geometry without a dithered control to exclude the load.

### Where the approximation fails

Pearson's statistic rests on a normal approximation to each window's binomial
count, which degrades once the expected count in either cell falls below about
five. A lane at duty 0.997 over 120 slots expects 0.4 idle samples per window,
and its dispersion is then driven by where a handful of rare zeros fall. Every
reading carries the smallest expected cell it saw and a `coarse` qualifier below
that threshold, so a consumer separates a verdict the approximation supports from
one it does not. A lane pinned at zero or at saturation produces no variance at
all and reads `degenerate`.

## What sets the value

Model the load as a level signal of period `T_L` and duty `d`, and the sampler as
a grid of period `T_s`. Sample `k` lands at phase

```text
phi_k = frac(k * T_s / T_L + phi_0) = frac(k * alpha + phi_0)
```

so the phase sequence is a rotation by `alpha = frac(T_s / T_L)`. A window of `M`
samples covers the arc `a = M * alpha` of the load's cycle, modulo one. Three
regimes follow, and the arc `a` is the coefficient that separates them.

### Oversampled, a much less than one is false and a much greater than one holds

When the window spans many cycles the rotation equidistributes. The count of
samples landing inside the on-arc deviates from `M * d` by a discrepancy term
`E` that an `alpha` with bounded partial quotients holds to order `log M` rather
than the order `sqrt(M)` an independent sampler produces, so the per-window duty
deviates by order `log(M)/M` against order `1/sqrt(M)`. Substituting into `D`:

```text
D -> mean(E^2) / (M * d * (1-d))
```

which falls as the window lengthens. This is systematic sampling of a periodic
population, and it is the regime where a fixed grid beats a random one.

### Near resonance, a comparable to one or smaller

When `T_s` is close to `T_L` or to a multiple of it, `alpha` is small and the
window sweeps only a fraction of a cycle. The window mean is then the average of
the level signal over one contiguous arc rather than over the whole cycle, and
its variance across windows is the variance of that arc mean over the starting
phase, which is order `d(1-d)` rather than order `d(1-d)/M`. Substituting:

```text
D -> M * Var_phi(arc mean) / (d * (1-d))
```

which approaches `M` as the arc narrows, because a window that sees one phase
delivers one independent measurement no matter how many samples it takes. The
dynamic range between the two regimes is therefore of order `M^2`, and at
`M = 120` the measured extremes, `D = 0.22` and `D = 11.0`, span a factor of 50
inside it.

### Randomized phase

A dither drawn inside each slot decorrelates `phi_k` from `k`, which restores the
independent null and drives `D` toward one from either side. It does not reach
one exactly: the offset stays inside its own slot rather than spanning the load's
cycle, and scheduler lateness selects against late-slot offsets, so a residual
phase structure survives. The measured dithered readings cluster just above one,
which is the expected direction.

The rigid-rotation model above assumes a load whose period does not move. The
retained loads carry cycle-to-cycle jitter of a few percent, which decorrelates
the phase sequence over tens of samples and pulls every regime toward one. The
model therefore sets the direction and the order of the coefficient rather than
its exact value, and the measurements below carry the quantities.

## What it reads on retained RS482 captures

The readings come from the `steinmarder-r300` bundle
`src/re/r300/results/rs482-radeontop-lane-load-discrimination`, captured on
RS482 PCI `1002:5974` at 120 samples per second with one-second windows, by
`radeontop-gororoba 1.4.r110.gf105aa8dd60b` under kernel 7.1.3-2-cachyos with
`radeon-unified/0.7`. That binary predates the dither-by-default change, so a
capture taken without `--dither-seed` runs the exact grid. Rows whose smallest
expected cell falls below five are excluded; they carry no verdict the
approximation supports.

| Capture | Lane | Windows | Duty | D | 95 percent interval | Verdict |
|---|---|---|---|---|---|---|
| `paired-load-vertex-build` | gpu | 60 | 0.0914 | 0.228 | 0.164 to 0.339 | underdispersed |
| `paired-load-vertex-build` | pa | 60 | 0.0914 | 0.228 | 0.164 to 0.339 | underdispersed |
| `paired-load-vertex-build` | cf | 60 | 0.0914 | 0.228 | 0.164 to 0.339 | underdispersed |
| `paired-load-vertex-build` | ee | 60 | 0.0911 | 0.235 | 0.169 to 0.349 | underdispersed |
| `paired-load-vertex-build` | cp | 60 | 0.0471 | 0.358 | 0.257 to 0.532 | underdispersed |
| `twod-solid-fill` | vgt | 45 | 0.8776 | 0.969 | 0.664 to 1.546 | binomial |
| `twod-solid-fill` | ee | 45 | 0.1730 | 1.438 | 0.986 to 2.295 | binomial |
| `dither-seed-grid` | vgt | 45 | 0.1313 | 1.382 | 0.947 to 2.205 | binomial |
| `dither-seed-7919` | vgt | 45 | 0.1365 | 1.252 | 0.858 to 1.998 | binomial |
| `dither-seed-65537` | vgt | 45 | 0.1348 | 1.339 | 0.917 to 2.136 | binomial |
| `dither-seed-1` | vgt | 45 | 0.1300 | 2.005 | 1.374 to 3.200 | overdispersed |
| `vapphase-continuous-600s` | vgt | 60 | 0.1351 | 1.791 | 1.287 to 2.665 | overdispersed |
| `vapphase-restarting-10s` | vgt | 60 | 0.1281 | 1.902 | 1.367 to 2.830 | overdispersed |

### The grid beats the independent null by a factor of four on a periodic load

The vertex-build capture is the sub-binomial regime on silicon. Its 60 windows
each validate all 120 reads, and the per-window busy counts run from 8 to 15 with
a standard deviation of 1.49 against a binomial 3.16. The exact grid places the
same phase set inside every window, so every window catches nearly the same
number of busy phases, and the duty estimate carries the precision an independent
sampler would need about 4.4 times as many samples to reach. Four lanes read the
identical `D = 0.228` because `GUI_ACTIVE`, `GA_BUSY`, and `CF_PIPE_BUSY` assert
together on this part, which the lane-load matrix already establishes; `ENG_EV`
and `CP_CMDSTRM` carry their own counts and their own values.

That figure is the price dithering pays on this load. It is a variance price, not
a bias price, and the output reports it, which is why it does not overturn the
shipped default. The failure dithering prevents is a duty figure that is simply
wrong, bounded by nothing; the cost it charges is a wider spread that the run
publishes. Naming the cost per run is what the estimator adds: a consumer now
reads how much the default spent on the load actually in front of it rather than
inheriting a decision taken on a different one.

### A real-silicon binomial control

`twod-solid-fill` reads `D = 0.969` at `p = 0.94` on the vgt lane at duty 0.878,
with expected cells of 105 and 15 per window. The estimator returns the null on
silicon under a load with no commensurate period, so an overdispersed or
underdispersed reading elsewhere is not an artifact of the statistic.

### A deviation the run does not resolve

Three dither seeds and one exact grid ran the same texture-fill load in sequence.
The exact grid and two seeds read binomial; seed 1 reads `D = 2.005` with the
interval clear of one at `p = 0.0002`, which survives a correction for the four
comparisons. Dithering is supposed to remove phase structure, so a dithered pass
scattering more than the exact grid is a deviation from the prediction, and the
deviation is the finding.

Coverage does not explain it. The exact grid retains all 120 slots in every
window; the three seeds retain a mean of 116.6, 117.0, and 116.7 with per-window
standard deviations of 1.99, 1.73, and 1.65, so seed 1 differs from the others in
coverage by far less than its dispersion differs. The passes ran one after
another against a restarted workload, so a load drift between passes and a
seed-specific phase selection remain unseparated. The discriminator is
interleaved slices with the leading grid alternating between pairs, which is the
design the bracketed-run comparison in `docs/open-work.md` already uses.

### The admitted MMIO bundle cannot answer the question

The bundle at
`src/re/r300/results/cachyos_vostro1000_rs482_rbbm_status_mmio_capture_20260809T185637Z`
runs `-l 3` and `-l 5`, so its captures carry three and five windows. Its
exact-grid load passes read vgt at `D = 2.97` and `D = 1.44`, whose intervals are
`[1.07, 24.5]` and `[0.52, 11.9]`. Two passes of the same configuration against
the same load land on opposite sides of the threshold, which is what four degrees
of freedom deliver. That bundle remains decision-grade for what it was built to
prove, the setuid-origin containment and the same-boot control-candidate
comparison; it carries no dispersion verdict.

The run-length rule follows from the interval width rather than from a
convention. A point estimate of `D = 2.0` first clears one at twelve windows, at
a lower limit of 1.004 that a single window's swing erases. Thirty windows put
that limit at 1.269 and 60 windows at 1.437, so a capture that intends to report
its own dispersion takes at least 30 windows, and 45 to 60 where the contrast
between two grids is the question.

## Reproduction

```sh
tools/capture-window-dispersion.py --self-test
tools/capture-window-dispersion.py CAPTURE...
tools/capture-window-dispersion.py --json CAPTURE... > dispersion.json
```

The analyzer reads `evidence_v1` and `evidence_v2` records, which both publish
the per-window per-lane `busy` and `valid` counts, so it runs against captures
taken before the record schema advanced. It parses every evidence object in a
file and fails on any object that does not parse, because a partial census over
windows a capture claims to carry supports no verdict. `make check` runs the
self-test.

The self-test calibrates against known-good and known-bad inputs before the tool
reports on evidence: independent windows must read binomial with the interval
covering one, alternating-rate windows must read overdispersed and must lower the
effective sample count, identical windows must read underdispersed and must leave
the effective count at the raw count, a saturated lane and a single window must
read degenerate, a truncated evidence object and a lane missing its valid count
must be refused rather than skipped, a generation gap must be reported, the
chi-square quantiles must match published values at four points, and a lane one
sample short of saturation must carry the coarse qualifier.

## Falsifiers

The estimator makes claims that a run can break.

- A dithered pass and an exact pass, interleaved over one load session with a
  period near one sample slot, that read the same `D` within their intervals
  refutes the claim that the grid's phase geometry drives the contrast. The
  prediction is `D` near 11 for the exact grid and near 1.6 for the dithered one,
  which are the retained scatter ratios squared.
- An interleaved repetition of the four-grid texture-fill comparison in which
  seed 1 again reads overdispersed while the exact grid and the other seeds do
  not establishes a seed-specific phase selection; a repetition in which the
  overdispersion follows pass order rather than seed attributes it to load drift.
- A capture whose windows are independent by construction, which a synthetic
  backend under the virtual clock supplies, that reads `D` away from one at a
  rate above the nominal 5 percent refutes the statistic's calibration rather
  than the sampler.
- A load whose period is measured independently, sampled at a rate that places a
  known arc `a` inside each window, that reads a `D` outside the regime the arc
  predicts refutes the model that sets the coefficient. This needs a client whose
  period is derived from hardware, because a `nanosleep`-timed client delivers
  cycle-to-cycle deviation coarser than the model's tolerance by a factor near
  445.

## Where this sits

The estimator consumes retained capture records and adds no sample-path code, so
it changes neither the collector nor the packaged binary. It answers a question
the phase-inclusion work in `docs/open-work.md` also approaches from the other
side: phase bins would record where inside its slot each accepted sample landed,
which describes the sampler's own behavior directly and needs a collector change;
dispersion measures what that behavior cost the estimate, needs no collector
change, and runs against every capture already retained. The two are
complementary, and dispersion is the one that can report on the existing corpus
today.

Retained probes, result bundles, and target-silicon verdicts live in
`steinmarder-r300` under `src/re/r300/`, and this repository carries the
citation. The readings above are computed from that bundle's retained capture
files and are reproducible from them with the command line named here.
