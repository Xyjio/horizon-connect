# Notices and third-party licences

## Horizon Connect

Copyright (c) 2026 the Horizon Connect contributors.

`HorizonConnect.dll` (the code in `plugin/`) and the release package are distributed under the **GNU General Public
License v3.0** (`LICENSE`), because `plugin/menu.c` adapts GPL-3.0 code (see below).

## FH2Net – MIT

`plugin/` (except `menu.c`) and `server/` grew out of **FH2Net** by conner-mcvay
(github.com/conner-mcvay/fh2-mods), MIT licence: `docs/LICENSE-FH2Net-MIT.txt`. Those parts may also be used
under the MIT licence.

## Extra Options / EO Weather Control – GPL-3.0

`plugin/menu.c` uses the technique for native pause-menu pages (a standalone VideoOptions page with its own XAML
served from the plugin's media directory, properties registered on the page type, text lists made in the game's
own collection type, hiding the pause page behind it) from:

- **Forza Horizon 2 Extra Options** by Arthur (RokkuDayo) – github.com/RokkuDayo/ForzaHorizon2_ExtraOptions
- **EO Weather Control** – Set Time & Weather FH2 by Pantsedit, adapted for Extra Options

Both GPL-3.0.

## FH2MP – GPL-3.0

`plugin/patches.c` (`placement_patch`, `placement_hook`, `alloc_near`) ports the CreatePlacementTexture2D fallback
from **FH2MP** by Charelene (github.com/Charelene/FH2MP, `shim/src/fh2mp_mod.c`), GPL-3.0.

## Shipped in the release package (not in this repository)

| Component | Licence | File in the package |
|---|---|---|
| playit.gg agent (`playitd.exe`, playit-cloud/playit-agent) | BSD-2-Clause | `plugins/HorizonConnect/playit/LICENSE-playit.txt` |
| Python embeddable package (python.org) | PSF License | `plugins/HorizonConnect/python/LICENSE.txt` |
| pefile (erocarrera/pefile) | MIT | `plugins/HorizonConnect/server/pefile-LICENSE.txt` |

## Game

Forza Horizon 2 is © Microsoft Corporation. No part of the game is included. The server reads the message
formats it needs from the player's own copy of the game at run time.
