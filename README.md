# FH2 Auto Backup Save Game

Automatic backups of your **Forza Horizon 2** profile on PC (Azimuth): one each time the game starts, before it loads your save, and another every few minutes while you play. A mod for the [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform).

Azimuth keeps only the newest two copies (revisions) of your profile, and the game writes a new one every time it saves. If a save goes bad, both copies can be gone within minutes. This mod keeps older ones for you.

## Installing

You need Azimuth 1.0.1 or later. Download `AutoBackupSaveGame-v1.0.0.zip` from [Releases](https://github.com/qiangqiang101/FH2AutoBackupSaveGame/releases) and extract it into the folder with `azimuth.exe`:

```
DWrite.dll                         the FH2 Mod Platform
fh2-mods\AutoBackupSaveGame.dll    this mod
```

## What it backs up

Your profile: progress, cars, credits, settings. That's the `User_9000000000000` folder in `data\titles\<title>\saves\player-1\` (`1A96545B` for Forza Horizon 2). Liveries, tunes and photos aren't included.

Backups go to `AutoBackupSaveGame\Backups\<title>\<date>_<time>\`, next to `azimuth.exe`:

```
AutoBackupSaveGame\
    AutoBackupSaveGame.ini
    Backups\1A96545B\
        2026-11-08_14-30-05\User_9000000000000\revisions\...
        2026-11-08_14-45-05\User_9000000000000\revisions\...
```

- Every revision is checked against the game's own record of it (the SHA-256 of each file in its `commit` file). A revision the game was still writing is tried again a few seconds later. One that never matches is left out (the log says so), so a backup only has revisions the game will load.
- If the save hasn't changed since the last backup, no new one is made.
- The newest 10 backups are kept, and older ones are deleted. Each is about 13 MB.

## Settings

**Pause > Settings > MODS**:

- **Auto Backup**: On / Off
- **Auto Backup: On Game Start**: On / Off
- **Auto Backup: Every**: Off, 5, 10, 15, 30 or 60 minutes

Or `AutoBackupSaveGame\AutoBackupSaveGame.ini`:

```ini
[Settings]
Enabled=1
OnStart=1
Interval=15
Keep=10
Folder=
```

- `OnStart`: back up when the game starts.
- `Interval`: minutes between backups while you play (0: none).
- `Keep`: how many backups to keep.
- `Folder`: where backups go. Leave it blank for `AutoBackupSaveGame\Backups`. A relative path is relative to the `azimuth.exe` folder, or use a full path like `D:\FH2 Backups`.

Changes in the ini apply the next time the game starts.

## Restoring a backup

1. **Close the game.**
2. Go to `data\titles\1A96545B\saves\player-1\`. To be safe, first move the `User_9000000000000` folder somewhere else rather than deleting it.
3. Copy `User_9000000000000` from the backup you want (`AutoBackupSaveGame\Backups\1A96545B\<date>_<time>\`) into `player-1`.
4. Start the game.

## Commands

Through the platform's command tool (`fh2mod.exe`, in the platform zip's `tools` folder), while the game runs:

```
fh2mod AutoBackupSaveGame backup          # back up now (if the save changed)
fh2mod AutoBackupSaveGame backup force    # back up now, even if it hasn't
fh2mod AutoBackupSaveGame status          # the settings, the backups and the last result
```

Each backup is also written to `fh2-mods\platform.log`.

## Building

You need CMake and Visual Studio 2022. The FH2 Mod Platform is a submodule:

```sh
git clone --recursive https://github.com/qiangqiang101/FH2AutoBackupSaveGame.git
cd FH2AutoBackupSaveGame
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release      # build/Release/AutoBackupSaveGame.dll
```
