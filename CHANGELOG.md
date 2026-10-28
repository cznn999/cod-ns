# Changelog

## Call of Duty NX 0.30 / Call of Duty UO NX 0.20

- Fixed saving in both games: every save after the first one failed with
  "Insufficient free disk space", and the save at the end of a level crashed the
  game (e.g. at the door at the end of the first Stalingrad mission).

## Call of Duty NX 0.21 / Call of Duty UO NX 0.10

### Call of Duty NX 0.21 (`cod1.nro`)

- Brightness slider works: the port applies the game's gamma ramp to every frame,
  as the PC's graphics card does.
- The logo videos at start-up are skipped; the cutscenes between missions still play.
- Shown in hbmenu as "Call of Duty NX", with its version.
- Game files go directly into `gamedata/`; `tools/check_game_files.py` checks them
  before building; clearer build instructions in the README.
- Builds configured with `-DCOD_PROFILE=ON` write their profile to the SD card.

### Call of Duty UO NX 0.10 (`cod1uo.nro`)

- First version of the United Offensive port: build with `-DCOD_GAME=uo`.
- Needs United Offensive from Steam, installed in the same folder as Call of Duty.
- Bink videos are skipped for now.
- Its own settings and log on the SD card (`args_uo.txt`, `env_uo.txt`,
  `cod1_uo.cfg`, `cod1_uo.log`), next to Call of Duty's; both games share the
  `pc` game folder.

## Call of Duty NX 0.10

- First version of the Nintendo Switch port of the Call of Duty campaign.
