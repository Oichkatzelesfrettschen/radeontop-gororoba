@AGENTS.md

# Claude Code Loader for radeontop-gororoba

## Loading rule

`AGENTS.md` owns the rules; the `@AGENTS.md` import above loads them. The import
path is spelled in that exact case: imports resolve literally, and this
filesystem is case-sensitive. This file carries Claude Code operating notes
only; shared doctrine lands in `AGENTS.md`.

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

Responses report results, decisions, evidence, and remaining uncertainty in
mechanism-first form: changed mechanism, evidence used, validation run, tests not
run and why, risks or unresolved falsifiers. Chained reasoning appears when it
explains the next action or a validation requirement; the rest of the
deliberation lives in thoughtspace. Responses are plain ASCII mechanism prose
under durable names.
