// Closest point on a triangle (orig: 0x00011CC0), used by the camera's sphere push-out 0x000129C0
// (docs/analysis/camera.md section 2.5). Verified bit-exact against the original with tools/difftest/geom_oracle.py.
//
// Behaviour of the original, kept as is:
//   * When the projection of p onto the triangle's plane lies inside the triangle (barycentric test), p is left
//     UNCHANGED - it is not projected onto the plane.
//   * Vertex regions: p becomes the vertex when both edge dot products from that vertex are negative.
//   * Edge regions: the edge is picked by the smallest of (p-v0).(p-v1), (p-v1).(p-v2), (p-v0).(p-v2), and the result
//     is vi + e * (e . p) with e the unit edge direction - the projection uses the ABSOLUTE point p, not p - vi, so
//     the result is only on the edge's line when vi . e == 0.
//   * A degenerate edge (length <= 1e-6) unitizes to zero, the result is then that edge's start vertex.
// The function returns nothing (void; EAX holds garbage).
#pragma once

#include "engine/ni/NiMath.h"

namespace fut::game {

// tri: 3 consecutive points v0, v1, v2. Modifies p in place.
void ClosestPointOnTriangle(ni::NiPoint3& p, const ni::NiPoint3 tri[3]);   // orig: 0x00011CC0

}  // namespace fut::game
