LobbyStats.dll v0.7.5 - Relic rating in the Dawn of War II: Retribution / Elite lobby and main menu

What it does
  In the game lobby each player's name gets the Relic rating appended:  PlayerName  [2087]
  Hover a player slot to see a tooltip: Relic rating, win rate (wins / losses), games, Steam hours.
  In the main menu your own name gets your rating too, and hovering the [rating] shows the same stats. That tooltip
  is drawn by the DLL in the game's own font (Arial Unicode MS, read from the game's files at start) in a frame like
  the game's tooltips; the rest of the name bar keeps the game's own "Currently logged in player".
  Data: a small stats web service (rts.fintebtc.de, or Server= in the ini) that reads the public Relic API.
  The first time a name appears it is shown without numbers; they appear on the next lobby update
  (someone joins, readies, changes race) and instantly in later lobbies (cache file next to the DLL).
  It only reads: no kicking, no chat commands, nothing sent to other players.

Install
  Without Elite: winmm.dll next to DOW2.exe, LobbyStats.dll and LobbyStats.ini into <game>\dow2-patches\.
  With Elite: LobbyStats.dll and LobbyStats.ini into <game>\Elite_DLLs\ and ", LobbyStats.dll" at the end of
  InjectedFiles= in Elite.launchconfig.
  Check: LobbyStats.log next to the DLL should contain "hooked Invoke at 0045DB6C" and later "lookup N names".
  Only DOW2.exe 3.19 (current Steam build) is supported; on any other build the DLL does nothing and says so in the log.

Options (LobbyStats.ini, see comments inside)
  [LobbyStats] Stats=0            - no numbers at all
  [LobbyStats] ShowRatingInName=0 - keep names untouched, show the rating only in the tooltips
  [LobbyStats] Server=host        - another stats service
  [MenuTooltip] DX, DY, MinWidth  - where the main menu tooltip sits, so it covers the game's own one
  [Debug] LogInvoke=1             - LobbyStats_invoke.log: every Flash call of the UI (for finding UI functions)

Remove: delete LobbyStats.dll (and remove it from InjectedFiles with Elite).
