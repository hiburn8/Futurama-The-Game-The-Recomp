# Native Linux preview

This experimental host uses SDL3 for its window and controller and DXVK Native
for D3D11 on Vulkan. It does not require a Wine runtime. Movie and audio services
share a native clock. The existing Xbox VP/GP/EP mixer feeds a bounded SDL audio
stream; Windows retains XAudio2. A successful build is not gameplay acceptance.
The Windows target remains available through the existing build configuration.

## Dependencies

Use a C++20 compiler and CMake 3.25 or newer. The initial build used:

| Dependency | Version / source | SHA-256 of source or distribution archive |
|---|---|---|
| DXVK Native | [3.1.1 Steam Runtime sniper](https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz) | `506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7` |
| SDL3 | release-3.4.18, commit `829a65d769d935c4852f8159e964312c0957260a` | `5c15cf07829599086f40fd6dcb19d4ba09027635f42567ee7e5d43ef4f36df78` |
| vkd3d shader compiler | [2.0 source](https://dl.winehq.org/vkd3d/source/vkd3d-2.0.tar.xz) | `9ad29bb236808186a47ec66e853b21e6fca59b9dc62a9474b05d5e3eda2710ef` |

DXVK_ROOT points to the extracted `usr` directory containing `include/dxvk` and
`lib`. SDL3_ROOT points to an SDL3 development prefix. The SDL3 runtime must also
be installed. Use the native vkd3d compiler with HLSL input support.

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
  -DFUT_NATIVE_PREVIEW=ON -DDXVK_ROOT=/path/to/dxvk/usr \
  -DSDL3_ROOT=/path/to/sdl3 \
  -DFUT_PAL_XBE=/path/to/owned/default.xbe \
  -DFUT_PAL_DATA=/path/to/owned/game-data \
  -DFUT_PAL_MOVIE=/path/to/owned/video/1-1-00-PAL.xmv
cmake --build build-linux --parallel 2
ctest --test-dir build-linux --output-on-failure
```

The PAL image is optional for building, but required to run the PAL binding test.
The optional data folder enables audio-table/effects tests and the movie path
enables the first-packet decoder smoke test. Inputs must be supplied locally and
must never be committed. See [PAL-DATA.md](PAL-DATA.md)
for the accepted image and the limits of the current table bindings.

With the native preview enabled, CTest also runs `native_sound_pipeline` against
the owned data folder using SDL's dummy driver. It checks nonzero offline PCM,
queue consumption, voice completion, joined shutdown and missing-effects refusal.
The dummy driver is set only for that test. Physical output, complete movies and
A/V synchronization require separate device checks.

Compile the renderer's four HLSL entry points to DXBC before launching:

```sh
python3 tools/compile_shaders.py --compiler /path/to/vkd3d-compiler \
  --out /path/to/generated/shaders
```

Point `--shaders` at that output directory.

```sh
LD_LIBRARY_PATH=/path/to/dxvk/usr/lib "$PWD/build-linux/FuturamaRecomp" \
  /path/to/owned/game-data --shaders /path/to/generated/shaders \
  --saves /path/to/new-scratch-directory --frames 300
```

For `--saves`, the directory must not exist and its parent must exist. A later
process can explicitly use `--resume-saves` with that same directory. Resume requires
the preview marker, refuses symlink roots/markers and locks the directory against
concurrent preview writers. It is a test-session boundary, not a player-data migration
or a guarantee of game-save durability. This preview deliberately
uses fresh scratch saves. Do not use existing player saves: upstream's save signature
is not compatible with original Xbox saves. `--frames` bounds a smoke run; omit it
for an interactive run. A controller is currently required for game input.

Before proposing full Linux/PAL support, verify native rendering, controller
disconnect/reconnect, sound, movies, PAL languages, progress save/load and clean exit.
Also build the Windows target to check for regressions. These checks are still pending.

Save-store opening now refuses filesystem and file-open errors instead of treating
an unreadable store as an empty card. `save_storage_read_errors` verifies invalid
roots and a normal synthetic save/reopen. Write completion, atomic publication and
crash durability still need separate work before native persistence acceptance.
