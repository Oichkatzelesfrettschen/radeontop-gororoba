#!/usr/bin/env python3
"""Window-to-window dispersion of a radeontop capture's lane duty figures.

A capture record publishes, per window, the busy count and the valid read count
of every exposed lane.  Those two integers per window are the sufficient
statistics for a dispersion test: under the null that a window's samples are
independent draws at one rate, the busy count is binomial and the scatter of the
per-window duty figures matches what each window's own denominator implies.  A
sampler whose grid aliases a periodic load violates that null, because
neighboring samples land at neighboring phases of the load rather than at
independent ones.

The statistic is the Pearson dispersion, chi-square over its degrees of freedom.
It reads near one when the null holds, above one when the windows scatter more
than their denominators allow, and below one when the grid distributes phases
more evenly than a random sampler would.  The reported effective sample size
divides by the interval's upper limit and never exceeds the raw sample count, so
an underdispersed reading widens no interval it should not.

Excess scatter has more than one cause.  A load whose own duty changes between
windows inflates the statistic exactly as aliasing does, so a single run bounds
the sampler's contribution rather than isolating it; the discriminator is a
paired exact and dithered pass over one load session.

Copyright (C) 2012 Lauri Kasanen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
"""

import argparse
import json
import math
import random
import sys

SCHEMA = "radeontop_window_dispersion_v1"

# The evidence object carries the record's own schema version in its marker, and
# both versions publish the per-window lane counts this analysis reads.
EVIDENCE_MARKERS = ("evidence_v2 {", "evidence_v1 {")

# The two-sided coverage the reported dispersion interval and duty interval use.
ALPHA = 0.05
NORMAL_QUANTILE = 1.959963984540054

# Pearson's statistic rests on a normal approximation to each window's binomial
# count.  Below this expected count in either cell the approximation degrades,
# so the reading carries a note rather than a silent verdict.
MIN_EXPECTED_CELL = 5.0


class CaptureError(Exception):
    """A capture file that cannot be read as a record stream."""


def _lower_gamma_series(shape, x):
    """Regularized lower incomplete gamma by its series, for x < shape + 1."""
    term = 1.0 / shape
    total = term
    index = shape
    for _ in range(1000):
        index += 1.0
        term *= x / index
        total += term
        if abs(term) < abs(total) * 1e-16:
            break
    return total * math.exp(-x + shape * math.log(x) - math.lgamma(shape))


def _upper_gamma_fraction(shape, x):
    """Regularized upper incomplete gamma by its continued fraction."""
    tiny = 1e-300
    b = x + 1.0 - shape
    c = 1.0 / tiny
    d = 1.0 / b if b != 0.0 else 1.0 / tiny
    h = d
    for index in range(1, 1000):
        an = -index * (index - shape)
        b += 2.0
        d = an * d + b
        if abs(d) < tiny:
            d = tiny
        c = b + an / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-16:
            break
    return h * math.exp(-x + shape * math.log(x) - math.lgamma(shape))


def chi_square_cdf(x, degrees):
    """P(X <= x) for a chi-square variate with `degrees` degrees of freedom."""
    if degrees <= 0:
        raise ValueError("degrees of freedom must be positive")
    if x <= 0.0:
        return 0.0
    shape = degrees / 2.0
    half = x / 2.0
    if half < shape + 1.0:
        return _lower_gamma_series(shape, half)
    return 1.0 - _upper_gamma_fraction(shape, half)


def chi_square_quantile(probability, degrees):
    """The x with P(X <= x) == probability, by bisection on the CDF.

    Bisection converges on any degrees of freedom this analysis reaches and
    needs no derivative, which keeps the implementation inside the standard
    library.
    """
    if not 0.0 < probability < 1.0:
        raise ValueError("probability must lie strictly inside the unit interval")
    low = 0.0
    high = max(1.0, float(degrees))
    while chi_square_cdf(high, degrees) < probability:
        high *= 2.0
        if high > 1e12:
            return high
    for _ in range(200):
        middle = 0.5 * (low + high)
        if chi_square_cdf(middle, degrees) < probability:
            low = middle
        else:
            high = middle
    return 0.5 * (low + high)


