// NiAVObject::Update(t, 1) as the game objects call it (default.xbe 0x001018D0).
#pragma once

#include "engine/ni/NiObjects.h"

namespace fut::game {

// orig: NiAVObject::Update(t, 1) 0x001018D0: the downward pass (vf64), then, with a parent, the parent's vfB0 (slot
// 44), which is the empty 0x000B0420 in every NiNode class of the game (NiNode 0x002A6848, NiBillboardNode, NiRoom,
// NiRoomGroup, NiWall, ...: renderer_plan.md 11). The ancestors' world bounds are NOT refreshed; LevelAnimation's
// PoseTarget walks them up to the room explicitly (0x000BA929..), Update does not.
inline void UpdateNodeAt(ni::NiAVObject* o, f32 t) { o->UpdateDownwardPass(t, true); }

}  // namespace fut::game
