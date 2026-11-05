# Changelog

## 1.0.2

- Only one player has the crown (session leader): the host when they join, otherwise whoever has been in the
  session longest. It passes on when the leader leaves or hands it over.
- Car-to-car collisions are sent between players.
- The relay keeps every player's updates in order, even when packets arrive out of order.

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
