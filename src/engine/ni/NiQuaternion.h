// NiQuaternion helpers shared by the controllers and the sequence blending (NiAnimation.cpp). x87 order as in the
// original; see NiMath.h for the floating point conventions.
#pragma once

#include "engine/ni/NiObjects.h"

namespace fut::ni {

NiMatrix3 QuatToRotation(const NiQuaternion& q);                 // orig: 0x00115670 NiQuaternion::ToRotation
NiQuaternion QuatFromRotation(const NiMatrix3& m);               // orig: 0x001519A0 NiQuaternion::FromRotation
// orig: 0x00153940. Not a true slerp: the "fast slerp" approximation (a lerp with a polynomial correction of t,
// then 0x00153880, a normalise by a tuned 1/sqrt approximation; J. Blow, "Hacking Quaternions").
NiQuaternion QuatSlerp(f32 t, const NiQuaternion& p, const NiQuaternion& q);

}  // namespace fut::ni
