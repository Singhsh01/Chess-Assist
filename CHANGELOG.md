# Changelog

## 1.0.0 — 2026-10-03

First release, built on the chess-3d renderer.

- PBR renderer: GGX + split-sum IBL, clear-coat, planar board reflections,
  4096² PCF soft shadows, contact AO + SSAO, depth of field, ACES tone mapping.
- Gallery hall environment: polished diagonal black/white marble floor and a
  photographic backdrop pinned to the 3D horizon; dark studio alternative.
- Side selection with animated camera flip (yaw 0° White / 180° Black).
- SAN move terminal with LAN mapping, disambiguation hints, en passant and
  promotion; structured move history.
- Asynchronous Stockfish UCI bridge, live win probability, best-move HUD and
  on-board recommendation glow; optional "Stockfish plays opponent".
- Loopback automation API + web console, `--headless` mode.
- GPU profiler overlay (F3) with per-pass timer queries.
- C++ unit tests, Playwright end-to-end suite, GitHub Actions CI.
