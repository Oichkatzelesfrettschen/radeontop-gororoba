# radeontop-gororoba Agent and Developer Reference

## Instruction source

`AGENTS.md` is the root instruction file for `radeontop-gororoba` and owns the
rules. Codex, compatible agents, and human contributors read it directly. Other
root agent files exist only to load it for tools that require a tool-specific
filename.

`CLAUDE.md` loads `@AGENTS.md`, spelled in that exact case because imports
resolve literally on a case-sensitive filesystem, and adds Claude Code operating
notes after the load line. Root agent files are regular tracked files; the body
lives here, and a loader carries tool-specific loading notes only, since copied
doctrine drifts into conflicting instructions.

The doctrine here descends from `mesa-26-gororoba/AGENTS.md` and keeps the parts
that survive the move to a GPL3 C monitoring tool: naming, comment voice,
evidence rank, falsification, and merge discipline. Mesa's Meson lanes, Vulkan
ICD plumbing, conformance-suite machinery, and MIT header policy stay in Mesa.

## Hard rules

These rules are enforceable. Later sections explain mechanism or rationale and
never weaken or contradict them.

### Generating principles

Six principles generate the rules in this file. A case no rule names resolves by
the nearest principle.

1. Durable mechanism identity: every durable artifact -- name, comment, claim,
   citation -- carries mechanism or content identity; chronology, actors, and
   process ride in commits, findings, and registry metadata.
2. Indicative voice: rules, comments, and reports state what an artifact is and
   does, present tense, artifact as subject. A boundary takes its positive dual:
   the restriction (`root privileges only`), the named home (`chronology lives
   in the commit message`), or the mechanism itself (`the radeon read-reg ioctl
   answers R600 and later, so R300-class parts read the BAR directly`). The
   positive form entails the absence a negation would state. A hard-stop safety
   boundary keeps its prohibition, where that is the whole content.
3. Authority by name and rank: a load-bearing claim binds to a named source at
   the highest available evidence rank; provenance detail rides in the commit
   message and the finding.
4. Evidence-class separation: known, hypothesized, and speculative stay marked;
   build, runtime, and silicon stay distinct; prediction precedes observation,
   and deviation is the finding.
5. Single home per fact: each fact keeps one canonical location, and other sites
   point to it; a hard-rule digest line plus one expansion section are the two
   permitted homes.
6. Smallest complete mechanism: a change, comment, or tool carries exactly its
   distinct load-bearing facts -- complete, and free of stubs, decoration, and
   repetition.

### Boundary, paths, and instruction files

- This repository is a fork of upstream `clbr/radeontop`. Build, install,
  package, and source-comment flows stand complete inside it.
- Paths in checked-in work are repository-relative, PATH-resolved tools, or
  explicit user roots; scripts discover the root with
  `repo_root=$(git rev-parse --show-toplevel)`.
- Local absolute paths, private host FQDNs, per-user toolchains, raw IP
  literals, and worktree names are workspace-local facts and live outside the
  tree.
- Instruction files are regular tracked files; each loader holds the
  `@AGENTS.md` reference plus tool-specific notes.

### Root cause and evidence

- A behavior change names the exact chip class, register, bit, read path, and
  code path first.
- PCI IDs and register sources identify silicon; family nicknames serve prose
  only. `include/r300_pci_ids.h` and `include/r600_pci_ids.h` are the checked-in
  identity tables, and `familycheck.sh` gates them.
- A gauge claim rests on a register readback under a named load. A register
  decode from kernel headers is a hypothesis until silicon asserts the bit.
- A hardware claim records the observation, the register or kernel source
  constraint, the hypothesis, the falsifier, and the command that produced the
  reading before the change.
- Build, runtime, and silicon stay separate evidence classes.

### Builds and verdicts

- Warnings and unexpected tool output are defects until explained.
- Touched code builds cleanly under the Makefile's `-Wall -Wextra` and adds no
  warnings.
- The report records what was built, tested, skipped, blocked, or unavailable.
- An unrun test reads `not run` with its reason.
- Build targets and validation checks survive a narrow fix.

### Languages and scripts

- C translation units stay at C11 or newer and keep the Makefile's configured
  standard.
