// NiObject::Clone (default.xbe 0x000FE120): deep copy of a scene subtree, as the game clones definition models for
// every object it spawns. Analysis: docs/analysis/ni_clone.md.
//
// Rules: nodes, geometry and lights are new objects (name copied, flags and local transform copied, world transform /
// bound / parent reset); children keep their slots (interior empty slots kept, trailing ones dropped); properties,
// geometry data, controller data and textures are shared; extra data is deep-copied with the chain reversed;
// controllers of cloned objects are new (target = the owner's clone, keyframe / visibility key ranges recomputed);
// NiUDSNode / NiUDSObject / NiBsp clone to nothing, NiWall / NiRoom / NiRoomGroup to a plain NiNode; a skin instance
// is cloned in the geometry's ProcessClone (0x00144DF0 / 0x0010E290) when its root parent or a bone was cloned (it
// then points at the cloned bones and shares the skin data), else shared.
//
// NiCamera (0x00101050, the explosion meshes' files) and NiUVController are cloned. NOT PORTED (reported through
// `unsupported` and once on stderr, the object is left out): NiTextureEffect, NiOutline, NiScreenPolygon /
// NiScreenTexture (a camera's lists), the flip / morpher controllers and the controller classes the port does not
// load; no shipped model that the game clones contains one (every object scenario, play and render oracle).
#pragma once

#include "engine/ni/NiObjects.h"

#include <string>
#include <vector>

namespace fut::ni {

// orig: 0x000FE120. `unsupported` (optional) receives the class names of objects the port cannot clone yet.
NiPointer<NiObject> Clone(NiObject* root, std::vector<std::string>* unsupported = nullptr);
// orig: NiSequence::CreateClone 0x0012F050 (as the characters call it, outside a clone pass): name, shared text keys
// and reference index, and a new clone of every controller (no target until the sequence is linked).
NiPointer<NiSequence> CloneSequence(NiSequence& s, std::vector<std::string>* unsupported = nullptr);
// The no-controller branch of 0x0013E5E0 (a node made an outline): the name mode DAT_002F90C0 = 1, a new clone map
// DAT_002F90B0 (0x000FE0D0(37)), NiNode::CopyMembers 0x001063C0 (= 0x00105880) from `src` into `dest`, then the
// source's ProcessClone (vf24, 0x00104B70), the map deleted.
void CopyNodeMembers(NiNode& src, NiNode& dest, std::vector<std::string>* unsupported = nullptr);

}  // namespace fut::ni
