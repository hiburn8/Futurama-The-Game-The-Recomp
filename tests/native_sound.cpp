#include "platform/common/GameSoundBackend.h"
#include "core/fs/Vfs.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    fut::fs::Vfs vfs;
    std::string error;
    std::vector<fut::u8> xbe, effects;
    if (!vfs.Open(argv[1], error) || !vfs.ReadFile("default.xbe", xbe) ||
        !vfs.ReadFile("xbox/dsstdfx.bin", effects)) return 3;
    std::vector<fut::u8> format(18, 0);
    auto put16 = [&](size_t offset, uint16_t value) { std::memcpy(format.data() + offset, &value, 2); };
    auto put32 = [&](size_t offset, uint32_t value) { std::memcpy(format.data() + offset, &value, 4); };
    put16(0, 1); put16(2, 1); put32(4, 48000); put32(8, 96000); put16(12, 2); put16(14, 16);
    std::vector<int16_t> wave(2048);
    for (size_t i = 0; i < wave.size(); ++i) wave[i] = int16_t(8192 * std::sin(i * 440.0 * 6.283185307179586 / 48000));
    for (bool offline : {true, false}) {
        fut::platform::GameSoundBackend backend;
        backend.LoadXbeTables(xbe);
        backend.SetEffectsImage(effects);
        if (!(offline ? backend.InitOffline() : backend.Init())) return 4;
        backend.DownloadEffectsImage();
        const auto voice = backend.CreateBuffer(0, format);
        if (!voice) return 5;
        backend.SetBufferData(voice, reinterpret_cast<const fut::u8*>(wave.data()), uint32_t(wave.size() * 2));
        const fut::sound::MixBin bins[] = {{0, 0}, {1, 0}};
        backend.SetMixBins(voice, bins, 2);
        backend.SetFrequency(voice, 48000);
        backend.Play(voice, 0);
        backend.DoWork();
        if (offline) {
            std::vector<fut::s32> output;
            if (!backend.RenderFrames(96, output) || output.size() != 96 * 32 * 2 ||
                std::none_of(output.begin(), output.end(), [](auto sample) { return sample != 0; })) return 6;
        } else {
            const auto start = SDL_GetTicks();
            while (backend.GetStatus(voice) && SDL_GetTicks() - start < 2000) {
                SDL_Delay(10);
                backend.DoWork();
                if (!backend.Healthy()) return 7;
            }
            if (backend.GetStatus(voice)) return 8;
        }
        backend.Release(voice);
        if (!backend.Healthy()) return 9;
    }
    // Missing effects must be a visible initialization failure, never a silent fallback.
    bool refused = false;
    try {
        fut::platform::GameSoundBackend invalid;
        invalid.LoadXbeTables(xbe);
        invalid.InitOffline();
        invalid.DownloadEffectsImage();
    } catch (const std::runtime_error&) { refused = true; }
    if (!refused) return 10;
    std::cout << "PASS: nonzero offline VP/DSP PCM, SDL queue drains, voice completion, joined shutdown and bad-input refusal\n";
}
