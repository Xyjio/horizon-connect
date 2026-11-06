# How Horizon Connect works

Forza Horizon 2's online play needed Xbox Live (sign-in, profiles, the multiplayer session directory, QoS,
party) and Turn 10's game servers (web services, plus a dedicated NerdCore server that relayed every player's
cars). Horizon Connect answers all of that on the players' own PCs.

## The pieces

| Where | What | Source |
|---|---|---|
| every player | `HorizonConnect.dll` – an Extra Options plugin inside the game process | `plugin/` |
| every player | the local server: web services on `127.0.0.1:5101` (TCP) and a relay on the same port (UDP) | `server/` |
| the host | the playit.gg agent: a UDP tunnel from the internet to the host's relay | shipped `playitd.exe` |

```
 game ── web (WinHTTP / IXMLHTTPRequest2) ─► the DLL ─► http://127.0.0.1:5101   (fh2srv.py, own PC)
 game ── UDP (NerdCore) ─────────────────────────────► relay of the server you joined
                                                        own PC: 127.0.0.1:5101
                                                        a friend: <tunnel>.ply.gg:<port> ─► their relay
```

## plugin/ (HorizonConnect.dll)

- `horizonconnect.c` – start-up, settings (`HorizonConnect.ini`), the per-player identity, import hooks (WinHTTP, Winsock, WinRT
  activation), the Azimuth version table.
- `xhr.c` – the game's web requests (IXMLHTTPRequest2) go over WinHTTP to the local server, with the original
  host in `X-Horizon-Host`.
- `streams.c` – a working Windows.Storage.Streams DataWriter (Azimuth's loses the bodies of the replies).
- `qos.c`, `assoc.c`, `party.c` – stand-ins for Windows.Xbox.Networking and Multiplayer.Party answers Azimuth
  gives offline (QoS measurements, secure device association, party registration).
- `patches.c` – the player id (Azimuth's XboxUserId) and two waits the game would never finish without Turn 10's
  traffic / weather service.
- `online.c` – starts the local server (hidden, ends with the game), sets up and runs the playit agent,
  decides where the game's UDP goes (`online_set_join`) and asks GitHub for the newest release
  (`CheckUpdates`).
- `menu.c` – the HORIZON CONNECT pages in the pause menu: native pages of the game (a VideoOptions page with its own
  XAML), the server list with status pings, adding / editing servers, hosting; joining fires the pause menu's
  own `online_private_freeroam` transition.
- `trace.c`, `input.c`, `json.c`, `proxy.c` – debugging aids (breakpoint traces, scripted controller input,
  JSON and WinRT call logging).

## server/

- `fh2srv.py` – HTTP stand-ins: Xbox Live (profiles, presence, session directory, QoS servers, game server
  allocation) and Forza's LSP services. The LSP message formats are read from the player's game executable with
  `pefile` (`xls.py`), so nothing of the game is copied into this project.
- `relay.py` – the NerdCore relay (formats in `nerd.py`): answers each console's connection, owns the lobby's
  SessionHost object (an invite-only session), forwards every object, car update, collision and repair to the
  other players (numbering object updates per receiver) and replays the history to players who join later. It
  chooses the session leader - the host when they join, else whoever has been in the session longest - and sets
  it in each player's PlayerData (`IsLeader`). It owns the session's WeatherSynced object: the first console's
  FullWeather sets it, later Atmosphere / Precipitation requests keep it moving. The time of day online is the
  server clock (48 game seconds per real second), so the clock starts at `--start-hour` (10:00). It also answers `FH2O?` with `FH2O!{"players": n, "max": m}` for the server list.
- `run_server.py` – starts `fh2srv.py` and keeps its output in `logs\`.

## Settings

`HorizonConnect.ini` next to the DLL. The shipped one (`package/HorizonConnect.ini`) is all a player needs; `Xuid=auto` and
`LocalAddress=auto` are replaced by the player's own values at the first start.
