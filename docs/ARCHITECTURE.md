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
  traffic / weather service. It also keeps the session leader's hint ("You are the president ...") from being
  shown (`[online] ShowLeaderHint=0`): with no server to take it away it stayed all session, and showing it listed
  the new leader twice in the online player list.
- `guards.c` – crash guards ported from FH2MP (`[guards] Enabled`): each checks the game's code first and sends a
  function (or one instruction) through a small stub in a code page just below the game image.
  RemotePrediction (1409224DD: a network car whose player has no user in the session gets a stand-in user that says
  no), GameplayLog (1409C88E0: an event for a user with no log record is left out), TorqueScale (14105E530: a missing
  upgrade rowset is read as empty), TorqueLoad (140FCFE70: a car whose engine, camshaft or torque curve is missing
  or unusable goes down the game's own unavailable-car path and shows as its stand-in car). Test switches
  `TestForce` / `TestRejectCar` trigger the guards' other path on purpose.
- `online.c` – starts the local server (hidden, ends with the game), sets up and runs the playit agent,
  decides where the game's UDP goes (`online_set_join`), asks GitHub for the newest release (`CheckUpdates`),
  runs `update.py` / `restart.py` from the update row, and notices a playit agent deleted on playit.gg
  (`InvalidAgentKey`).
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
  server clock (48 game seconds per real second), so the clock starts at `--start-hour` (10:00). It also answers `FH2O?` with `FH2O!{"players": n, "max": m}` for the server list
  and `FH2N?` with the players' gamertags (`FH2N!{xuid: name}`, from each PlayerData), which every player's
  fh2srv asks when the game wants another player's profile (the DLL sends the relay's address in
  `X-Horizon-Relay`). Its sequenced packets are kept until the console acks them and sent again after 0.4 s; a
  packet it can't read is skipped; only an FH2 OpenConnection gets a player slot. It listens on 127.0.0.1 (the
  host's console and the playit agent) unless `--game-host ::` (`[online] OpenToLan=1`).
- Events (races), also in `relay.py` (`event_tick`), as Turn 10's server ran them: the leader's console asks for
  ownership of the lobby's GameOptions and sets the event up; the relay passes its GameOptions on to the others
  (whose consoles made their own copy, kept under handle flags 1, 0). The leader's MsgRequestRace makes the
  SessionHost race ready with a countdown and goes on to the others (their join prompt). Who is ready when the
  countdown ends takes part (ConsoleSynced ParticipatingInRace, also set on the others' copies); when all have
  loaded the participants' cars, MsgStartingGrid; when all are render ready, MsgStartRace a few seconds ahead,
  then SessionHost InRace. Lobby event messages start with the handle of the SessionHost. Scores
  (MsgInProgressScoreUpdate, MsgLapScore, MsgScoreKeyframe) go to the others with the player handle translated;
  after the last finisher (or 30 s after the first) InRace goes off, which ends the results screen. A player who
  says it is no longer ready drops out. Every GameOptions update is kept, so a console that comes into the session later gets the whole
  setup with its first one; a console that comes during the countdown gets the join prompt.
- Tag modes (King, Infected; `tag_*` in `relay.py`, rules from FH2MP): `gameopts.py` merges the leader's
  GameOptions updates and tells a tag event from its values (Infected by its header, King otherwise - confirmed or
  corrected by the first hit) and reads the time limit. 1.5 s after the grid the server picks the first It car(s)
  (King: one per six players) and sends MsgTagBuckets (37: twelve car handles as each receiver knows them, then
  their buckets, 0 / 1 = It) - again a few times and every 3 s. A console that sees an It car touch another sends
  MsgTagCollision (36: both cars, their buckets before and after); the server takes it if it matches what it has
  and sends the new buckets. The event ends at the time limit, or for Infected when everyone is It. The owner of the
  lobby's GameOptions counts its updates anew after every grant.
- `netsim.py` – testing only: with a `netsim.json` next to `relay.py` (`{"delay_ms": 70, "jitter_ms": 50,
  "loss": 0.05}`), packets to and from the tunnel's players are delayed and dropped like on a bad line.
- fh2srv answers only the game's requests (`X-Horizon-Host` and a 127.0.0.1 / localhost Host).
- `run_server.py` – starts `fh2srv.py` and keeps its output in `logs\` (the newest 30; capture lists over 20 MB
  are rotated, request bodies older than 7 days removed).
- `update.py` – installs the newest release from GitHub over the running mod: changed files are swapped in (files
  in use are renamed to `*.update-old`, deleted by the plugin at the next start), `HorizonConnect.ini` keeps the
  player's values and gets new settings added, and a failed swap is undone.
- `restart.py` – waits until the game has closed and starts Azimuth again (`--skip-launcher --game=<title>`).

## Settings

`HorizonConnect.ini` next to the DLL. The shipped one (`package/HorizonConnect.ini`) is all a player needs; `Xuid=auto` and
`LocalAddress=auto` are replaced by the player's own values at the first start; `[xbl] Owner` (a hash of the PC's
MachineGuid) makes a folder copied to another PC get its own id there. The DLL converts the file to UTF-16 once, so
names in any script survive `WritePrivateProfileString`.
