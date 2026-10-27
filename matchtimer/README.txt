MatchTimer.dll v1 - match clock (MM:SS) in Dawn of War II: Retribution 3.19 (Elite). Experimental, not tested in a match yet.

Shows how long the match has been going, in real time: 12:34, after an hour 1:02:34. It starts when the match
starts (after the loading screen) and keeps running through lags. In every game: multiplayer, offline vs AI,
replays (OnlineOnly=1 in the ini keeps it to live multiplayer). Never in menus, on the loading or post-game
screen. Only you see it: the DLL reads the game, it changes nothing in the simulation, so it can't desync anyone.

Install: put MatchTimer.dll and MatchTimer.ini into  <game>\dow2-patches\  (loaded by the winmm shim), or into
<game>\Elite_DLLs\ and add ", MatchTimer.dll" to InjectedFiles in Elite.launchconfig.
Settings: MatchTimer.ini (position, size, colours). The game picks up a saved change within a second.
Log: MatchTimer.log (next to the dll). Turn off: MatchTimer.ini -> Enabled=0.

Needs DoW2 Retribution 3.19 (Steam); 32-bit. On any other build it does nothing and says so in the log.
