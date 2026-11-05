# Futurama (Xbox, 2003) as a native Windows / Linux game: the release build, nothing else (no options, tests or
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
    # no fused multiply-adds (MSVC makes none), so the floats stay as on the Xbox; 64-bit file offsets for the ISO
    add_compile_options(-Wall -Wextra -ffp-contract=off)
    add_compile_definitions(_FILE_OFFSET_BITS=64)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--disable-new-dtags")   # (DT_RPATH: transitive)
endif()
# The static C / C++ runtime (/MT): the exe needs no VC++ redistributable, only DLLs that ship with Windows.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# SDL3 (the window, input and clock layer), linked statically: a system SDL3 when CMake finds one, else the pinned
# release fetched at configure time.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    if(WIN32)                                         # static: the one-exe release
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    else()                                            # shared: DXVK-native dlopens libSDL3.so.0 (the same SDL)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

if(NOT WIN32)
    set(FUT_DXVK_ROOT "" CACHE PATH "DXVK-native prefix (the release tarball's usr directory)")
    find_path(DXVK_DIRECTX_INCLUDE_DIR d3d11_1.h HINTS "${FUT_DXVK_ROOT}/include/dxvk" REQUIRED)
    find_path(DXVK_WINDOWS_INCLUDE_DIR windows_base.h HINTS "${FUT_DXVK_ROOT}/include/dxvk" REQUIRED)
    find_library(DXVK_D3D11_LIBRARY NAMES dxvk_d3d11 HINTS "${FUT_DXVK_ROOT}/lib" "${FUT_DXVK_ROOT}/lib64" REQUIRED)
    find_library(DXVK_DXGI_LIBRARY NAMES dxvk_dxgi HINTS "${FUT_DXVK_ROOT}/lib" "${FUT_DXVK_ROOT}/lib64" REQUIRED)
endif()

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
    src/game/app/LanguageSelect.cpp
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

add_library(fut_render STATIC
    src/platform/sdl/Window.cpp
    src/platform/sdl/GamepadInput.cpp
    src/platform/sdl/Launcher.cpp
    src/platform/sdl/SdlSoundBackend.cpp
    src/platform/sdl/MovieService.cpp
    src/render/TextureDecode.cpp
    src/render/xd3d/D3D11Device.cpp
)
target_include_directories(fut_render PRIVATE src)
if(NOT WIN32)
    target_include_directories(fut_render PUBLIC "${DXVK_DIRECTX_INCLUDE_DIR}" "${DXVK_WINDOWS_INCLUDE_DIR}")
endif()
target_link_libraries(fut_render PUBLIC $<IF:$<BOOL:${WIN32}>,SDL3::SDL3-static,SDL3::SDL3-shared> fut_core)

add_executable(fut_game
    app/game/GameMain.cpp
)
set_target_properties(fut_game PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME FuturamaRecomp)
if(MSVC)
    target_link_options(fut_game PRIVATE /ENTRY:mainCRTStartup)
endif()
target_include_directories(fut_game PRIVATE src)
if(WIN32)
    target_link_libraries(fut_game PRIVATE SDL3::SDL3-static fut_core fut_render winmm imm32 version setupapi dinput8 d3d11 dxgi xinput)
else()
    target_link_libraries(fut_game PRIVATE SDL3::SDL3-shared fut_core fut_render "${DXVK_D3D11_LIBRARY}" "${DXVK_DXGI_LIBRARY}")
endif()
if(NOT WIN32)
    install(TARGETS fut_game RUNTIME DESTINATION .)
    foreach(lib "${DXVK_D3D11_LIBRARY}" "${DXVK_DXGI_LIBRARY}")
        get_filename_component(real "${lib}" REALPATH)
        get_filename_component(name "${lib}" NAME)
        install(FILES "${real}" DESTINATION . RENAME "${name}.0")
    endforeach()
    if(TARGET SDL3-shared)
        install(TARGETS SDL3-shared LIBRARY DESTINATION . NAMELINK_SKIP)
    endif()
endif()