- Shell scripts are POSIX `sh`, matching `familycheck.sh`, `getver.sh`, and
  `getamdgpuids.sh`; a script that requires `bash` declares it.
- Shell changes pass `shellcheck`.

### Git, merge, and submission

- Branches, commit subjects, PR titles, source comments, and finding filenames
  carry durable mechanism names. The branch name, first commit subject, and PR
  title are set before first push.
- The default branch is `master`. Work lands through a branch, a PR, and a merge
  to `master`; the merged branch is then deleted locally and on `origin`.
- Merges preserve all non-refuted content; the default resolution is union plus
  synthesis. `git merge -X theirs`, `git checkout --theirs`, and blanket
  conflict-marker stripping are selection, not synthesis.
- A force-push to `master` carries explicit user sign-off and a commit message
  explaining why.
- `PKGBUILD` pins one merged source commit in `_commit`, and `pkgver` derives
  from it as `1.4.rN.g<abbrev-object-id>`. Advancing the pin changes `pkgver`
  and resets `pkgrel` to 1. Changing the recipe, the dependencies, the flags, or
  the installed artifact against an unchanged pin increments `pkgrel`. A change
  outside the packaged source, such as an instruction or documentation file,
  moves neither and says so in the commit body.
- The pinned commit is reachable from `master` before it enters the recipe. A
  squash or rebase merge rewrites the object ids a pull request head carried,
  and deleting that branch can leave a pre-merge pin unreachable.
- The binary's `VERSION` identifies source. `pkgrel` identifies packaging and
  stays out of the binary version string.

### AI disclosure, authorship, and copyright

- When a commit carries AI-assisted creative work, it uses the `Assisted-by:`
  trailer, or `Generated-by:` when AI generated almost the entire change.
- `Co-authored-by:` names human co-authors only.
- Trailer form, matching this repository's history:
  `Assisted-by: Claude (Opus 4.8)`.
- Historical pre-policy `Co-Authored-By: Claude` trailers stand; a force-push to
  scrub them stays out.
- This repository is GPL3. Upstream radeontop headers, including
  `Copyright (C) YYYY Lauri Kasanen` and the GNU General Public License
  paragraph, stay verbatim through edits, splits, and file moves, with no second
  collective copyright line above them.
- A new file matches the header style of the adjacent file it joins, under the
  existing GPL3 terms. A fabricated personal-name line
  (`Copyright (c) YYYY <git config user.name>`) is LLM template output and gets
  stripped, as does an MIT or `Terascale Functionalists` line imported from a
  Mesa-derived tree.
- Source headers carry copyright and license content only; AI disclosure such as
  `(LLM-assisted)` lives in commit trailers.

### Comments, prose, and safety

- A source comment stands on its own for a radeontop maintainer six months
  later, without this project's task tracker. It cites no fork issue number, PR
  chronology, phase label, task number, author tag, local path, private host, or
  deictic time.
- New or modified source comments, commit messages, and documentation use
  American (United States) English spelling.
- A patch changes behavior or structure with intent: no mass reformat, no stub,
  placeholder, dead code, or `TODO: finish later` prose absent explicit tracked
  rationale.
- Reports state results and decisions directly: the outcome, the decision, the
  evidence, and the residual uncertainty.
- A defect in privileged register or BAR access stops normal feature work;
  contain and report it, then resume.

## Project scope and priorities

`radeontop-gororoba` is a fork of `clbr/radeontop` that adds the R300-class
(RS400/RS480/RS482/RS485) read path. On that pre-R600 IGP the radeon DRM
read-reg ioctl rejects every register, so the fork reads `RBBM_STATUS` (`0x0E40`)
through the BAR2 PCI sysfs `resourceN` node; `radeontop -m` forces that path on
any card. R600 and later keep the upstream `GRBM_STATUS` path unchanged.

Priority order is fixed: correctness of the reported number, upstream
compatibility, stability, performance. Safety applies throughout. Earlier
priorities override later priorities.

A gauge that renders a number the silicon does not produce is worse than an
absent gauge. A lane a family lacks is display-gated on its mask.

Investigate before editing. Read source, register documentation, kernel paths,
commit history, and retained evidence. Work in this order: scope the task,
identify the component, split claims, collect primary evidence, model the
mechanism, design the change, implement, verify, and record the result.

