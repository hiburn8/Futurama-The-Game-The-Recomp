// Compatibility name for the existing Windows host. The shared VP/DSP path
// outputs through XAudio2 on Windows and SDL3 on Linux.
#pragma once
#include "platform/common/GameSoundBackend.h"
namespace fut::platform { using XAudio2SoundBackend = GameSoundBackend; }
