#include "audio/dsp/DsImage.h"
#include "audio/dsp/McpxEp.h"
#include "audio/xdsound/XdsMath.h"
#include "core/XbeImage.h"
#include "core/fs/Vfs.h"
#include <algorithm>
#include <cmath>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    fut::fs::Vfs vfs;
    std::string error;
    std::vector<fut::u8> bytes, effects;
    if (!vfs.Open(argv[1], error) || !vfs.ReadFile("default.xbe", bytes) ||
        !vfs.ReadFile("xbox/dsstdfx.bin", effects)) {
        std::cerr << "Owned PAL data including xbox/dsstdfx.bin required: " << error << '\n';
        return 3;
    }
    fut::xbe::XbeImage image;
    if (!image.Load(bytes, &error) || !image.IsPalEu()) return 4;
    fut::audio::xds::Tables tables;
    if (!tables.LoadFromXbe(bytes) || !tables.Loaded()) return 5;
    if (tables.hrtfFilters.size() != fut::audio::xds::kHrtfFilterCount * 32 ||
        tables.hrtfDefault.size() != 32 || tables.centreGain.size() != 45 ||
        tables.centreCut.size() != 45) return 6;
    for (auto index : tables.hrtfIndex) if (index >= fut::audio::xds::kHrtfFilterCount) return 7;
    for (auto gain : tables.centreGain) if (!std::isfinite(gain) || gain < 0 || gain > 1) return 8;
    fut::audio::dsp::McpxEp ep;
    if (!ep.LoadFromXbe(bytes, &error)) { std::cerr << error; return 9; }
    // A front-left impulse must remain on the left in the retail stereo downmix.
    const fut::s32 impulse = 0x100000, zero = 0;
    fut::s32 left = 0, right = 0;
    ep.Stereo(&impulse, &zero, &zero, &zero, &zero, 1, &left, &right);
    if (left <= 0 || right != 0) return 10;
    fut::audio::dsp::DsImage dsp;
    if (!dsp.Load(effects, bytes, &error) || !dsp.Loaded()) { std::cerr << error; return 11; }
    if (dsp.Executive().size() != fut::audio::dsp::DsImage::kExecutiveBytes) return 12;
    bytes.back() ^= 1;
    if (tables.LoadFromXbe(bytes) || tables.Loaded() || ep.LoadFromXbe(bytes) || ep.Loaded() ||
        dsp.Load(effects, bytes) || dsp.Loaded()) return 13;
    std::cout << "PASS: PAL HRTF/reverb, EP downmix, GP/effects image and altered-image refusal\n";
}
