# Game files go here

Copy the **contents** of your Call of Duty folder from Steam into this folder, so
that `CoDSP.exe` is `gamedata/CoDSP.exe`:

```
gamedata/
├── CoDSP.exe
├── gamex86.dll, cgamex86.dll, uix86.dll
├── main/
└── CoDUOSP.exe, uo_*.dll, uo/     (United Offensive, optional)
```

Git ignores everything here except this file, so the game files are never
committed. Check them with `python3 tools/check_game_files.py gamedata`.
