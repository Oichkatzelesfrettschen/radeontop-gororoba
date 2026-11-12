# radeontop-gororoba Agent and Developer Reference

## Instruction source

`AGENTS.md` is the root instruction file for `radeontop-gororoba` and owns the
rules. Codex, compatible agents, and human contributors read it directly.

`CLAUDE.md` is a tracked repository-relative symbolic link to `AGENTS.md`, so Claude Code reads this same body and the rules live in one place.

## Contributor essentials

- Create a branch before the first edit and stage paths by name: an uncommitted edit has no owner. The default branch is `master`; work lands through a branch, a PR, and a merge to `master`, then the merged branch is deleted locally and on `origin`.
- Write a commit subject as a component prefix plus the mechanism (`RS480: add a cf gauge for CF_PIPE_BUSY, the unmapped R300 engine lane`); write the body in one to five sentences naming the invariant, the change, and the evidence. Each commit builds and bisects, with formatting apart from logic. Keep a forge-appended `(#NNN)` off subjects.
- Name branches, commit subjects, PR titles, comments, and filenames by mechanism, never by phase, wave, or actor: `rs480-add-cf-pipe-gauge`, not `rs480-phase2-fix`. Set the branch name, first commit subject, and PR title before the first push. Ordinal containers such as `set5` or `batch_2` and the word `tranche` stay out of names.
- Write text emoji-free in American English; meaningful symbols stay. Comments state present-tense mechanisms and carry no issue numbers, PR chronology, local paths, or deictic time.
- Keep secrets, private hostnames, and absolute local paths out of tracked files, commits, and logs; scripts discover the root with `repo_root=$(git rev-parse --show-toplevel)`.
- Commits and PRs carry no AI attribution trailer; `Co-authored-by:` names human co-authors only. A patch sent to another project follows that project's disclosure policy, which the owner decides per submission.
- Build under the Makefile's `-Wall -Wextra` with no new warning, run `shellcheck -S error` on shell changes, and run the checks under Build, package, and validation. An unrun check reads `not run` with its reason; a new verdict-producing script calibrates on a known-good and a known-bad input first.
- Keep build, runtime, and silicon evidence apart, and mark known, hypothesized, and speculative claims. A hardware claim records the observation, the constraint, the hypothesis, the falsifier, and the command that produced the reading before the change. Cite how a code symbol was found with the location.
- Privileged register and BAR access is hazardous: a path to it opens on one exact documented token, as the Security and hardware stop-line describes.
- Merge by synthesis: keep every contribution that evidence has not refuted, and read `git diff --staged` before each commit.

## Hard rules

### Boundary and paths

- This repository is a fork of upstream `clbr/radeontop`. Build, install,
  package, and source-comment flows stand complete inside it.

### Root cause and evidence

- A behavior change names the exact chip class, register, bit, read path, and
  code path first.
- PCI IDs and register sources identify silicon; family nicknames serve prose
  only. `include/r300_pci_ids.h` and `include/r600_pci_ids.h` are the checked-in
  identity tables, and `familycheck.sh` gates them.
- A gauge claim rests on a register readback under a named load. A register
  decode from kernel headers is a hypothesis until silicon asserts the bit.

### Builds and languages

- Touched code builds cleanly under the Makefile's `-Wall -Wextra` and adds no
  warnings. Build targets and validation checks survive a narrow fix.
- C translation units stay at C11 or newer and keep the Makefile's configured
  standard.
- Shell scripts are POSIX `sh`, matching `familycheck.sh`, `getver.sh`, and
  `getamdgpuids.sh`; a script that requires `bash` declares it.

### Packaging

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

### Authorship, copyright, and license

- Historical pre-policy `Co-Authored-By: Claude` trailers stand in merged history.
- This repository is GPL3. Upstream radeontop headers, including
  `Copyright (C) YYYY Lauri Kasanen` and the GNU General Public License
  paragraph, stay verbatim through edits, splits, and file moves, with no second
  collective copyright line above them.