def lane_dispersion(windows):
    """Dispersion of one lane's per-window busy counts.

    `windows` is a sequence of (busy, valid) integer pairs.  Windows whose lane
    validated no read carry no denominator and take no part.
    """
    counted = [(busy, valid) for busy, valid in windows if valid > 0]
    busy_total = sum(busy for busy, _ in counted)
    valid_total = sum(valid for _, valid in counted)
    result = {
        "windows": len(counted),
        "busy": busy_total,
        "valid": valid_total,
        "duty": (busy_total / valid_total) if valid_total else None,
        "chi_square": None,
        "degrees_of_freedom": max(len(counted) - 1, 0),
        "dispersion": None,
        "dispersion_low": None,
        "dispersion_high": None,
        "p_value": None,
        "effective_samples": valid_total,
        "duty_low": None,
        "duty_high": None,
        "min_expected_cell": None,
        "approximation": None,
        "verdict": "degenerate",
    }

    if len(counted) < 2 or not valid_total:
        return result

    duty = busy_total / valid_total
    if duty <= 0.0 or duty >= 1.0:
        # A lane pinned at zero or at saturation produces no variance to test,
        # and the statistic would divide by zero rather than report a verdict.
        result["duty_low"] = duty
        result["duty_high"] = duty
        return result

    variance_unit = duty * (1.0 - duty)
    chi_square = 0.0
    min_expected = math.inf
    for busy, valid in counted:
        expected = valid * duty
        residual = busy - expected
        chi_square += residual * residual / (valid * variance_unit)
        min_expected = min(min_expected, expected, valid - expected)

    degrees = len(counted) - 1
    dispersion = chi_square / degrees
    lower_quantile = chi_square_quantile(ALPHA / 2.0, degrees)
    upper_quantile = chi_square_quantile(1.0 - ALPHA / 2.0, degrees)
    dispersion_low = chi_square / upper_quantile
    dispersion_high = chi_square / lower_quantile if lower_quantile > 0.0 else math.inf

    cdf = chi_square_cdf(chi_square, degrees)
    p_value = 2.0 * min(cdf, 1.0 - cdf)

    # The interval widens on excess scatter and holds at the raw count otherwise,
    # so a reading below one never asserts precision the sample size denies.
    inflation = max(1.0, dispersion_high)
    effective = valid_total / inflation
    half_width = NORMAL_QUANTILE * math.sqrt(variance_unit / effective)

    if dispersion_low > 1.0:
        verdict = "overdispersed"
    elif dispersion_high < 1.0:
        verdict = "underdispersed"
    else:
        verdict = "binomial"

    result.update({
        "duty": duty,
        "chi_square": chi_square,
        "degrees_of_freedom": degrees,
        "dispersion": dispersion,
        "dispersion_low": dispersion_low,
        "dispersion_high": dispersion_high,
        "p_value": min(1.0, p_value),
        "effective_samples": effective,
        "duty_low": max(0.0, duty - half_width),
        "duty_high": min(1.0, duty + half_width),
        "min_expected_cell": min_expected,
        "approximation": "coarse" if min_expected < MIN_EXPECTED_CELL else "normal",
        "verdict": verdict,
    })
    return result


def parse_evidence_objects(text, origin):
    """Every evidence object in one capture file, in record order.

    A record's evidence object runs from its marker's brace to the end of its
    line, so the trailing brace needs no balance scan.  A malformed object stops
    the read: a capture whose records cannot all be parsed supports no census
    over the windows it claims to carry.
    """
    objects = []
    for number, line in enumerate(text.splitlines(), start=1):
        start = -1
        for marker in EVIDENCE_MARKERS:
            found = line.find(marker)
            if found >= 0:
                start = found + len(marker) - 1
                break
        if start < 0:
            continue
        try:
            objects.append(json.loads(line[start:]))
        except ValueError as error:
            raise CaptureError(
                "%s:%d: evidence object does not parse: %s"
                % (origin, number, error)) from error
    return objects


def analyze_records(records, origin):
    """Per-lane dispersion over the windows one capture published."""
    lanes = {}
    generations = []
    for record in records:
        generation = record.get("generation")
        if isinstance(generation, int):
            generations.append(generation)
        for name, lane in sorted(record.get("lanes", {}).items()):
            busy = lane.get("busy")
            valid = lane.get("valid")
            if not isinstance(busy, int) or not isinstance(valid, int):
                raise CaptureError(
                    "%s: lane %s publishes no integer busy and valid counts"
                    % (origin, name))
            lanes.setdefault(name, []).append((busy, valid))

    contiguous = bool(generations) and generations == list(
        range(generations[0], generations[0] + len(generations)))

    return {
        "source": origin,
        "records": len(records),
        "generations_contiguous": contiguous,
        "lanes": {name: lane_dispersion(windows)
                  for name, windows in sorted(lanes.items())},
    }


