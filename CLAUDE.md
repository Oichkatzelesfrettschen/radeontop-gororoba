@AGENTS.md

# Claude Code Loader for radeontop-gororoba

## Loading rule

`AGENTS.md` owns the rules; the `@AGENTS.md` import above loads them. The import
path is spelled in that exact case: imports resolve literally, and this
filesystem is case-sensitive. This file carries Claude Code operating notes
only; shared doctrine lands in `AGENTS.md`.

Each section below names the `AGENTS.md` section it defers to, so a rule has one
home and this file adds only what is specific to Claude Code. A rule that would
apply to any agent belongs in `AGENTS.md` instead.

When Claude Code starts inside a parent workspace or a temporary worktree, load
`radeontop-gororoba/AGENTS.md` before editing radeontop paths. These rules govern
every edit under this repository regardless of launch directory.

## Claude Code operating notes

Inspect the real repository with Claude Code tools before editing; memory, prior
summaries, and recalled context are leads, and `AGENTS.md` plus source are
authority.

Inspect the diff after every edit; the adversarial staged-diff read from
`AGENTS.md` runs before any commit or completion claim.

Claude Code task tracking is transient working state; durable state lands in
code, commit messages, findings, documentation, or retained bundles.

Subagent limits, the read-only default, and citation duties live in `AGENTS.md`
under `Tooling for investigation and audits`.

## Response shape

`AGENTS.md` owns how a response reads, under `Response and report prose`, and
that section governs every response about this repository: the answer first,
one new fact per sentence, plain verbs over nominalizations, quantities at the
precision the argument uses, and scarce emphasis. The mandatory content lives
there too -- outcome, decision, evidence, validation run, an unrun check as
`not run` with its reason, and residual uncertainty.

Claude Code renders a response as GitHub-flavored Markdown in a terminal, so
headings, tables, and bold render live, and the scarcity rule applies to what
they mark on a screen the reader scrolls once.

Deliberation lives in thinking blocks. Chained reasoning reaches the response
only where it explains the next action or a validation requirement.
