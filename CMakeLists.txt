# Futurama (Xbox, 2003) as a native Windows game: the release build, nothing else (no options, tests or
# tools). Written by the original project's tools/export/export_clean.py from its release configuration.
cmake_minimum_required(VERSION 3.25)
project(FuturamaPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()
# The static C / C++ runtime (/MT): the exe needs no VC++ redistributable, only DLLs that ship with Windows.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(fut_core STATIC
    src/core/FatalError.cpp
    src/core/XbeImage.cpp
    src/core/memory/MemoryManager.cpp
    src/core/fs/ImgArchive.cpp
    src/core/fs/Xdvdfs.cpp
    src/core/fs/Vfs.cpp
    src/game/script/ScriptVM.cpp
    src/game/script/ScriptNatives.cpp
    src/game/script/GameNatives.cpp
    src/engine/ni/NiStream.cpp
    src/engine/ni/NiObjects.cpp
    src/engine/ni/NiSceneGraph.cpp
    src/engine/ni/NiClone.cpp
    src/engine/ni/NiCollision.cpp
    src/engine/ni/NiCull.cpp
    src/engine/ni/NiOutline.cpp
    src/engine/ni/NiAnimation.cpp
    src/engine/ni/NiKeyframeManager.cpp
    src/engine/ni/NiRenderState.cpp
    src/engine/stream/StreamManager.cpp
    src/game/text/LanguageDb.cpp
    src/game/text/TextLayout.cpp
    src/game/text/TextObject.cpp
    src/game/input/Pad.cpp
    src/game/defs/Defs.cpp
    src/game/defs/CharacterDef.cpp
    src/game/defs/IngameEvent.cpp
    src/game/defs/ExplosionDef.cpp
    src/game/defs/EffectDefs.cpp
    src/game/defs/NpcDefs.cpp
    src/game/sound/GameSound.cpp
    src/game/sound/SoundEvents.cpp
    src/game/sound/SoundSystem.cpp
    src/game/level/LevelData.cpp
    src/game/level/LevelLoader.cpp
    src/game/level/LevelAnimation.cpp
    src/game/level/LightManager.cpp
    src/game/level/Intermission.cpp
    src/game/level/Session.cpp
    src/game/level/Hud.cpp
    src/game/level/Dialogue.cpp
    src/game/level/QuickSave.cpp
    src/game/level/Camera.cpp
    src/game/level/ScriptTriggers.cpp
    src/game/level/ClosestPoint.cpp
    src/game/level/Stage.cpp
    src/game/level/TriggerManager.cpp
    src/game/level/StagePreCache.cpp
    src/game/app/App.cpp
    src/game/app/Breakdown.cpp
    src/game/app/CheatCodes.cpp
    src/game/app/Contexts.cpp
    src/game/app/ErrorOverlay.cpp
    src/game/app/Extras.cpp
    src/game/app/Frontend.cpp
    src/game/app/LoadPanel.cpp
    src/game/app/MemoryCards.cpp
    src/game/app/MenuScene.cpp
    src/game/app/MoviePlayer.cpp
    src/game/app/Options.cpp
    src/game/app/OptionsPanel.cpp
    src/game/app/PauseMenu.cpp
    src/game/app/SaveStorage.cpp
    src/game/object/Emitter.cpp
    src/game/object/Explosion.cpp
    src/game/object/Spell.cpp
    src/game/object/Fire.cpp
    src/game/object/GameObject.cpp
    src/game/object/Impact.cpp
    src/game/object/Ripple.cpp
    src/game/object/RespawnEffect.cpp
    src/game/object/Splash.cpp
    src/game/object/Laser.cpp
    src/game/object/Liquid.cpp
    src/game/object/MarkInstance.cpp
    src/game/object/Character.cpp
    src/game/object/LipSync.cpp
    src/game/object/CharacterUpdate.cpp
    src/game/object/Cannon.cpp
    src/game/object/ChickenWalker.cpp
    src/game/object/PlayerControl.cpp
    src/game/object/Destructable.cpp
    src/game/object/GenericNpc.cpp
    src/game/object/SimpleNpc.cpp
    src/game/object/Moveable.cpp
    src/game/object/Pickup.cpp
    src/game/object/RatRide.cpp
    src/game/object/Static.cpp
    src/game/physics/Physics.cpp
    src/game/data/ConfigFile.cpp
    src/render/xd3d/XD3DTypes.cpp
    src/render/xd3d/XD3DDevice.cpp
    src/render/nixbox/NiXBoxGeometryBuffers.cpp
    src/render/nixbox/NiXBoxRenderer.cpp
    src/render/nixbox/NiXBoxRenderState.cpp
    src/render/nixbox/NiXBoxLightManager.cpp
    src/render/nixbox/NiXBoxTexturePipeline.cpp
    src/audio/Wav.cpp
    src/audio/xdsound/XdsMath.cpp
    src/audio/xdsound/XdsDirectSound.cpp
    src/audio/vp/VpFrontEnd.cpp
    src/audio/vp/Vp.cpp
    src/audio/vp/VpBridge.cpp
    src/audio/vp/VpStream.cpp
    src/audio/dsp/DsImage.cpp
    src/audio/dsp/Dsp56300.cpp
    src/audio/dsp/McpxGp.cpp
    src/audio/dsp/McpxEp.cpp
    src/audio/dsp/AudioDsp.cpp
    src/video/xmv/XmvImage.cpp
    src/video/xmv/Wmv2Decoder.cpp
    src/video/xmv/Wmv2Kernels.cpp
    src/video/xmv/XmvDecoder.cpp
)
target_include_directories(fut_core PRIVATE src)