## Evidence rank

When sources conflict, higher rank controls.

1. Silicon evidence: register readback on the target part under a named load,
   `radeontool regmatch` histograms, `dmesg`, live `radeontop` output.
2. Register and hardware documentation: R300/RS480 register specifications, AMD
   documentation, `RBBM_STATUS` and `GRBM_STATUS` bit definitions.
3. Kernel source: radeon DRM, `r300d.h`, `radeon_drm.h`, kernel commit log.
4. radeontop source: `radeon.c`, `amdgpu.c`, `detect.c`, `ui.c`, `dump.c`.
5. Documentation and comments, only when consistent with ranks 1 through 4.

Implementation-affecting claims require a rank 1 through 4 source by name.
Claims without that backing are hypotheses. When a comment conflicts with a
higher-ranked source, cite the higher-ranked source and remove or annotate the
comment.

A register definition establishes a field's position and architectural name, and
a rank-1 observation and a rank-2 or rank-3 decode answer different questions. A
target non-observation challenges the exposure hypothesis and constrains
observability under the sampled conditions; falsifying the decode itself takes
contradictory field evidence, such as the bit carrying a different block's
activity. Rank governs the gauge-exposure decision, and the decode keeps the
field it names.

The RS480 lane audit is the calibration example: a live `RBBM_STATUS` histogram
under a sustained textured fill overrode the gauge-exposure decision derived
from the `r300d.h` decode, and the rasterizer, texture, and render-backend lanes
lost their gauges because the sampled RS482 conditions showed those bits clear
throughout. `r300d.h` still names those fields.

### Falsification record

Before a gauge or read-path change, record the direct observation, the register
or kernel source constraint, the hypothesis, the falsification criterion, and
the command that produces the reading.

Prediction form: if bit N of `RBBM_STATUS` is exposed on this part, it asserts
under `[named load]` and stays clear at idle. A bit that stays clear under load
refutes the exposure hypothesis for the sampled rate, load, and part, and the
lane is display-gated on that result. The decode keeps its field position and
architectural name, which the register document establishes.

A non-observation bounds the sampling that produced it. Record the rate, the
window, and the load, because a higher-rate sampler can assert a bit a slower
one misses.

When a reading deviates from prediction, the deviation is the finding. Open a
new investigation instead of changing the prediction after observation.

Stop implementation and report when a hypothesis survives three independent
falsification attempts, fails in an unexpected way, requires a non-obvious
architecture choice, or contradicts a rank-1 or rank-2 source.

## Chip identity and register names

Use identity strings searchable by codename, product, family enum, and register.

- R300-class integrated: `RS400`, `RS480`, `RS482`, `RS485`; pre-R600; engine
  busy in `RBBM_STATUS` (`0x0E40`) read through the BAR2 PCI sysfs `resourceN`
  node.
- R600 and later: engine busy in `GRBM_STATUS` through the radeon DRM read-reg
  ioctl, or through the same BAR path under `radeontop -m`.
- amdgpu parts: usage through `libdrm_amdgpu`, gated by the Makefile `amdgpu`
  option.

Register citations name the register and bit: `RBBM_STATUS.CF_PIPE_BUSY`
(bit 14), `RBBM_STATUS.GUI_ACTIVE` (bit 31), `RBBM_STATUS.CP_CMDSTRM_BUSY`
(bit 16). Bit numbers travel with the register name, and the kernel header that
decodes them is cited by symbol (`r300d.h`), not by line number.

Hardware citations name public documents and sections. Internal extracts,
retained bundle paths, and audit artifacts are evidence, not citation authority.

## Build, package, and validation

Build from the repository with GNU make:

```sh
make
make install PREFIX=/usr DESTDIR=./staging
```

Makefile option knobs: `nls` (translations, default on), `xcb` (unprivileged
Xorg access, default on), `amdgpu` (auto by `libdrm_amdgpu` presence), `debug`,
`nostrip`, and `plain`. The build carries `-Wall -Wextra`; touched code adds no
warnings.

