// Decompiled from default.xbe 0x00065CA0-0x00067850; see ExplosionDef.h.
#include "game/defs/ExplosionDef.h"

#include <string>

namespace fut::game {

ExplosionDef::ExplosionDef(DefContext& ctx) {         // orig: 0x00067570
    name = "ExplosionDef";
    // orig: slot 1 (0x00066730): with no mesh and no stream item yet, queue the stream and load the sound.
    streamRequested = true;
    if (stream::Manager* m = stream::Manager::Current()) request = m->Create(*this, "ExplosionDef");   // 0x0006677F
    sound = ctx.loadSound ? ctx.loadSound("Sounds/Level1/Generic/Xplo-1.wav", 0) : 0;
    releaseSound_ = ctx.releaseSound;
}

ExplosionDef::~ExplosionDef() {                       // orig: 0x00067630
    // ReleaseMeshes 0x000667E0: the eleven meshes, then Sound_Release(+0x58); +0x24 (the stream request) released
    for (auto& m : meshes) m = nullptr;
    if (releaseSound_) releaseSound_(sound);
}

bool ExplosionDef::OnStream(stream::Request& r) {     // orig: 0x000668A0
    if (const char* next = OnStreamLoaded(r.state == stream::State::New ? nullptr : r.Stream())) {
        r.SetPath(next);
        return true;
    }
    request.reset();
    return false;
}

const char* ExplosionDef::OnStreamLoaded(ni::NiStream* stream) {   // orig: 0x00066820
    if (state > 0) {
        ni::NiNode* node = nullptr;
        if (stream && !stream->TopLevelLinks().empty())
            node = dynamic_cast<ni::NiNode*>(stream->ResolveLink(stream->TopLevelLinks()[0]));
        ni::NiPointer<ni::NiNode>& slot = meshes[static_cast<size_t>(state - 1)];
        slot = node;
        if (slot) {                                   // port-only check: the original dereferences it
            slot->flags = static_cast<u16>(slot->flags | ni::kNiAVHidden);
            slot->name = "ExpDef::Mesh" + std::to_string(state - 1);   // orig: sprintf("ExpDef::Mesh%i")
        }
    }
    static const char* const kFiles[] = {
        "effects\\explosns\\Ground_Explosion.nif", "effects\\explosns\\Air_Explosion.nif",
        "effects\\explosns\\Big_Air_Explosion32.nif", "effects\\explosns\\Bomb_Explosion.nif",
        "effects\\explosns\\Explosion_shockwave.nif"};
    // orig: jump table 0x000669A8. States 5-10 fall through the copies of mesh 0 in this order (7 and 8 swapped).
    switch (state) {
        case 0: case 1: case 2: case 3: case 4:
            return kFiles[state++];
        case 5: meshes[5] = meshes[0]; ++state; [[fallthrough]];
        case 6: meshes[6] = meshes[0]; ++state; [[fallthrough]];
        case 8: meshes[8] = meshes[0]; ++state; [[fallthrough]];
        case 7: meshes[7] = meshes[0]; ++state; [[fallthrough]];
        case 9: meshes[9] = meshes[0]; ++state; [[fallthrough]];
        case 10: meshes[10] = meshes[0]; ++state; [[fallthrough]];
        default:
            return nullptr;                           // orig: releases the stream item
    }
}

}  // namespace fut::game
