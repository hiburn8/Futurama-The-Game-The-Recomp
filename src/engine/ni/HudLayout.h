#pragma once

#include "core/Types.h"

namespace fut::ni {

// Port-only ownership, never inferred from a vertex's position or serialized.
enum class HudAnchor { Center, Left, Right };

inline f32 HudExpansion(f32 aspect) {
    return aspect > 4.0f / 3.0f ? aspect / (4.0f / 3.0f) : 1.0f;
}

// X in the original height-scaled canvas, extended past [0,1] in widescreen.
inline f32 HudAnchoredX(f32 x, HudAnchor anchor, f32 aspect) {
    const f32 extra = (HudExpansion(aspect) - 1.0f) * 0.5f;
    return anchor == HudAnchor::Left ? x - extra : anchor == HudAnchor::Right ? x + extra : x;
}

inline f32 HudCanvasToOutputX(f32 x, f32 aspect) {
    return 0.5f + (x - 0.5f) / HudExpansion(aspect);
}

}  // namespace fut::ni