Package with `makepkg -si` from the repository root. `PKGBUILD` fetches the
commit its `_commit` pin names, so the package builds one immutable tree rather
than the working directory, and `make VERSION="$pkgver"` stamps that revision
into the binary. `SOURCE_DATE_EPOCH` set to the pinned commit's committer date
makes two clean builds byte-identical; without it makepkg stamps the current
time into `builddate` and every archive member mtime.

`.github/workflows/build.yml` runs the compiler and option matrix, the
analyzers, and the command-line contracts.
`.github/workflows/package.yml` proves the source pin, the version identity in
both recipe and binary, the namcap verdict, and the byte-identical rebuild.

Minimum validation by changed surface:

- Register or gauge mapping: build plus a readback on the target part under a
  named load and at idle.
- Read-path selection in `detect.c` or `radeon.c`: build plus a run on the
  affected family, and confirmation that the other family's path is unchanged.
- PCI ID tables: `./familycheck.sh`.
- Makefile or `PKGBUILD`: a clean `makepkg` run from a directory outside the
  checkout, the packaged binary's `--version` matching `pkgver`, and a second
  clean build producing an identical artifact.
- Shell scripts: `shellcheck` plus a known-good and a known-bad path.
- Comments and documentation: comment hygiene and a source-reference audit.

A pass claim rests on a run; an unrun test reads `not run` with its reason.

## Durable names

Use names from mechanism or content, not chronology, actors, work sessions, or
review process. This applies to branch names, finding filenames, PR titles,
commit subjects, source comments, and checked-in identifiers.

Forbidden load-bearing identity includes waves, phases, missions, agents,
worktrees, sessions, reviewers, PR numbers, task numbers, and dates that do not
describe content.

Examples:

- Branch: avoid `rs480-phase2-fix`; use `rs480-add-cf-pipe-gauge`.
- Commit subject: avoid `Wave 3 follow-ups`; use
  `RS480: add a cf gauge for CF_PIPE_BUSY, the unmapped R300 engine lane`.
- Comment: avoid `Phase 4 path`; use `RS480 BAR2 read path`.

The first commit subject matters because a squash merge may reuse it even when
the PR title was corrected later. Set branch name, first commit subject, and PR
title before first push.

Phase, wave, and chronology terms may appear only as secondary registry
metadata, such as a `phase:` field in finding YAML.

A name describes what is inside: content, target, or mechanism, not the act of
collecting, grouping, staging, or sequencing. `tranche` is forbidden in
branches, filenames, identifiers, and comments. `set`, `batch`, and `group` are
forbidden as ordinal containers such as `set5` or `batch_2`, and allowed in
descriptive domain compounds such as `batch_size`.

## Comments, commits, and Markdown

The code is the primary text. Comments explain mechanisms that are not obvious
from the next line of code. A useful comment records a silicon constraint, a
register rule, a kernel ioctl rule, a measured quirk, an ABI boundary, or the
reason a workaround preserves a correct reading.

Follow local style first. radeontop C uses tab indentation, K&R braces, `//`
line comments and `/* */` blocks, and the GPL3 file header. Mass reformatting
enters only as an explicitly requested formatting migration.

### Stating mechanism as fact

State what a thing is and does, in positive declarative form, third-person
present tense: `the kernel rejects RADEON_INFO_READ_REG on pre-R600 parts, so
the BAR2 path carries the busy word`. The comment carries the mechanism and the
constraint that makes it hold; correctness follows from the mechanism, so the
reviewer assumes it and contrast framing falls away. A boundary takes its
positive dual: the restriction, the named home, or the mechanism itself.

Use one thought per comment; stack separate comments when steps are distinct.
Default to a one-line trailing comment on the load-bearing line over a function
header paragraph, and reserve a multi-line block for a genuine silicon quirk.
Mechanical code reads bare.

The language form follows the evidence class. Documented behavior takes the
plain indicative. Reproduced-but-undocumented behavior names the part and the
path where it was observed, so a reader knows the claim rests on a run rather
than a register document. Conjecture carries a marker (`appears to`, `seems
to`) or leaves.

Placement follows scope. Architecture that persists across a file lives at file
scope; the point of use carries the local link in the chain -- skip condition,
bound state, offset rule; a branch comment states what distinguishes the
branch, at the branch.

A full mechanism comment orders its facts: the load-bearing claim, the named
authority (register, bit, kernel symbol, ioctl), the consequence with an inline
code fragment when clearer than prose, and the guard or scope. Most comments
carry one or two of those elements.

