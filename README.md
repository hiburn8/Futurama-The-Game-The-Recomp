![Fry at his PC, launching Futurama: The Game Recomp](docs/header.png)

# Futurama: The Game: The Recomp

**Good news, everyone!** The 2003 Futurama game for the original Xbox now runs natively on Windows. No emulator,
just a real Windows build.

I rebuilt the whole game in C++, one function at a time, straight from the original Xbox code (the `// orig:` comments point back to where each bit lives in the original).
Graphics go through Direct3D 11, sound through XAudio2, and you can play with an Xbox controller or the keyboard.

The aim was 1:1 with the original. Same levels, same physics, same scripts, same timing, even the same random
numbers, frame for frame. It's not a remake or a remaster. It's the same game, it just runs on your PC now.

It's not an automatic translation of the disc either. All of the game's code is right here in this repo, written out
in C++, and it's what gets compiled into the exe. Nothing from the disc's executable gets translated or run as game
code. The disc is only read for the stuff that can't be shared: the models, textures, sounds, music, movies, level
scripts, text, and some data tables stored inside the original `default.xbe`. There's one exception: the Xbox's sound
chip has its own little DSP programs (the effects in `dsstdfx.bin` on the disc, plus a small startup program inside
`default.xbe`), and those run on a tiny built-in emulation of that audio DSP, just like they would on the console.

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

Add `--size 1920x1080` to change the window size (the default is 1280x960). `--render-scale` independently selects
the internal 4:3 rendering resolution: `1` is the original 640x480, `2` is 1280x960, `3` is 1920x1440, `4` is
2560x1920 and `4.5` is 2880x2160. Scales above 1 fit the image to the output without changing its aspect ratio,
including after a window resize. Scaled output requires at least 1280x800 and stays within a 3840x2160 pixel budget;
scale 1 preserves the original rendering and presentation. For example:

```
build\Release\FuturamaRecomp.exe <your futurama.iso or extracted folder> --size 3840x2160 --render-scale 4.5
```

For opt-in Hor+ widescreen, add `--widescreen on` and use exactly one of `1280x800`, `1920x1080`, `1920x1200`,
`2560x1440` or `3840x2160`:

```
build\Release\FuturamaRecomp.exe <your futurama.iso or extracted folder> --size 1920x1080 --widescreen on
```

Widescreen renders at the output resolution, keeps vertical framing, expands gameplay and frontend views horizontally,
and anchors the HUD to the output edges without changing its scale. Movies and 4:3 loading artwork stay aspect-correct.
Window resizes are accepted only at those five sizes. Widescreen cannot be combined with `--render-scale` above 1;
without either feature, the original 4:3 camera and presentation are unchanged.

Saves go to `%APPDATA%\FuturamaRecomp\saves`, or wherever you point `--saves`.

An Xbox controller just works. No controller? The keyboard does it too:

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

## How I made it

This took about two weeks (12-25 October 2026), with Claude Opus 5.5 on high reasoning effort doing most of the heavy
lifting. Every function was rewritten by reading and understanding the original machine code, then tested against
the real thing.

A few things that made it work:

- I used both the Xbox and PS2 versions. The Xbox version is the one I ported and tested against. The PS2 version
  still had its class and function names left in, so it was basically a map of the code.
- The original game runs in an emulator I put together on top of [Unicorn](https://www.unicorn-engine.org/), and the
  port gets compared against it frame by frame: player and camera, enemies, every script call, the random numbers,
  physics, graphics calls, sound and movies. If a single bit is off, the test fails.
- Lots of playtesting. First a bot that finds its own way around each level. Then me playing, with my inputs recorded
  and replayed on the original. And finally a lockstep mode, where the original runs right alongside the port while I
  play and every frame gets checked live.

## How close is it?

Really close. Every game function the tests reach (701 of them) matches the original. That's backed by almost 4,000
object tests, 60 level tests, 38 rendering tests, the menus, sound, movies and full playthroughs. One run goes from
the start of level 1-1 to the end and on into level 1-2, and all 3.7 million things it compares match.

I've played the whole game through, all 22 levels in order, each one carrying on from where the last ended with the
same health, lives and ammo. That's about 3.5 hours and 752,000 frames, compared one by one against the original
running in an emulator, and every frame matches. Where the original reads memory it never set up, the checks hand
the port the original's values so the two stay in step (see [Original bug fixes](#original-bug-fixes)). Internal ID
numbers the scripts give objects sometimes differ, but they don't change anything you see or play.

<details>
<summary>Per-level details</summary>

| Level | Name | Frames |
|---|---|---|
| 1-1 | Planet Express | 9,369 (~2.5 min) |
| 1-2 | Sewers | 75,143 (~21 min) |
| 1-3 | Subway | 27,999 (~8 min) |
| 1-4 | Old New York | 82,111 (~23 min) |
| 1-5 | Red Light District | 25,691 (~7 min) |
| 1-7 | Uptown | 25,539 (~7 min) |
| 1-6 | New New York | 45,736 (~13 min) |
| 2-1 | Weasel Canyon | 43,898 (~12 min) |
| 2-6 | Run, Bender, Run | 7,117 (~2 min) |
| 2-2 | The Mine Facility | 20,686 (~6 min) |
| 2-3 | Red Rock Creek | 19,462 (~5.5 min) |
| 2-4 | The Junkyard | 27,562 (~7.5 min) |
| 2-5 | Rumble in the Junkyard | 14,752 (~4 min) |
| 3-1 | Market Square | 20,383 (~5.5 min) |
| 3-2 | Left Wing | 38,276 (~10.5 min) |
| 3-3 | Right Wing | 53,185 (~15 min) |
| 3-6 | Temple Courtyard | 28,896 (~8 min) |
| 3-4 | Inner Temple | 53,104 (~15 min) |
| 4-1 | Bogad Swamp Trail | 15,855 (~4.5 min) |
| 5-1 | Bender's Breakout | 33,217 (~9 min) |
| 5-2 | Leela's Last Laugh | 46,522 (~13 min) |
| 5-3 | Fry Fights Back | 37,798 (~10.5 min) |

</details>

What I can't fully promise yet:

- I haven't compared the final picture and sound with a real Xbox. The port sends exactly the same graphics and sound
  commands, but how they come out on actual hardware hasn't been checked.
- The emulator I tested against does a few maths instructions (sine, cosine and a couple of others) slightly
  differently from a real Xbox, so those could be off in the very last bit.
- Code that nothing ever reaches, like old debug features, is ported but untested.

## Quality-of-life improvements

- **A language menu for the PAL disc.** The European Xbox release takes its language from the console's settings. The
  PS2 version has a language menu at start-up, and its graphics are still on the Xbox discs, so I ported it. Pick
  English, French, German, Italian or Spanish and you get that language's text, speech and movies. The NTSC disc
  only has English speech, so it stays English.
- **60 Hz on both discs.** The PAL game originally ran at 50 Hz. Here it runs at 60 Hz, like the NTSC one.

## Original bug fixes

The aim is 1:1, so any bug that changes what you see or how the game plays stays in. The only ones I fixed are where
the original's result comes down to luck (memory it never sets up) or would crash the Xbox.

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
