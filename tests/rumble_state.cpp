#include "platform/common/RumbleState.h"
#include <array>
#include <stdexcept>
#include <vector>

int main() {
    fut::platform::RumbleState state;
    std::vector<std::array<unsigned, 3>> effects;
    bool accepted = true;
    auto sink = [&](unsigned l, unsigned r, unsigned ms) {
        effects.push_back({l, r, ms}); return accepted;
    };
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("rumble regression"); };
    state.SetDesired(123, 456);
    for (unsigned now = 0; now <= 300; now += 10) require(state.Tick(now, false, sink));
    require(effects.size() == 7); // Same native request remains alive beyond SDL's 100 ms expiry.
    for (const auto& effect : effects) require(effect == std::array<unsigned, 3>{123, 456, 100});
    require(state.Tick(310, true, sink));
    require(effects.back() == std::array<unsigned, 3>{0, 0, 0});
    const auto pausedCount = effects.size();
    require(state.Tick(500, true, sink) && effects.size() == pausedCount);
    state.SetDesired(321, 654); // Native effects can change while output is blocked.
    require(state.Tick(510, false, sink)); // No new native notification on resume.
    require(effects.back() == std::array<unsigned, 3>{321, 654, 100});
    state.SetDesired(0, 0); // Host disconnect clears the old controller's request.
    require(state.Stop(sink)); // Disconnect/exit explicitly stops the old device.
    require(effects.back() == std::array<unsigned, 3>{0, 0, 0});
    require(state.Tick(511, false, sink)); // Replacement must not inherit old motor levels.
    require(effects.back() == std::array<unsigned, 3>{0, 0, 0});
    // Model the host loop: pre-Frame refresh, native Frame changes, post-Frame flush.
    require(state.Tick(512, false, sink));
    state.SetDesired(111, 222);
    require(state.Tick(512, false, sink));
    require(effects.back() == std::array<unsigned, 3>{111, 222, 100});
    state.SetDesired(0, 0);
    require(state.Tick(512, false, sink));
    require(effects.back() == std::array<unsigned, 3>{0, 0, 0});
    state.SetDesired(700, 800);
    accepted = false;
    require(!state.Tick(520, false, sink));
    accepted = true;
    require(state.Tick(521, false, sink)); // Failed submissions are not mistaken for success.
    require(effects.back() == std::array<unsigned, 3>{700, 800, 100});
    require(state.Stop(sink));
}