def analyze_file(path):
    if path == "-":
        return analyze_records(parse_evidence_objects(sys.stdin.read(), "-"), "-")
    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        text = stream.read()
    records = parse_evidence_objects(text, path)
    if not records:
        raise CaptureError("%s: carries no evidence object" % path)
    return analyze_records(records, path)


def format_report(analysis):
    """One fixed-width table per capture, lanes in name order."""
    lines = [
        "%s  records=%d contiguous=%s"
        % (analysis["source"], analysis["records"],
           "yes" if analysis["generations_contiguous"] else "no"),
        "%-6s %7s %9s %9s %9s %9s %9s %8s  %s"
        % ("lane", "windows", "duty", "disp", "disp_lo", "disp_hi",
           "n_eff", "p", "verdict"),
    ]
    for name, lane in analysis["lanes"].items():
        def number(value, digits=4):
            return "-" if value is None else "%.*f" % (digits, value)

        lines.append(
            "%-6s %7d %9s %9s %9s %9s %9s %8s  %s"
            % (name, lane["windows"], number(lane["duty"]),
               number(lane["dispersion"], 3), number(lane["dispersion_low"], 3),
               number(lane["dispersion_high"], 3),
               number(lane["effective_samples"], 1), number(lane["p_value"]),
               lane["verdict"]))
        if lane["approximation"] == "coarse":
            lines.append(
                "%-6s expected cell %.2f below %.0f, so the chi-square "
                "approximation is coarse here"
                % ("", lane["min_expected_cell"], MIN_EXPECTED_CELL))
    return "\n".join(lines)


def _binomial_windows(rng, count, size, duty):
    return [(sum(1 for _ in range(size) if rng.random() < duty), size)
            for _ in range(count)]


def _aliased_windows(rng, count, size, low, high):
    """Windows drawn at alternating rates, the signature a grid meeting one
    phase of a load it samples about once per cycle leaves behind."""
    windows = []
    for index in range(count):
        duty = low if index % 2 == 0 else high
        windows.append((sum(1 for _ in range(size) if rng.random() < duty), size))
    return windows


def _stratified_windows(count, size, busy):
    """Every window carrying the same busy count, which is the limit a grid
    approaches when it distributes phases perfectly evenly."""
    return [(busy, size) for _ in range(count)]