Source comments cite public, durable authority. Fork issue numbers, PR numbers,
phase labels, worktree names, agent names, author tags, local absolute paths,
private hosts, and deictic time (`currently`, `previously`, `our GPU`) live in
the commit message or the PR description.

Preferred shape:

```text
The radeon read-reg ioctl answers R600 and later. R300-class parts read
RBBM_STATUS through the BAR2 resourceN node instead.
```

Bad shape:

```text
Phase 8 workaround from the agent branch.
```

### TODO comments

A deferred-work comment opens with `TODO:`, `FIXME:`, `XXX:`, or `HACK:`, and a
new marker comes from that four-item set. It names three mechanism elements:

- missing work: the function, register, bit, kernel symbol, or specification
  section that needs the change;
- deferral reason: the silicon, ABI, or evidence constraint blocking completion;
- tracking artifact: a durable function name, register name, upstream issue URL,
  or silicon-constraint name. When no external issue exists, the named function
  or register is the tracking artifact.

Reviewer breadcrumbs, PR-thread references, phase labels, `AGENTS.md` rule
numbers, and deictic references live in the commit message or PR description.

### Commit and PR prose

Commit subjects carry a component prefix and a concise mechanism, matching this
repository's history: `RS480: add a cf gauge for CF_PIPE_BUSY, the unmapped R300
engine lane`.

Bodies are declarative prose. They open with the load-bearing claim, name the
primary source by authority, state the consequence, and give the evidence: the
register reading, the load, the observed values. `WHY:`, `WHAT:`, and `HOW:`
section headers stay out; they strip the declarative voice and read as a filled
template. Commits that predate this rule stand as historical artifacts.

Build invocations, host names, and validation checklists live in the PR
description. One commit per logical change; each commit stays buildable,
reviewable, and bisectable. Formatting churn and logic changes ride separate
commits.

An appended `(#NNN)` on the subject moves to a trailer that carries the PR
link. A squash-merge button that appends `(#NNN)` to the merged subject is a
forge artifact rather than an author artifact, so it refutes no commit and
justifies no history rewrite; passing an explicit merge subject keeps the
suffix off the subject line.

### Markdown

Markdown loaded by agents uses exactly one H1, heading depth no deeper than
`###`, language tags on code fences, exact cross-references, and rule text as
direct positive-declarative statements. Rule files carry plain ASCII,
present-tense declarative text. Tables appear only when columns carry
independent comparison value.

## Evidence boundary

This repository owns the monitoring tool: read paths, gauge mapping, packaging,
and the source comments that make them intelligible. Retained probes, result
bundles, findings, hazard policy, and the verdict assigned to a target-silicon
run live in the sibling projects, and this repository carries the citation.

Each sibling is an independent checkout with its own `AGENTS.md`; they sit
alongside this one in the workspace. Cite a sibling by repository name plus its
repository-relative path, matching the `radeon-custom` README convention, and
let the workstation supply the equivalent absolute path.

- `steinmarder-r300`: the RS482/RS485 (Dell Vostro 1000, PCI `1002:5974`)
  evidence lane and the primary peer for the R300-class read path here. It owns
  probes, manifests, logs, result bundles, falsifiers, and hazard policy under
  `src/re/r300/`, with `findings/`, `results/`, `probes/`, `corpora/`,
  `oracles/`, `registry/`, and `docs/` beneath it. The path predates the
  `git filter-repo` extraction from the Steinmarder monorepo and is stable, so
  historical finding links and bundle names stay valid.
- `steinmarder-r600-terakan`: the r600 and Terakan evidence lane, under
  `src/re/r600/`.
- `steinmarder`: the shared and cross-architecture reverse-engineering root.
- `mesa-26-gororoba`: the Mesa fork carrying r300g, r600g, and Terakan driver
  code, and the registry of Vostro 1000 kernel modules under
  `docs/hardware/vostro1000-kernel-modules.md`.
- `radeon-custom`: the out-of-tree radeon DRM/DKMS source for the RS480, RS482,
  RS485, and Palm lanes. A gauge reading taken against a patched kernel names
  the DKMS revision that produced it.
- `radeontool-gororoba`: the `radeontool regmatch` source used for the raw
  `RBBM_STATUS` histograms that rank-1 gauge evidence rests on.
- `vostro1000-re`: the RS482 host-platform lane, holding the board, ACPI,
  COMBIOS, and firmware evidence under `systems/dell-vostro-1000/`.

Cite the exact path spelling that exists in the sibling. When the sibling
checkout is absent, the finding filename, the result-bundle name, the commit
SHA, or the public register source carries the citation.

Source comments prefer public register documentation, kernel symbols, DRM
names, and exact hardware identity. Empirical fork evidence belongs in findings
or commit messages unless the code needs the mechanism to be intelligible.

## Tooling for investigation and audits

Use the strongest available tool that matches the claim; a weaker text search
yields when the claim requires a structural, indexed, or empirical tool.

- Source navigation and reachability: `clangd`, `ctags`, GNU Global, `cscope`,
  `rg`, `git grep`, `fd`, `git log -S`, `git log -G`, `git blame`.
- Structural search: `ast-grep`, Semgrep, Coccinelle/`spatch`, `weggli`.
- Static analysis: compiler diagnostics, `clang-tidy`, `scan-build`, `cppcheck`,
  `sparse`, `shellcheck`.
- Binary and symbolization: `gdb`, `objdump`, `nm`, `readelf`, `addr2line`.
- Runtime and tracing: `strace`, `ltrace`, `bpftrace`, Valgrind, `dmesg`.
- Silicon: `radeontool regmatch`, BAR reads through `resourceN`, live
  `radeontop` and `radeontop -d` output under a named load.

When an audit reports a code claim, cite how the symbol or path was found, not
only `file:line`: `(clangd: references on FUNC)`, `(global -r SYMBOL)`,
`(rg --fixed-strings SYMBOL .)`.

A missing tool proves nothing about the code. Record the package name, the
install source, and the validation command; when the tool cannot be installed,
record `not run` and why.

Every new probe, lint, or verdict-producing script earns trust by calibration
against known-good and known-bad inputs first.

Subagents are read-only evidence collectors unless the user authorizes a
different role; use at most three concurrently, and give each a bounded task,
input scope, expected output, and citation requirement. The parent agent owns
synthesis, conflict resolution, implementation choices, commits, deletions, and
final claims.

## Synthesis over selection

When merging parallel branches or review findings, preserve all non-refuted
content. Mechanism and evidence decide; chronology, branch age, and author do
not. Default additive resolution is union plus synthesis; selection requires
proof of refutation or supersession by a verified line-level diff and recorded
rationale.

After every merge resolution and before every commit, read the staged diff
adversarially:

```sh
git diff --staged
```

Verify each removed line was intentional, duplicated elsewhere, or refuted;
compare test labels with the commands they run; verify every symbol named in
comments or documentation against source. When anything non-refuted was
dropped, restore it, refute it by name and citation, or record it as explicit
follow-up.

When a reviewer finds a defect, fix the class, not only the instance. Add the
rule, check, or test that would have caught it.

## Security and hardware stop-line

Privileged register access and BAR mapping are the hazardous surface here.
Contain and report before continuing when a change exposes unchecked
`/dev/mem`, BAR, or PCI sysfs `resourceN` access outside the family gate that
selects it; an unchecked filesystem write or path traversal; command injection
through a shell wrapper or generated script; or sensitive data in logs and
generated artifacts.

Untrusted input passes allow-lists, normalization, and containment checks before
any shell or path use. Hazardous paths open on exact opt-in values; unset,
empty, and zero-valued gates stay closed.

## Engineering foundations

Advance through mechanism, evidence, implementation, and validation. Generate
bold hypotheses, then constrain them with source, register documentation,
silicon behavior, build results, and adversarial review. An idea becomes
repository value once it is expressed as code, documentation, probe methodology,
validation data, or a clearer model of the system.

Treat warnings and unexpected output as defects until explained. Every artifact
reproduces on a clean host: PATH-resolved tools, tracked regular files, and
documented dependencies. Notice anomalies while working; a surprising deviation
is evidence, and it changes the model or it does not, on the record either way.

Final artifacts end more accurate, reproducible, navigable, testable, and
source-grounded than their inputs.
