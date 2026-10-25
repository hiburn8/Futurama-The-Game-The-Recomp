// Process-wide game flags of the original's .data section (not part of any object), decompiled from default.xbe.
#pragma once

#include "core/Types.h"

namespace fut::game {

// orig: byte 0x002D6214 (1 in the image): corpses fade (Character +0x660). FsSetDecomposeCorpses 105 writes it,
// Stage::ResetObjects sets 1 (0x000CE57E); read by Character::UpdateTimers 0x00034DD2 and GenericNpc::PostUpdate
// 0x000478CB (0: +0x660 = 0). Never reset by a level load. docs/analysis/natives_batch_a_port.md 6.1.
inline u8 g_decomposeCorpses = 1;

// orig: dword 0x002DCDB8 (.bss, 0 at start): the chicken walker's last rocket launcher (0..3), advanced before each
// rocket (ChickenWalker::FireRocket 0x0004541D). Never reset (the first rocket of a process uses launcher 1).
// docs/analysis/explore_gaps_port.md 1.3.
inline s32 g_rocketSide = 0;

// orig: dword 0x002D67E0 (4 in the image): App::Frame's clear flags (renderer vf44(0, flags): 1 colour, 2 stencil,
// 4 depth). Camera::Apply 0x00012210 writes 5 with the fog on and its depth 1.0, else 4.
inline u32 g_clearFlags = 4;

}  // namespace fut::game
