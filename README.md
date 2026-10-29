# Dow2-patches

Small DLL patches for Dawn of War II: Retribution (DOW2.exe 3.19.1.10320, the current Steam build).
Plain C, built with MinGW. Every patch checks the bytes it is about to change and does nothing if the
exe is a different build.

    make                          # all DLLs into build/
    make STATS_HOST=other.host    # same, with another stats host baked into LobbyStats (default rts.fintebtc.de)

Needs gcc-mingw-w64-i686. CI builds every push and does a dry-run publish of the DLLs as a launcher add-on; the real publishing is described in Publishing to the launcher below.

Versions: the version of the set lives only in VERSION and needs an entry at the top of CHANGELOG.md. After a merge
to main CI tags vX.Y.Z; for X.Y.0 it also publishes a GitHub Release with the DLLs and the CHANGELOG entries since the
previous X.Y.0. Tags are set by CI, not by hand.

obsfix/
    Observer lag fix. When you spectate a live game the client asks "which slot am I in" dozens of times
    per frame, and every time it rebuilds the whole match info from the network session. ObsFix caches
    the answer for 500 ms. Write-up with addresses: docs/OBSERVER_LAG.md

lobbystats/
    Relic rating next to player names in the lobby, plus a tooltip with win rate, games and Steam hours; your own
    rating and that tooltip in the main menu too (the tooltip in the game's own font, read from its files).
    It only reads the lobby list: no kicking, no chat commands, no extra menus. Data comes from a small
    web service that reads the public Relic API (host: rts.fintebtc.de, or STATS_HOST at build time, or Server= in
    LobbyStats.ini). It runs in its own thread and doesn't touch the game's netcode.

reinforcefix/
    Auto-reinforce is lost when a squad goes through an Eldar webway gate. The auto-reinforce list is
    kept by the HUD and every tick it drops squads that have no models on the map, which is the case
    while a squad sits inside the gate. ReinforceFix skips such squads instead of dropping them.

vanilla-loader/
    winmm.dll loader. Forwards everything to the real winmm.dll and loads every *.dll from
    <game>\dow2-patches\, in vanilla and in Elite. Each patch refuses to load twice, so having the same
    DLL in Elite_DLLs as well is fine.

cooldowns/
    Cooldown numbers (inside LobbyStats.dll): seconds left on recharging abilities and global abilities in the middle of
    the button, over the game's own sweep, in the game's font. Only reads: the share the HUD shows, the engine's own
    recharge query and, for global abilities, the sim ticks left - no desync. LobbyStats.ini [Cooldowns]: Enabled,
    Size (px at 1440), Color.

frameprobe/
    The diagnostics DLL I used to find the observer lag: frame times, sim ticks, a stack sampler on the
    game thread, a probe on the post-game Continue button.

cgnumbers/
    Control-group numbers above squads (experimental, skeleton). When you assign squads to a control
    group (Ctrl+1..0) the group number is drawn on that squad's banner, handy in 2v2/3v3. Draws through
    the game's Direct3D9 (DXVK-safe). Stage 1 proves the render path; reading squads/groups is stage 2.

matchtimer/
    Match clock, built into LobbyStats.dll (no DLL of its own). MM:SS of game time since the match started (pauses
    not counted), in the game's own font (read from its files), through Direct3D9; off as soon as the match ends.
    Shown in every game, OnlineOnly=1 keeps it to live multiplayer. Look and position in LobbyStats.ini, section
    [MatchTimer], picked up while the game runs.

eliteai/
    The new AI (launcher add-on "New AI (test, skirmish only)"). EliteAI.dll hooks lua_load in LuaConfig.dll and puts
    eliteai/mod/main.ai into Elite's default.ai right before its InitializeAI() call (the army fights together, holds
    its target, waits for the others at the rally point), and appends the order queue to elite.scar. Elite's archives
    are not touched. In a skirmish the Normal computer gets it (the setup menu calls it "NEW AI Computer") (EliteAI.ini [Lua] NewAI=all|none|normal|<player ids>);
    only in single-player games (by the game type, [*(0x01335A78)+0x118]): online and in replays it stays out - the
    scripts run on every machine of a match, one player with it would desync everyone.

reverse/
    Backing off like the reverse order of Company of Heroes: hold Ctrl and right-click the ground, the selected squads
    back off to the point (also on the minimap), vehicles driving backwards instead of turning around: the game's
    own turn plans reverse on paths up to 30 m, so the way is driven in 25 m legs, the next one 8 m before the end of the current, without stopping. Only the point of the player's own right-click is moved: the order is the normal one, nothing in the
    simulation changes, no desync, the other players don't need it. Reverse.ini: Key, StepMeters, LeadMeters.

mmfind/
    Automatch (launcher add-on "Automatch (test)"): Multiplayer -> Find a Game -> Start Searching queues the player on
    the ladder service (rts.fintebtc.de/ladder/api/mm); when a match is found the host creates the room, the others
    join, the lobby options (VP, 500 points, fixed positions, standard resources), swaps and observers are set by the
    DLL, and the match starts by itself. Settings in MMFind.ini (server, maps, rating spread, chosen race/hero).
    Published by hand for now (launcher id "automatch", store version mmfind-<ver>), not through addons.json.

Installing with Elite: put the DLL into Elite_DLLs\ and add it to InjectedFiles= in Elite.launchconfig.
Without Elite: winmm.dll next to DOW2.exe, patches into dow2-patches\.

## Publishing to the launcher

`addons.json` is what the Elite Launcher gives players. Each add-on has a `version`; to ship a new build, raise it and
merge to `main`. Within ~2 minutes the main server pulls `main`, runs `make`, publishes `build/<dll>` as
`<id>-<version>` into the launcher's store, points `launcher.json` at it, signs, verifies, syncs the mirrors and posts
the result to the team's Telegram thread. Players get it on the next launcher refresh (≤ 15 minutes).

- A version is published once and never changes: new code needs a new version (`0.7.1`, `0.8.0`, …). Pushing code
  without raising the version publishes nothing.
- Going back = put the old version back into `addons.json` (it is already in the store, nothing is rebuilt).
- `files` ships more than the DLL: `{"EliteAI/main.ai": "eliteai/mod/main.ai"}` = path under `Elite_DLLs` → file in
  the repo (`.ai`/`.scar`/`.lua` are syntax-checked with luac5.1 first).
- Optional fields override the launcher's texts: `name`, `description` (`{"en": …, "ru": …}`), `default` (on for
  new players), `conflicts`, `options`. A new add-on needs `dll`, `name` and `description`.
- Only add-ons listed here are touched; the others in the launcher (automatch, relay) are managed elsewhere.
- No secrets in GitHub: the server pulls with its deploy key and signs with the store key it already has.
  If something fails (no build, not a 32-bit DLL, signature check), nothing is published and the thread says why.
- Test on a branch first: CI builds every push. Don't raise a version in `main` before the DLL has run in a real game,
  minimised and restored included: everyone with the add-on on gets it.

MIT.
