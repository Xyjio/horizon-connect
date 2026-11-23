# Horizon Connect

**Play Forza Horizon 2's online free roam with your friends again** – on your own PC, without the
Microsoft servers that were shut down.

Horizon Connect is a mod for **Azimuth** with
[Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions). One player hosts, the others join.
It tunnels through [playit.gg](https://playit.gg), so you don't have to forward any ports.
Everything needed is inside the mod folder; nothing else has to be installed.

**There is no central server and no matchmaking.** Like a LAN party over the internet, one of you hosts a
session on their own PC and the friends join that PC directly - it works much like peer-to-peer play with a host.

> 🚧 **Work in progress:** what's being worked on and what comes next → [ROADMAP.md](ROADMAP.md)


## How playing together works

```
 Friend's PC ──┐
 Friend's PC ──┼── internet ── playit.gg tunnel ──►  Host's PC: the game + the session server (relay)
 Friend's PC ──┘                                       (started from the F12 menu, runs while the host plays)
```

- **The host** is a normal player. Their PC runs the session: a small server inside the mod folder that passes
  every player's cars, races, weather and traffic on to the others. It runs only while the host's game is open.
- **Friends** add the host's address once (it looks like `name-something.tun.ply.gg:12345`) and join
  from the F12 list. Their game talks to the host's PC; nobody else is involved.
- **No public servers, no matchmaking** - only people who have your address can join, and there is no central
  server to go offline. The playit.gg tunnel only forwards packets so you don't have to open router ports; the host
  sets up a free playit.gg agent once (from the F12 menu), friends need nothing for joining.
- **Every player's PC** also answers the game's own Xbox Live and Forza web requests locally (profiles,
  sign-in), so the game works without the shut-down Microsoft services.
- If the host leaves, the session ends for everyone. Up to 11 players per session.

## Features

- **In-game server list** – open the pause menu in free roam and press **F12** (controller: **View**):
  - your saved servers with their status (`online · 2 players` / `can't be reached`)
  - **Add server** – the address you copied (e.g. from Discord) is filled in by itself
  - **Host a server** – set up once, then *Start the server and join* with one button
- **Hosting over the internet** without port forwarding: the built-in playit.gg agent makes a tunnel; your
  address stays the same, so friends only have to add it once.
- **Joining starts the game's own *Private Free Roam Online*** – no menu digging.
- **Races together** – the session leader starts an event, the others join from the game's prompt: starting
  grid, shared start, positions and lap times, results and rewards, then back to free roam. A player whose game
  can't load the event is taken out of it after a while (back to free roam) and the others race on.
- **King and Infected** – the Playground games, with the crown or the infection passed on between players.
- **Crash guards** for players coming and going and for cars with broken data (e.g. modded cars), and the
  game's video clips (Game DVR), which Azimuth can't record, are turned off.
- **Traffic in online free roam** – the same traffic cars for everyone, at the same places; hit one and it reacts
  for everyone. The traffic is shared out between the players' games (each drives the cars near it, as Turn 10's
  server did).
- **Same weather and time of day** for everyone, collisions between players, the leader's star.
- After the finish of a race, the other players' cars are hidden until you're back in free roam (they no longer
  stand inside your car on the results and rewards screens).
- **Every player gets their own player id** automatically (and keeps their save).
- German and English menu texts (follows the game's language).

## Requirements

- Forza Horizon 2 for Xbox One, version **1.0.0.64**, running in **Azimuth 1.0.4**
- **Extra Options 0.4.4** (the mod loader)
- Windows 10 or 11

This repository contains **no game files**. You need your own copy of the game.

## Install

1. Download `Horizon-Connect-1.3.0.zip` from the [Releases](../../releases) page.
2. Unzip it and copy the folder `Horizon Connect` into `<Azimuth folder>\mods\`.
3. Start the game. If Windows Firewall asks about `python` or `playitd`, allow it.

Full guide: [`docs/INSTALL.md`](docs/INSTALL.md)

## How it works

- **`plugin/`** – `HorizonConnect.dll`, an Extra Options plugin. It hooks the game's network imports and a few of
  Azimuth's WinRT answers so the game's Xbox Live calls get usable answers, starts the local server and the
  playit agent (`online.c`), and adds the HORIZON CONNECT pages to the pause menu (`menu.c`).
- **`server/`** – `fh2srv.py`: stand-ins for the Xbox Live and Forza web services the game needs, plus
  `relay.py`, a NerdCore relay that forwards every player's cars and objects to the others. The message formats
  are read from the game's own executable at start, so nothing of the game is copied here.

More detail: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Known limitations

- Up to 11 players per server.
- The crown next to every gamertag is the game's VIP badge (Extra Options `OfflineVIP=1`), not the session
  leader. The leader has the star icon in the player list.
- Liveries of others show once they were saved online in the paint shop.
- Joining from the list starts free roam on the mainland (not Storm Island).
- Traffic cars can have a different colour on each player's screen.
- After a player leaves and joins again, the time of day can differ between players.
- Only the session leader (star) can start an event, and at least two players must be in the session.
- Road trips, co-op Bucket List challenges and AI drivers (Drivatars) in online races aren't supported yet.

What's being worked on and what comes next: **[ROADMAP.md](ROADMAP.md)**.

## Credits

- **FH2Net** by conner-mcvay – the network hooks and the server this project grew from (MIT).
- **Extra Options** by Arthur (RokkuDayo) and **EO Weather Control** (Set Time & Weather FH2 by Pantsedit) –
  the technique for native menu pages (GPL-3.0).
- **FH2MP** by Charelene – the texture fallback, the crash guards, the King / Infected rules, the event load timeout
  and the online traffic manager (GPL-3.0).
- **playit.gg** agent (BSD-2-Clause), **Python** (PSF), **pefile** (MIT).

See [`NOTICE.md`](NOTICE.md) for the licences.

## License

**GPL-3.0** – see [`LICENSE`](LICENSE). Parts that come from FH2Net are also available under the MIT licence
([`docs/LICENSE-FH2Net-MIT.txt`](docs/LICENSE-FH2Net-MIT.txt)).

Horizon Connect is an unofficial fan project. It is not affiliated with or endorsed by Microsoft, Xbox Game Studios,
Playground Games or Turn 10 Studios. Forza Horizon 2 is a trademark of Microsoft.
