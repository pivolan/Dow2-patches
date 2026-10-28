# Changelog

Every version gets an entry here before it reaches `main`. CI then tags `vX.Y.Z` on its own; for a minor or major
version (`X.Y.0`) it also publishes a GitHub Release with the DLLs and the entries since the previous `X.Y.0`.
The version of the set lives only in `VERSION`; each DLL still writes its own version to its log.

## 0.1.2 — 2026-10-28
- LobbyStats v0.7.5: own rating after the name in the main menu and the stats tooltip on hover over it (the game's own
  Arial Unicode MS, read from its files, in a frame like the game's tooltips); the stats host
  is baked in by default, so CI builds show numbers again; `[Debug] LogInvoke=1` logs the Flash calls of the UI.

## 0.1.1 — 2026-10-28
- MatchTimer v1: match clock (MM:SS of game time, pauses not counted) in a box next to the Menu button, experimental.

## 0.1.0 — 2026-10-26
- ObsFix v1: observer lag fix, the "which slot am I in" lookup is cached for 500 ms.
- LobbyStats v0.7.0: Relic rating next to player names in the lobby and a tooltip with win rate, games and Steam hours;
  the host filter and auto-kick are gone.
- ReinforceFix v1: squads inside an Eldar webway gate keep auto-reinforce.
- CGNumbers v1: control-group numbers above squads, experimental skeleton (render path only).
- FrameProbe v9: diagnostics DLL (frame times, sim ticks, game thread stack sampler).
- winmm.dll loader: loads every DLL from `dow2-patches\` in vanilla and in Elite; each patch refuses to load twice.
