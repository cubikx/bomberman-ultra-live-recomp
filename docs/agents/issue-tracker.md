# Issue tracker: GitHub (bugs only)

GitHub issues on `cubikx/bomberman-ultra-live-recomp` are for **bug reports only**. Use the `gh` CLI for all operations.

Features, specs, plans and other tickets do **not** go on GitHub. When a skill wants to publish non-bug work to the issue tracker, don't create an issue: ask the user where it should go.

Never put game data, or anything derived from it, in an issue, comment or label.

## Conventions

- **Create an issue** (bugs only): `gh issue create --title "..." --label bug --body "..."`. Use a heredoc for multi-line bodies.
- **Read an issue**: `gh issue view <number> --json number,title,body,labels,comments`.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."`

Infer the repo from `git remote -v`; `gh` does this automatically when run inside a clone.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

## When a skill says "publish to the issue tracker"

If it is a bug, create a GitHub issue. Otherwise ask the user where it should go.

## When a skill says "fetch the relevant ticket"

Read it as in **Read an issue** above.

## Wayfinding operations

Not set up. `/wayfinder` maps and tickets are planning work, which doesn't go on GitHub here; ask the user where to keep them.
