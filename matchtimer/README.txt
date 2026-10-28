MatchTimer.dll v2 - match clock (MM:SS) in Dawn of War II: Retribution 3.19 (Elite). Experimental, tested offline vs AI.

Shows how long the match has been going, in game time: 12:34, after an hour 1:02:34, next to the Menu button in the
top right corner, in a box drawn like the game's own (the replay panel). Style=1: text only in the look of the
campaign's objective timers (Font=Caslon Antique New: the game's own, read from its files at start, Book Antiqua
if that fails; Outline=000000). Anchor=1 moves it right of the victory points bar, Style=0
anywhere on the screen. It starts when the match
starts (after the loading screen) and stands still while the game does (pause, an offline game minimized, "waiting
for players"), so everyone in the match and the replay see the same time. In every game: multiplayer, offline vs AI,
replays (OnlineOnly=1 in the ini keeps it to live multiplayer). Never in menus, on the loading or post-game
screen. Only you see it: the DLL reads the game, it changes nothing in the simulation, so it can't desync anyone.

Install: put MatchTimer.dll and MatchTimer.ini into  <game>\dow2-patches\  (loaded by the winmm shim), or into
<game>\Elite_DLLs\ and add ", MatchTimer.dll" to InjectedFiles in Elite.launchconfig.
Settings: MatchTimer.ini (style, font, position, size, colours). The game picks up a saved change within a second.
Log: MatchTimer.log (next to the dll). Turn off: MatchTimer.ini -> Enabled=0.

Needs DoW2 Retribution 3.19 (Steam); 32-bit. On any other build it does nothing and says so in the log.
