# Changelog

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
