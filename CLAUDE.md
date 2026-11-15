# Project rules

## Changelog
Every commit that changes the project includes its entry in `CHANGELOG.md`, in the same commit, under
the section for the commit's date (`## YYYY-MM-DD`, newest first; add it if missing), then `Added`,
`Changed` or `Fixed`. Write for people building or playing the port: what changed for them, plus
anything they must do. Never mention game data or commit anything derived from it.

## Agent skills

### Issue tracker

GitHub issues, for bug reports only; other work doesn't go on GitHub. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five labels (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
