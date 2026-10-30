# Changelog

Every version gets an entry here before it reaches `main`. CI then tags `vX.Y.Z` on its own; for a minor or major
version (`X.Y.0`) it also publishes a GitHub Release with the DLLs and the entries since the previous `X.Y.0`.
The version of the set lives only in `VERSION`; each DLL still writes its own version to its log.

## 0.1.5 — 2026-10-30
- LobbyStats v0.9.3 - the cooldown numbers get their own tick in the launcher (off by default, as in 0.9.2): addons.json
  now lists the LobbyStats options (match timer, loading screen ratings, join sound and the new `[Cooldowns] Enabled`), so
  the tick writes the key into LobbyStats.ini. The DLL is the same as 0.9.2 but for its version in the log.
  - checked with GCC 14.2 (the server's compiler): 0.9.1 built with it crashes on the cultists' prayer exactly like the
    published DLL, 0.9.2 does not.

## 0.1.4 — 2026-10-30
- LobbyStats v0.9.2 - the crash on abilities fixed (a player: the game closed on W, the cultists' prayer):
  - cooldowns/: the game's GetBounds (0x6859E0) pops its own 4 arguments (ret 0x10) but was declared __cdecl, so the first
    time a button showed a recharge our stack moved by 16 bytes and the game crashed. Declared __stdcall now.
  - the ability button hook asks the engine nothing any more (0.9.0 asked vt+0xB4 for the share of every button with the
    owner cached in the button; the 0.9.1 attempt to ask vt+0xB0 first crashed the game at the start of a match). The
    numbers come from the panel's own SetProgress; hidden or empty buttons are cleared. No numbers out of energy for now.
  - the numbers stay off by default (`[Cooldowns] Enabled=1` in LobbyStats.ini turns them on), as in the 0.9.1 hotfix.
- MMFind 0.28 (outside addons.json, released by hand): slot swap requests are auto-accepted only in a ladder/automatch
  lobby, not in a plain public game; `[MMFind] AutoSwap=0` still always asks.

## 0.1.3 — 2026-10-29
- LobbyStats v0.9.0 (carries the match clock and the cooldown numbers, one Direct3D hook):
  - cooldowns/: seconds left on recharging abilities and global abilities in the middle of the button, over the game's
    own dark sweep, in the game's Caslon Antique New with its text shadow. Read-only (no desync): the share of each
    button from the panel's SetProgress 0x688620, abilities also asked from the engine after the panel's update (out of
    energy the game shows no sweep), the rate per sim tick and remembered per ability of each squad; global abilities
    read exactly (sim ticks left). `[Cooldowns] Enabled, Size (px at 1440), Color` in LobbyStats.ini.
  - main menu: the stats go into the game's own tooltip of the name bar (Relic rating as the title, win rate, games,
    Steam hours), no box of ours drawn over it; `[MenuTooltip] Game=0` brings the box back.
  - loaded late by the Elite injector (the main menu is up by then): the own name comes from Steam and gets its rating.
  - the match clock (and the numbers) go off when the match ends - the loading screen out of the match is built,
    the game is decided or the match scripts stop - instead of staying on the frozen loading screen.
- MatchTimer.dll on its own is gone (the clock has been inside LobbyStats.dll since 0.8.0); an old hand-installed copy
  is still recognised and left to draw the clock alone.

## 0.1.2 — 2026-10-28
- MatchTimer v2: the digits in the game's own Caslon Antique New, read from the player's game files
  (common/gamefont.h, nothing of the game shipped), above the minimap with the game's own text shadow, like the campaign
  timers; Style=2 box, Anchor=0 by the Menu button, Anchor=1 by the VP bar.
- DLLs are stripped (-s): 25-82 KB instead of ~220-280 KB.
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
