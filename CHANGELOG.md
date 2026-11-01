# Changelog

Changes that matter to people building or playing the port, newest first, grouped by the date they
were committed.

## 2026-11-01

### Added
- First import of the recompiler, runtime, tools and build scripts.
- README: a Features section, and the project's Bomberman ULTRA LIVE! name.

### Changed
- `render_scale` now defaults to `1` (1280x720, the PS3's resolution) on every display, so handhelds
  and older graphics cards aren't asked to render far more pixels than they show. `auto` and `2` to
  `4` are still available in `settings.ini`. A settings file that already has `render_scale` keeps
  its value; one without it gets `1`.
- README: new setup steps with winget and PowerShell commands, measured timings, how to update,
  `deploy.cmd` and more troubleshooting.

### Fixed
- The recompiler now works from a fresh clone. It used to read system function names from a
  git-ignored RPCS3 checkout, and without one the built game crashed on startup. The names this game
  imports are now in `recomp/import_names.txt`, and an unknown function stops the recompile with an
  error.
