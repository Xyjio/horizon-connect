# Installing and playing Horizon Connect

## 1. What you need

- Forza Horizon 2 (Xbox One, version 1.0.0.64) that you own, running in **Azimuth 1.0.4**
- **Extra Options 0.4.4** installed (the mod loader for Azimuth)
- Windows 10 or 11 and an internet connection

## 2. Install

1. Back up your save first: copy `<Azimuth folder>\data\titles\1A96545B\saves` somewhere.
2. Unpack `Horizon-Connect-1.2.3.zip`.
3. Copy the folder `Horizon Connect` into `<Azimuth folder>\mods\`.
   `mods\Horizon Connect\mod.json` and `mods\Horizon Connect\plugins\` must be directly inside it.
4. Check your player name – the name next to the round profile icon at the top right of Azimuth's start window.
   Everyone in your group should have a different one, and it must not contain a hyphen (`-`), or the game
   shows "Player 1" instead. For a name used only by Horizon Connect, set `Gamertag=` in
   `mods\Horizon Connect\plugins\HorizonConnect.ini`.
5. Start the game. If Windows Firewall asks about `python` or `playitd`, allow it.

At the first start you get your own player id automatically. Your save so far is copied for it, so you keep your
progress; the original stays as it was.

## 3. Open the menu

In free roam, open the pause menu and press **F12** (controller: **View**). HORIZON CONNECT opens with your server list.

| Button | |
|---|---|
| A | do it (join, open) |
| B | back |
| X | edit a server |
| Y | delete a server (press twice) |
| Up / Down | choose a row |

With the Azimuth Mapper's default keyboard layout: A = Space or Enter, B = E or Backspace, X = Q, Y = R,
Up/Down = arrow keys.

Don't use F5–F9 for anything with the Azimuth Mapper running: they are its hotkeys. F6 switches its mouse pointer
mode, in which W and S no longer accelerate or brake; if that happened, press F6 once outside the menu. Another key
for the menu can be set with `Key=` under `[menu]` in `HorizonConnect.ini`.

## 4. Join a friend

1. Ask the host for their address (e.g. on Discord). It looks like `name-something.tun.ply.gg:12345`.
2. Copy it (Ctrl+C).
3. In the game: pause menu → F12 → **Add server** → A. The address you copied is already filled in → **Save**.
   (If it isn't: go to **Address** and press A.)
4. The server is now at the top of your list; on the right you see whether it is up (`online · 2 players`).
5. Choose it → A. When *"FREE ROAM SESSION FOUND – press X"* shows at the top, **press X** (within 20 s).

Next time: pause menu → F12 → A → X.

## 5. Host

The first time (once):

1. Pause menu → F12 → **Host a server** → **Set up hosting** → A.
2. Your browser opens playit.gg (free; it is how your friends reach you over the internet). Log in or sign up and
   confirm.(**Make sure your account is verified** or the game wont be able to setup a tunnel)
3. Go back to the game. After a few seconds the page shows **Start the server and join** and your address.

Every time:

1. Pause menu → F12 → **Host a server** → **Start the server and join** → A, then X at *"session found"*.
2. **Address for friends** → A copies it. Send it to your friends. It stays the same, so they add it
   only once.
3. When you are done: **Stop the server** → A, or just close the game.

Your game must keep running while you host.

## 6. Race together

1. The session leader (star in the player list) drives to a race in free roam and starts it as usual, choosing
   the event's options.
2. Everyone else gets the game's prompt that an event is starting and can join it (about 15 seconds).
3. Everyone picks a car and waits on the starting grid; the race starts for all at the same time.
4. After the race: results, rewards, and everyone drives on in free roam. The leader can start the next one.

A race ends a few seconds after the last player crossed the line, or 30 seconds after the first one. Players who
leave the race or lose the connection drop out; the others race on. Only the host needs version 1.1.0 or newer for
this.

**King and Infected** (the Playground games) start the same way. The crown or the infection passes on when cars
touch. King ends at the time limit the leader picked, Infected when everyone is infected. The host needs 1.2.0.

## 7. Troubleshooting

- **"can't be reached"** – the host hasn't started the server, or you have different Horizon Connect versions.
  When a new version is out, the server list shows it at the top: A installs it, A again restarts the game.
- **"Agent deleted" when hosting** – your playit agent was deleted on playit.gg: **Set up a new one** → A. You get a
  new address; send it to your friends. (A free playit account has room for one agent.)
- **Can't start an event** – only the session leader (star in the player list) can, and at least one other player
  must be in the session. Make someone else the leader: pause menu → online player list → the player → *Make
  session leader*.
- **Started an event and the menu is stuck** – the host has a version before 1.1.0; restart the game, the host updates.
- **Nothing happens after A / the search never ends** – try again from F12; if that doesn't help, restart the game.
- **F12 does nothing** – it works only in free roam's pause menu (not in races, the Autoshow, …).
- **The browser doesn't open when hosting** – check whether a window opened behind the game (Alt+Tab).
- **"Azimuth stopped: activation Windows.Xbox.Media.Capture.ApplicationClipCapture"** – the game tried to record a
  video clip; fixed in 1.2.1, update.
- **A player stays on the loading screen after picking a car** – their game couldn't load the event (for example a
  car from a DLC or car pack they don't have, or a very slow PC). Since 1.2.2 the host's server takes them out of the
  event after a while: they are back in free roam and the others race on. The host needs 1.2.2.
- **A player is missing from the race positions (top right) of the others** – happened when players joined a
  session that had been running for a while: the host's server sent them packets bigger than the game reads, so
  part of the other players' data never arrived. Fixed in 1.2.3; the host needs it.
- **Reporting a problem** – send `<Azimuth folder>\mods\Horizon Connect\plugins\HorizonConnect.log` (it is written anew at
  every start).

## 8. Uninstall

Delete `<Azimuth folder>\mods\Horizon Connect`. Your original save is unchanged.