def _self_test():
    """Calibration against known-good and known-bad inputs.

    Every case names the property it proves and the reading that would refute
    it, so a change to the statistic that breaks one of them fails here rather
    than in a report.
    """
    failures = []

    def check(name, condition, detail):
        if not condition:
            failures.append("%s: %s" % (name, detail))

    rng = random.Random(20260820)

    independent = lane_dispersion(_binomial_windows(rng, 40, 120, 0.4))
    check("independent-binomial", independent["verdict"] == "binomial",
          "independent windows read %s at dispersion %.3f"
          % (independent["verdict"], independent["dispersion"]))
    check("independent-interval",
          independent["dispersion_low"] <= 1.0 <= independent["dispersion_high"],
          "the dispersion interval [%.3f, %.3f] excludes one"
          % (independent["dispersion_low"], independent["dispersion_high"]))

    aliased = lane_dispersion(_aliased_windows(rng, 40, 120, 0.10, 0.70))
    check("aliased-overdispersed", aliased["verdict"] == "overdispersed",
          "alternating-rate windows read %s at dispersion %.3f"
          % (aliased["verdict"], aliased["dispersion"]))
    check("aliased-widens", aliased["effective_samples"] < aliased["valid"],
          "the effective sample count %.1f did not fall below the raw %d"
          % (aliased["effective_samples"], aliased["valid"]))

    stratified = lane_dispersion(_stratified_windows(40, 120, 48))
    check("stratified-underdispersed", stratified["verdict"] == "underdispersed",
          "identical windows read %s at dispersion %.3f"
          % (stratified["verdict"], stratified["dispersion"]))
    check("stratified-never-narrows",
          stratified["effective_samples"] == stratified["valid"],
          "an underdispersed reading raised the effective count to %.1f above "
          "the raw %d" % (stratified["effective_samples"], stratified["valid"]))

    check("independent-approximation-normal",
          independent["approximation"] == "normal",
          "windows with expected cells of 48 and 72 read the approximation as %s"
          % independent["approximation"])

    # A lane one sample short of saturation leaves an expected minority cell
    # near one, where Pearson's normal approximation to the binomial fails and
    # the reading needs the qualifier rather than a bare verdict.
    near_saturation = lane_dispersion([(119, 120)] * 6 + [(120, 120)] * 6)
    check("near-saturation-approximation-coarse",
          near_saturation["approximation"] == "coarse",
          "an expected minority cell of %.2f read the approximation as %s"
          % (near_saturation["min_expected_cell"], near_saturation["approximation"]))

    saturated = lane_dispersion([(120, 120)] * 8)
    check("saturated-degenerate", saturated["verdict"] == "degenerate",
          "a lane pinned at saturation read %s" % saturated["verdict"])
    single = lane_dispersion([(48, 120)])
    check("single-window-degenerate", single["verdict"] == "degenerate",
          "one window read %s" % single["verdict"])
    empty = lane_dispersion([(0, 0), (0, 0)])
    check("no-denominator-degenerate", empty["verdict"] == "degenerate",
          "windows validating no read read %s" % empty["verdict"])

    # The chi-square tail is the interval's authority, so it is calibrated
    # against published quantiles rather than trusted.
    for degrees, probability, expected in ((1, 0.95, 3.841459), (4, 0.975, 11.14329),
                                           (10, 0.025, 3.246973), (30, 0.5, 29.33603)):
        computed = chi_square_quantile(probability, degrees)
        check("chi-square-quantile-%d-%s" % (degrees, probability),
              abs(computed - expected) < 1e-3,
              "quantile read %.6f against the published %.6f"
              % (computed, expected))

    good = ('1.0: gpu 40.00%, evidence_v1 {"generation":1,"lanes":'
            '{"gpu":{"busy":48,"valid":120}}}')
    parsed = parse_evidence_objects(good, "known-good")
    check("parser-known-good", len(parsed) == 1,
          "a well-formed record parsed to %d objects" % len(parsed))

    bad = '1.0: gpu 40.00%, evidence_v1 {"generation":1,"lanes":{'
    try:
        parse_evidence_objects(bad, "known-bad")
        check("parser-known-bad", False, "a truncated evidence object parsed")
    except CaptureError:
        pass

    missing = [{"generation": 1, "lanes": {"gpu": {"busy": 48}}}]
    try:
        analyze_records(missing, "known-bad")
        check("lane-counts-known-bad", False,
              "a lane without a valid count produced an analysis")
    except CaptureError:
        pass

    gapped = analyze_records(
        [{"generation": 1, "lanes": {}}, {"generation": 3, "lanes": {}}],
        "known-bad")
    check("generation-gap", not gapped["generations_contiguous"],
          "a generation gap reported as contiguous")

    for failure in failures:
        sys.stderr.write("calibration failed: %s\n" % failure)
    if failures:
        return 1
    sys.stdout.write("window dispersion: independent, aliased, and stratified "
                     "windows separate; malformed records rejected\n")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Window-to-window dispersion of radeontop lane duty figures.")
    parser.add_argument("capture", nargs="*",
                        help="capture files written by radeontop -d, or - for standard input")
    parser.add_argument("--json", action="store_true",
                        help="write one machine-readable object instead of a table")
    parser.add_argument("--self-test", action="store_true",
                        help="calibrate the statistic and the parser, then exit")
    arguments = parser.parse_args(argv[1:])

    if arguments.self_test:
        if arguments.capture:
            parser.error("--self-test takes no capture file")
        return _self_test()

    if not arguments.capture:
        parser.error("name at least one capture file, or - for standard input")

    analyses = []
    for path in arguments.capture:
        try:
            analyses.append(analyze_file(path))
        except (CaptureError, OSError) as error:
            sys.stderr.write("%s\n" % error)
            return 1

    if arguments.json:
        json.dump({"schema": SCHEMA, "alpha": ALPHA, "captures": analyses},
                  sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    else:
        sys.stdout.write("\n\n".join(format_report(a) for a in analyses) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