# The core-only target is a portability check, not a playable game.
if(NOT WIN32)
    target_compile_definitions(fut_core PRIVATE _FILE_OFFSET_BITS=64)
    option(FUT_NATIVE_PREVIEW "Build the experimental SDL3/DXVK native host" OFF)
    if(FUT_NATIVE_PREVIEW)
        set(DXVK_ROOT "" CACHE PATH "Pinned DXVK Native usr prefix")
        set(SDL3_ROOT "" CACHE PATH "SDL3 include/library prefix")
        find_path(DXVK_INCLUDE_DIR d3d11_1.h HINTS "${DXVK_ROOT}/include/dxvk" REQUIRED)
        find_library(DXVK_D3D11_LIBRARY NAMES dxvk_d3d11 HINTS "${DXVK_ROOT}/lib" REQUIRED)
        find_library(DXVK_DXGI_LIBRARY NAMES dxvk_dxgi HINTS "${DXVK_ROOT}/lib" REQUIRED)
        find_path(SDL3_INCLUDE_DIR SDL3/SDL.h HINTS "${SDL3_ROOT}/include" REQUIRED)
        find_library(SDL3_LIBRARY NAMES SDL3 HINTS "${SDL3_ROOT}/lib" REQUIRED)
        add_executable(fut_native_preview app/linux/Main.cpp src/platform/common/MovieService.cpp
            src/render/TextureDecode.cpp src/render/xd3d/D3D11Device.cpp)
        target_include_directories(fut_native_preview PRIVATE src "${DXVK_INCLUDE_DIR}" "${SDL3_INCLUDE_DIR}")
        target_link_libraries(fut_native_preview PRIVATE fut_core "${DXVK_D3D11_LIBRARY}" "${DXVK_DXGI_LIBRARY}" "${SDL3_LIBRARY}")
        set_target_properties(fut_native_preview PROPERTIES OUTPUT_NAME FuturamaRecomp)
    endif()
    include(CTest)
    if(BUILD_TESTING)
        add_executable(fut_pal_audio_test tests/pal_audio.cpp)
        target_link_libraries(fut_pal_audio_test PRIVATE fut_core)
        target_include_directories(fut_pal_audio_test PRIVATE src)
        set(FUT_PAL_DATA "" CACHE PATH "Optional owned PAL disc folder for audio data checks")
        if(FUT_PAL_DATA)
            add_test(NAME pal_audio_binding COMMAND fut_pal_audio_test "${FUT_PAL_DATA}")
        endif()
        add_executable(fut_pal_image_test tests/pal_image.cpp src/core/XbeImage.cpp)
        target_include_directories(fut_pal_image_test PRIVATE src)
        set(FUT_PAL_XBE "" CACHE FILEPATH "Optional owned PAL/EU image for data binding checks")
        add_executable(fut_pal_movie_test tests/pal_movie.cpp)
        target_link_libraries(fut_pal_movie_test PRIVATE fut_core)
        target_include_directories(fut_pal_movie_test PRIVATE src)
        set(FUT_PAL_MOVIE "" CACHE FILEPATH "Optional owned PAL XMV file for native decode checks")
        if(FUT_PAL_XBE AND FUT_PAL_MOVIE)
            add_test(NAME pal_movie_binding COMMAND fut_pal_movie_test "${FUT_PAL_XBE}" "${FUT_PAL_MOVIE}")
        endif()
        if(FUT_PAL_XBE)
            add_test(NAME pal_image_binding COMMAND fut_pal_image_test "${FUT_PAL_XBE}")
        endif()
        add_executable(fut_disc_reader_test tests/disc_reader.cpp src/core/fs/Xdvdfs.cpp)
        target_include_directories(fut_disc_reader_test PRIVATE src)
        target_compile_definitions(fut_disc_reader_test PRIVATE _FILE_OFFSET_BITS=64)
        add_test(NAME disc_reader_large_offsets COMMAND fut_disc_reader_test
                 "${CMAKE_CURRENT_BINARY_DIR}/synthetic-disc.bin")
    endif()
    return()
endif()

add_library(fut_render STATIC
    src/platform/win32/Window.cpp
    src/platform/win32/GamepadInput.cpp
    src/platform/win32/Launcher.cpp
    src/platform/win32/XAudio2SoundBackend.cpp
    src/platform/common/MovieService.cpp
    src/render/TextureDecode.cpp
    src/render/xd3d/D3D11Device.cpp
)
target_include_directories(fut_render PRIVATE src)
target_link_libraries(fut_render PUBLIC fut_core)

add_executable(fut_game
    app/game/GameMain.cpp
)
set_target_properties(fut_game PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME FuturamaRecomp)
if(MSVC)
    target_link_options(fut_game PRIVATE /ENTRY:mainCRTStartup)
endif()
target_include_directories(fut_game PRIVATE src)
target_link_libraries(fut_game PRIVATE fut_core fut_render d3d11 dxgi d3dcompiler xinput xaudio2)
