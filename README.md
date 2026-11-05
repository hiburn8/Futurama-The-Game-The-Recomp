![Fry at his PC, launching Futurama: The Game Recomp](docs/header.png)

# Futurama: The Game: The Recomp

**Good news, everyone!** The 2003 Futurama game for the original Xbox now runs natively on Windows. No emulator,
just a real Windows build.

I rebuilt the whole game in C++, one function at a time, straight from the original Xbox code (the `// orig:` comments point back to where each bit lives in the original).
Graphics go through Direct3D 11, sound through XAudio2, and you can play with an Xbox, DualShock, Switch controller or
keyboard.

The aim was 1:1 with the original. Same levels, same physics, same scripts, same timing, even the same random
numbers, frame for frame. It's not a remake or a remaster. It's the same game, it just runs outside of the PS2/Xbox now.
Automated full playthroughs, replayed on the port and on the original running in an emulator, confirm it is
frame-perfect end to end: all 752,000 frames of the game match.

It's not an automatic translation of the disc either. All of the game's code is right here in this repo, written out
in C++, and it's what gets compiled into the exe. Nothing from the disc's executable gets translated or run as game
code. The disc is only read for the stuff that can't be shared: the models, textures, sounds, music, movies, level
scripts, text, and some data tables stored inside the original `default.xbe`.

### You'll need your own copy

There's no game data in here. You need your own Xbox copy of Futurama, either the North American (NTSC-U) or the
European (PAL) one, as a disc image (`.iso`) or the extracted files. All the models, textures, sounds, movies and
scripts get loaded from that when you play.

## Playing

The easy way: grab the zip from the [Releases](../../releases) page, unzip it anywhere and double-click
`FuturamaRecomp.exe`. The first time, it asks for your disc image (pick your `.iso`, or the `default.xbe` inside your
extracted folder) and remembers it from then on. Put the `.iso` (or a `futurama` folder with the extracted files)
right next to the exe and it finds it by itself. Windows 10 or 11, 64-bit, nothing else to install.

Built it yourself? Double-click works the same, or point it straight at your disc:

```
build\Release\FuturamaRecomp.exe <your futurama.iso or extracted folder>
```

It starts full screen at your screen's resolution, 4:3 with black bars; Alt+Enter toggles a window. Options:

| Option | Effect |
| --- | --- |
| `--windowed` | start in a window (1280x960) |
| `--size 1920x1080` | the window size |
| `--render-scale 1` | the original 640x480 picture scaled up (`2` = 1280x960, default `auto` = your screen) |
| `--filter nearest` | crisp unfiltered pixels instead of smooth scaling |
| `--saves <folder>` | where saves go (default `%APPDATA%\FuturamaRecomp\saves`) |

An Xbox controller just works, and so do most other pads (DualShock, DualSense, Switch Pro and the like), plugged in
before or during play. No controller? The keyboard does it too (the physical keys, on any layout):

| Keys | Xbox pad |
| --- | --- |
| W A S D | left stick |
| I J K L | right stick |
| arrow keys | d-pad |
| Space / E / Left Ctrl / R | A / B / X / Y |
| Q / F | White / Black |
| Z / Shift | left / right trigger |
| Enter / Esc | Start / Back |

## Building

You need Windows, Visual Studio 2022 with the "Desktop development with C++" workload, and CMake 3.25 or newer.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Have fun!

## Quality-of-life improvements

- **A language menu for the PAL disc.** The European Xbox release takes its language from the console's settings. The
  PS2 version has a language menu at start-up, and its graphics are still on the Xbox discs, so I ported it. Pick
  English, French, German, Italian or Spanish and you get that language's text, speech and movies. The NTSC disc
  only has English speech, so it stays English.
- **60 Hz on both discs.** The PAL game originally ran at 50 Hz. Here it runs at 60 Hz, like the NTSC one.
- **A few of the original's bugs fixed.** The aim is 1:1, so any bug that changes what you see or how the game plays
  stays in. The only ones I fixed are where the original's result comes down to luck (memory it never sets up) or
  would crash the Xbox.

<details>
<summary>The original's bugs and what I did with them</summary>

| Bug | Status |
|---|---|
| Dead enemies always fade away one second after dying, so longer death animations get cut off (most weasels) | Left original |
| Spawning one kind of toxic warrior can hand the other kinds its shotgun | Left original |
| Shots at some breakable objects ignore the object's rotation and can't hit one of its six sides | Left original |
| The rat ride's steering is never reset, so riding it a second time starts with the old steering | Left original |
| A script that turns a path point saves the wrong number as its angle | Left original |
| A respawned enemy can skip planning its path and walk straight at you for a moment | Fixed |
| A grenade trooper killed before it ever attacks throws its dying grenade in an unpredictable direction | Fixed |
| A respawned weasel can copy the death animation of the weasel it replaced | Fixed |
| A few enemies (the 2-5 tank, the 5-1 Destructor) start falling one frame late | Fixed |
| Resetting the player can shift the player's model out of place | Fixed |
| The lock-on marker's see-through level is random instead of half | Fixed |
| The high priest (3-6) starts with a random summon timer and warp spot | Fixed |
| A few enemies and the 4-1 rat ride can act on a sound that was never started | Fixed |
| Settings missing from a config file get random values (the game's own files never miss any) | Fixed |
| Objects start with a random count of how many frames since they were last drawn | Fixed |
| Crashes only bad data could cause (missing objects, dividing by zero, broken files, running out of memory) | Fixed |

</details>