- A new file matches the header style of the adjacent file it joins, under the
  existing GPL3 terms. A fabricated personal-name line
  (`Copyright (c) YYYY <git config user.name>`) is LLM template output and gets
  stripped, as does an MIT or `Terascale Functionalists` line imported from a
  Mesa-derived tree.
- Source headers carry copyright and license content only; AI disclosure stays out of them.

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

## Prediction form for gauge exposure

If bit N of `RBBM_STATUS` is exposed on this part, it asserts under `[named
load]` and stays clear at idle. A bit that stays clear under load refutes the
exposure hypothesis for the sampled rate, load, and part, and the lane is
display-gated on that result. The decode keeps its field position and
architectural name, which the register document establishes. A higher-rate
sampler can assert a bit a slower one misses, so a non-observation records its
rate, window, and load.

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
make install PREFIX=/usr DESTDIR=/path/to/staging
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

## Comments, commits, and Markdown

Follow local style first. radeontop C uses tab indentation, K&R braces, `//`
line comments and `/* */` blocks, and the GPL3 file header. Mass reformatting
enters only as an explicitly requested formatting migration.

Documented behavior takes the plain indicative. Reproduced-but-undocumented
behavior names the part and the path where it was observed, so a reader knows
the claim rests on a run rather than a register document. Conjecture carries a
marker (`appears to`, `seems to`) or leaves.

Markdown loaded by agents uses exactly one H1, heading depth no deeper than
`###`, language tags on code fences, exact cross-references, and rule text as
direct positive-declarative statements. Tables appear only when columns carry
independent comparison value.

## Reply prose

A reply opens with the outcome in plain words: what was asked and what
happened, so a reader who stops after two sentences holds the result. A
sentence opens on something the reader already holds and closes on one new
fact, and ordinary connectives (because, so, but) state the relation between
sentences. Verbs stay verbs: `a census showed those bits never turn on, so the
three lanes stay masked` names who did what. A figure carries the precision the
argument uses (`about 96 million reads`); full-precision values and hashes
belong in the commit body, the finding, and the artifact. Bold marks the few
sentences a reader must not miss. A correction or retraction enters when it
changes what the reader should now believe or do; the sequence of attempts
stays in the commit body.

## Evidence boundary

This repository owns the monitoring tool: read paths, gauge mapping, packaging,
and the source comments that make them intelligible. Retained probes, result
bundles, findings, hazard policy, and the verdict assigned to a target-silicon
run live in the sibling projects, and this repository carries the citation.

Each sibling is an independent checkout that sits alongside this one in the
workspace. Cite a sibling by repository name plus its
repository-relative path, matching the `radeon-custom` README convention, and
let the workstation supply the equivalent absolute path.

- `steinmarder-r300`: the RS485M (Dell Vostro 1000, PCI `1002:5974`)
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
- `vostro1000-re`: the RS485M host-platform lane, holding the board, ACPI,
  COMBIOS, and firmware evidence under `systems/dell-vostro-1000/`.

Cite the exact path spelling that exists in the sibling. When the sibling
checkout is absent, the finding filename, the result-bundle name, the commit
SHA, or the public register source carries the citation.

Source comments prefer public register documentation, kernel symbols, DRM
names, and exact hardware identity. Empirical fork evidence belongs in findings
or commit messages unless the code needs the mechanism to be intelligible.

## Security and hardware stop-line

Privileged register access and BAR mapping are the hazardous surface here.
Contain and report before continuing when a change exposes unchecked
`/dev/mem`, BAR, or PCI sysfs `resourceN` access outside the family gate that
selects it; an unchecked filesystem write or path traversal; command injection
through a shell wrapper or generated script; or sensitive data in logs and
generated artifacts.

Untrusted input passes allow-lists, normalization, and containment checks before
any shell or path use. A hazardous path opens on one exact documented arming token; a
selector whose zero or `-1` is meaningful takes a separate arming variable.

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
