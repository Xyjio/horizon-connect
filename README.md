# Horizon Connect

**Play Forza Horizon 2's online free roam with your friends again** – on your own server, without the
Microsoft / Turn 10 servers that were shut down.

Horizon Connect is a mod for **Azimuth** (the Xbox One translation layer for Windows) with
[Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions). One player hosts, the others join.
Everything needed – the server, Python, and the internet tunnel through [playit.gg](https://playit.gg) – ships
inside the mod folder; nothing else has to be installed.

> 🚧 **Work in progress:** what's being worked on and what comes next → [ROADMAP.md](ROADMAP.md)

## Features

- **In-game server list** – open the pause menu in free roam and press **F12** (controller: **View**):
  - your saved servers with their status (`online · 2 players` / `can't be reached`)
  - **Add server** – the address you copied (e.g. from Discord) is filled in by itself
  - **Host a server** – set up once, then *Start the server and join* with one button
  - join with **A**, edit with **X**, delete with **Y**
- **Hosting over the internet** without port forwarding: the built-in playit.gg agent makes a tunnel; your
  address stays the same, so friends only add it once.
- **Joining starts the game's own *Private Free Roam Online*** – no menu digging.
- **Every player gets their own player id** automatically (and keeps their save).
- German and English menu texts (follows the game's language).

## Requirements

- Forza Horizon 2 for Xbox One, version **1.0.0.64**, that **you own**, running in **Azimuth 1.0.4**
- **Extra Options 0.4.4** (the mod loader for Azimuth)
- Windows 10 or 11

This repository contains **no game files**. You need your own copy of the game.

## Install

1. Download `Horizon-Connect-1.0.1.zip` from the [Releases](../../releases) page.
2. Copy the folder `Horizon Connect` into `<Azimuth folder>\mods\`.
3. Start the game. If Windows Firewall asks about `python` or `playitd`, allow it.

Full guide: [`docs/INSTALL.md`](docs/INSTALL.md) · the same as plain text in every release ZIP: [`package/GUIDE.txt`](package/GUIDE.txt)

## How it works

```
 your game ── web calls ──► local server (fh2srv.py, 127.0.0.1:5101)          on every player's PC
     │
     └──── game traffic (UDP, NerdCore) ──► relay of the host (relay.py)
                                             ▲
              friends ── playit.gg tunnel ───┘                                 only on the host's PC
```

- **`plugin/`** – `HorizonConnect.dll`, an Extra Options plugin. It hooks the game's network imports and a few of
  Azimuth's WinRT answers so the game's Xbox Live calls get usable answers, starts the local server and the
  playit agent (`online.c`), and adds the HORIZON CONNECT pages to the pause menu (`menu.c`).
- **`server/`** – `fh2srv.py`: stand-ins for the Xbox Live and Forza web services the game needs, plus
  `relay.py`, a NerdCore relay that forwards every player's cars and objects to the others. The message formats
  are read from the game's own executable at start, so nothing of the game is copied here.

More detail: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Building

You need [Zig](https://ziglang.org) (its C compiler) for the DLL and Python 3 for the package.

```powershell
# Windows
.\tools\build.ps1                      # -> build\HorizonConnect.dll
python tools\package.py                # -> dist\Horizon-Connect-1.0.1.zip (downloads Python + playitd once)
```

```sh
# Linux (cross-compiles the Windows DLL)
./tools/build.sh
python3 tools/package.py
```

## Known limitations

- Up to 11 players per server.
- Every player is shown as session leader (crown).
- Car-to-car collisions are not sent between players yet.
- Liveries of others show once they were saved online in the paint shop.
- Joining from the list starts free roam on the mainland (not Storm Island).
- After choosing a server the game asks *"Free roam session found – press X"*: press X within 20 seconds.

What's being worked on and what comes next: **[ROADMAP.md](ROADMAP.md)**.

## Credits

- **FH2Net** by conner-mcvay – the network hooks and the server this project grew from (MIT).
- **Extra Options** by Arthur (RokkuDayo) and **EO Weather Control** (Set Time & Weather FH2 by Pantsedit) –
  the technique for native menu pages (GPL-3.0).
- **playit.gg** agent (BSD-2-Clause), **Python** (PSF), **pefile** (MIT).

See [`NOTICE.md`](NOTICE.md) for the licences.

## License

**GPL-3.0** – see [`LICENSE`](LICENSE). Parts that come from FH2Net are also available under the MIT licence
([`docs/LICENSE-FH2Net-MIT.txt`](docs/LICENSE-FH2Net-MIT.txt)).

Horizon Connect is an unofficial fan project. It is not affiliated with or endorsed by Microsoft, Xbox Game Studios,
Playground Games or Turn 10 Studios. Forza Horizon 2 is a trademark of Microsoft.
