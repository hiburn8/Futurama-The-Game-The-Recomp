#pragma once
#include <cstdint>

namespace fut::platform {
// SDL effects expire after 100 ms; native Pad only reports changed motor levels.
// Owner-thread state. The sink returns whether SDL accepted the request.
class RumbleState {
public:
    void SetDesired(std::uint16_t left, std::uint16_t right) { left_ = left; right_ = right; }
    template<class Sink> bool Tick(std::uint64_t nowMs, bool blocked, Sink&& sink) {
        const auto left = blocked ? std::uint16_t(0) : left_;
        const auto right = blocked ? std::uint16_t(0) : right_;
        if (sent_ && left == sentLeft_ && right == sentRight_ &&
            (!(left || right) || nowMs - lastMs_ < 50)) return true;
        if (!sink(left, right, (left || right) ? 100u : 0u)) return false;
        sent_ = true; sentLeft_ = left; sentRight_ = right; lastMs_ = nowMs;
        return true;
    }
    template<class Sink> bool Stop(Sink&& sink) {
        const bool ok = sink(0, 0, 0);
        sent_ = false; // The next controller submission must establish its state.
        return ok;
    }
private:
    std::uint16_t left_ = 0, right_ = 0, sentLeft_ = 0, sentRight_ = 0;
    std::uint64_t lastMs_ = 0;
    bool sent_ = false;
};
} // namespace fut::platform
