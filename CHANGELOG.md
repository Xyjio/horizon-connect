# Changelog

## 1.2.2

- Fix: a player could stay on the loading screen for good after picking a car for an online race. That happened
  when their game couldn't load the event (for example another player's car from a DLC or car pack they don't
  have) or took longer than a minute to load the track - the race then started without them. Now:
  - who hasn't loaded the cars 20 s after the others have (at the latest 60 s after the countdown) is taken out of
    the event and is back in free roam; the others race on. If nobody loads, the event ends for everyone;
  - the race waits up to 4 minutes for a slow PC to load the track (it was 1 minute).
  Both as FH2MP (GPL-3.0) does it.
- The host's server log says when each player loaded the cars and the track, and who was taken out.
- Only the host needs 1.2.2.

## 1.2.1

- Fix: the game could stop out of the blue with "Azimuth stopped: activation
  Windows.Xbox.Media.Capture.ApplicationClipCapture" (naming HorizonConnect.dll). At notable moments the game
  records a video clip (the Xbox's Game DVR), which Azimuth can't do. The plugin now turns those clips off with the
  game's own switch for it. The error came from the game, not from online play - it could happen in solo free
  roam too.

## 1.2.0

- **King and Infected** (the Playground games) online. The session leader starts them like a race; the server says
  who has the crown or is infected, takes the hits the games report and tells everyone. King ends at the time
  limit the leader picked; Infected when everyone is infected (or at the time limit).
- **Crash guards** (ported from FH2MP, GPL-3.0). The game no longer stops when:
  - a player comes or goes just as the game updates the other cars;
  - a car has broken engine data (for example a modded car) - it is shown as the game's own stand-in car;
  - a car's upgrade data is missing;
  - the game logs something for a player it has no record of.
  `[guards] Enabled=0` switches them off.
- Fix: when the session leader left and came back, the next event they set up didn't reach the others (they got
  the previous event instead).
- Only the host needs 1.2.0 for King and Infected; the crash guards help everyone who has them.

## 1.1.2

- Fix: a player whose car wore a design with vinyls (layers) saved online crashed at every join ("Azimuth stopped:
  CreatePlacementTexture2D of semantics not carried"). The game draws such a design into a texture Azimuth can't
  make; the plugin now makes it as an ordinary texture instead (the fallback comes from FH2MP, GPL-3.0). The
  design may look a little wrong, the game goes on.
- The local server sends designs with layers only to a game that has this fix.

## 1.1.1

- Fix: a player who came into the session after the leader had set up an event crashed when joining the next
  event on the same route. Players who come later now get the event's whole setup.
- A player who comes into the session (or comes back) while an event is counting down gets the join prompt too.
- After the leader role was passed on, the new leader's event setup could be lost for the others; fixed.
- A race ends right away when the players still racing leave and everyone left has finished.
- The leader's hint "You are the president ..." no longer stays on screen all session (`ShowLeaderHint=1` in
  `[online]` brings it back). It also showed the new leader twice in the online player list.
- Gamertags of the others are asked for up to three times, so a lost packet no longer shows "Driver 1234".
- The host's server log names each event, its players, finish times and why it ended.

## 1.1.0

- **Online events and races.** The session leader (star) starts an event in online free roam as usual; the others
  get the game's join prompt. Everyone who joins picks a car, is on the starting grid, and the race starts at the
  same moment for all. Positions, lap times and results are shared, and afterwards everyone gets their rewards and
  drives on in free roam. Races can follow one another;
  a player who leaves or is disconnected drops out of the race, the others race on.
- Only the host needs 1.1.0 for races (the server does the work); still, everyone should update.
- Tested with a simulated internet line (150-250 ms ping, 5 % of packets lost) between two players.

## 1.0.6

- The other players' gamertags instead of "Driver 1234" (everyone in the session needs 1.0.6 for this).
- More stable sessions over the internet: lost packets are sent again, objects keep updating in long sessions,
  bad or strange packets are skipped, and unusable weather values are ignored.
- The host's server only listens to this PC and the playit tunnel (for LAN play without playit:
  `OpenToLan=1` in `[online]`), and the local web server answers only the game.
- A mod folder copied to another PC gets its own player id there; settings keep any characters (the ini is
  stored as Unicode); old server logs and captures are cleaned up.
- The update check also finds releases marked as pre-release; restarting after an update waits for open game
  windows instead of ending the game by force.
- Note: starting an event (race) in online free roam doesn't work yet and can lock the menu.

## 1.0.5

- Fix: players were disconnected again and again. When a part of a roadside object came off, the host's server
  read the message wrongly and stopped; after 15 s everyone was dropped. A bad packet no longer stops the server,
  and that message is read correctly (the part coming off isn't shown to the others yet).
- Packets from outside the game (port scans through the tunnel) no longer take player slots.

## 1.0.4

- Update with one button: when a new version is out, A on its row in the server list downloads and installs it,
  then A restarts the game. Your settings and servers stay. (From 1.0.3 you update by hand once more.)
- When your playit agent was deleted on playit.gg, the host page says "Agent deleted" and offers "Set up a new
  one".

## 1.0.3

- Same weather and time of day for everyone: the server takes the weather from the first player's game and
  sends every change to all players. The time of day starts at 10:00 when the server starts; online, a game day
  lasts 30 minutes.
- The relay now reads every message the game sends and passes on the ones for other players (also breaking
  glass and parts coming off).
- The server list shows when a new version of Horizon Connect is out (A opens the download page). Turn it off
  with `CheckUpdates=0` in `[online]`.
- Note: still no traffic in online free roam.

## 1.0.2

- One session leader (star icon in the player list, "You are the leader" hint): the host when they join,
  otherwise whoever has been in the session longest. It passes on when the leader leaves or hands it over.
  The session is now invite-only (private), as the game expects for private free roam.
- Car-to-car collisions and repairs are sent between players.
- The relay keeps every player's updates in order, even when packets arrive out of order.
- Note: the crown next to gamertags is the VIP badge (Extra Options `OfflineVIP`), not the leader.

## 1.0.1

First public version.

- In-game server list in the pause menu (F12 / View): add, edit, delete servers; status and player count of each
  server; join with one button (starts Private Free Roam Online).
- Hosting over the internet with the built-in playit.gg agent: set up once in the menu, then start, copy the
  address, stop.
- A local server on every player's PC (web services) and a relay for the host (game traffic), started and stopped
  with the game.
- Every player gets their own player id automatically; the save so far is copied for it.
- German and English menu texts.
