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

There's no game data in here. You need your own NTSC-U Xbox copy of Futurama, either as a disc image (`.iso`) or the
extracted files. All the models, textures, sounds, movies and scripts get loaded from that when you play.

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

What I can't fully promise yet:

- I haven't compared the final picture and sound with a real Xbox. The port sends exactly the same graphics and sound
  commands, but how they come out on actual hardware hasn't been checked.
- In a few spots the original reads memory it never set up, so what it does there depends on whatever ran before. The
  port uses fixed values instead (see [Bug fixes](#bug-fixes)), so very rarely something can happen a little
  differently, like a respawned enemy taking a different route.
- The emulator I tested against does a few maths instructions (sine, cosine and a couple of others) slightly
  differently from a real Xbox, so those could be off in the very last bit.
- Code that nothing ever reaches, like old debug features, is ported but untested.

## Bug fixes

The aim is 1:1, so the original's bugs stay in, even the ones you can see. There are just two kinds I did fix. In
both, the original's behaviour depends on luck, and fixing them doesn't change how the game plays.

### Memory the original never sets up

In a few places the original reads a value it never set, so it gets whatever an earlier object left in that memory.
What happens then depends on everything that ran before. The port starts these at zero, or at the default the code
clearly expects. The tests still feed the original's leftover values into the port, so the code around them is
still checked against the original bit for bit. These are all the ones I've found:

- **Enemy route planning.** An enemy's route planner remembers the last trip it planned, but a new enemy never clears
  that note. If a respawned enemy lands in a dead one's memory and wants the same trip, the original thinks it already
  has a route, skips planning, and walks straight at its target for a moment. In the port it always plans the route.
- **Characters' "on the ground" flags.** A new character's body starts with leftover ground and contact flags. In the
  original that made a few enemies (the level 2-5 tank, a level 3-5 shooter, the level 5-1 Destructor) count as
  standing on their first frame, so they started falling a frame late.
- **The player's reset.** Resetting the player reads a field nothing ever sets. If there's leftover data in it, the
  original treats it as an object and moves the player's model around. The port reads zero and skips that.
- **The lock-on marker.** In third person its transparency is worked out from a leftover number. In the runs I
  checked, it came out very close to half anyway. In the port it's exactly half.
- **The high priest** (level 3-6). His summon timer and last warp spot start out as leftovers. The port starts both at
  zero.
- **Sound handles.** A few enemies and the level 4-1 rat ride check sound handles they never set. The port starts them
  empty.
- **Settings a config file leaves out.** Some object and character settings get copied from leftover memory when the
  file doesn't give them. The port uses zero or the default. None of the game's own files rely on them.
- **Objects' "frames since last drawn" counters.** These start out as leftovers too. The port starts them at zero.

### Crashes and freezes

The original skips some safety checks. It reads through empty pointers, divides by zero, writes to memory that's
already been freed, and trusts sizes read from files. As far as all my testing shows, the game's own data and normal
play never trigger any of these. But if one ever did, the Xbox would crash. The port checks instead and carries on:

- a script that divides by zero gets 0;
- an enemy with no dying animation just skips it;
- a missing effect doesn't get spawned;
- damaged or oversized files get rejected instead of overflowing.

Running out of memory shows an error instead of freezing the game.

### Bugs I kept

Anything that changes what you see or how the game plays stays exactly as it was. For example:

- Spawning a regular toxic warrior writes into the settings it shares with its other versions, so they can end up with
  its shotgun.
- The hit test for health triggers ignores the box's rotation and only checks five of its six sides.
- The rat ride's steering is never reset, so a second ride starts with the last one's steering.
- Setting a waypoint's rotation from a script stores a cosine where the angle should go.

## Building

You need Windows, Visual Studio 2022 with the "Desktop development with C++" workload, and CMake 3.25 or newer.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Playing

The easy way: grab the zip from the [Releases](../../releases) page, unzip it anywhere and double-click
`FuturamaRecomp.exe`. The first time, it asks for your disc image (pick your `.iso`, or the `default.xbe` inside your
extracted folder) and remembers it from then on. Put the `.iso` (or a `futurama` folder with the extracted files)
right next to the exe and it finds it by itself. Windows 10 or 11, 64-bit, nothing else to install.

Built it yourself? Double-click works the same, or point it straight at your disc:

```
build\Release\FuturamaRecomp.exe <your futurama.iso or extracted folder>
```

Add `--size 1920x1080` to change the window size (the default is 1280x960). Saves go to
`%APPDATA%\FuturamaRecomp\saves`, or wherever you point `--saves`.

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

Have fun!
