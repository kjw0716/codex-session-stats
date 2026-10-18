# Changelog

## 0.2.0 — 2026-10-18

- Add a native dashboard and independent floating statistics window sharing one process. Floating updates continue when Dashboard closes.
- Resize the floating widget down to a 220 × 40 single line. Pack metrics by measured text width with two-space gaps and hide only metrics that cannot fit.
- Show context used/capacity with an adjacent green/yellow/red indicator from the latest local request snapshot. Missing data stays gray.
- Add adaptive metric cards, a compact actions menu, Mini/Compact/Details presets, saved dimensions, pinning, and 35–100% opacity.
- Add native desktop icons, portable installation, and optional startup through a verified desktop ChatGPT shortcut, with backups of replaced shortcuts.
- Support relay-provider usage through local records without provider credentials or a debugging connection in floating mode.
- Keep embedded mode available and wait for normal app exit when a restart is needed; never force-close the desktop app.
- Bound conversation selection to 30 recent main sessions and retain compatibility with older metadata schemas.
- Add native panel, layout, context-boundary, opacity, lifecycle, relay fixture, and installation checks.
- Refresh the README with floating screenshots, mode-specific instructions, context provenance, and separate memory measurements.

## 0.1.0

- Native Windows C++ helper with incremental log reading and one selected-session summary.
- Eight selectable session metrics in an English in-app status bar and panel.
- Native Codex context usage with green/yellow/red capacity indicators.
- No persistent Node.js/Python runtime or separate dashboard window.
- English start/stop launchers, a portable Windows x64 package, and synthetic reader/UI tests.
- About 11 MB helper working set observed in local Windows 11 testing.
