// The render data on the scene graph (docs/analysis/renderer_lighting_port.md 1; the classes are in NiObjects.h):
// the property type of a property and the NiDynamicEffect id allocator.
#pragma once

#include "core/Types.h"

#include <functional>
#include <map>
#include <vector>

namespace fut::ni {

class NiProperty;
class NiDynamicEffect;

// orig: NiProperty vtbl+0x2C (0 alpha .. 11 z-buffer; 0xFFFFFFFF for a class without a type).
u32 PropertyType(const NiProperty& p);

// The NiDynamicEffect ids (+0xC4, the D3D light index): orig: 0x00141910 (ctor) takes the free list's last id, else
// the counter 0x002D7CAC (starting at 1); 0x00141B80 (dtor) gives the id back. Every effect the original creates
// before one shifts its id: the effects of the unported front end / pause menu are not created by the port, so a
// test reproduces the state the oracle implies with Reset (renderer_lighting_port.md 11.1).
namespace effect_ids {
u32 Acquire();
void Release(u32 id);
void Reset(u32 nextId, const std::vector<u16>& freeList);
extern u32 next;
extern std::vector<u16> freeList;
}  // namespace effect_ids

}  // namespace fut::ni
