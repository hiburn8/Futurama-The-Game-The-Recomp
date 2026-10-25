// The menus' scene helpers, shared by the front end (Frontend.cpp), the pause menu (PauseMenu.cpp) and the breakdown:
// the scene pre-cache (orig: 0x000CEAC0 / 0x000CE9C0), a sequence's cycle type (0x00022460), the named-node lookups
// (vf54 GetObjectByName + an RTTI walk), and the labels object (orig: new(0x10) 0x00025710 (scene node, camera), Init
// 0x00025850, the texts 0x00025970, the per-frame update 0x00025700, RemoveAll 0x000256F0, the database reload
// 0x000256D0, the release 0x00025900): a scene-mode TextObject whose strings (Language/FRONTEND.ldb's LABEL_xxx)
// hang under the scene's "text_*" nodes. docs/analysis/frontend_port.md 1.7.
#pragma once

#include "engine/ni/NiObjects.h"
#include "game/defs/Defs.h"

#include <memory>

namespace fut::ni {
class NiRenderer;
}
namespace fut::text {
class TextObject;
class LanguageDb;
}  // namespace fut::text

namespace fut::game::app {

class App;

// orig: 0x000CE9C0 (object, static): an outline (RTTI 0x0030456C) and its subtree are skipped; a node recurses over
// its child slots (+0xC4, size +0xCC; empty slots pass null); a geometry (0x0030EC0C) is pre-cached by the renderer
// (App->vf14()->vf9C(geometry, static, 0, 0, 0); a geometry with controllers is never static). 0x000CEAC0 = static 1.
void PreCacheScene(ni::NiRenderer* renderer, ni::NiAVObject* object, bool isStatic = true);

// orig: 0x00022460 (sequence, loop): every controller of the sequence (an NiUDSTimeController, RTTI 0x0030E628: all of
// them) gets the cycle type LOOP (0) or CLAMP (2) (0x0013EBA0).
void SetSequenceLoop(ni::NiSequence* seq, bool loop);

// vf54 GetObjectByName (strcmp over the subtree), then the RTTI walk (IsKindOf): null when absent or of another class.
template <class T>
T* FindAs(ni::NiAVObject* root, const char* name) {
    return root ? dynamic_cast<T*>(GetObjectByName(root, name)) : nullptr;
}

// orig: the labels object (0x10 bytes): +0 the text object, +4 the scene node, +8 the camera, +0xC the database.
struct MenuLabels {
    MenuLabels(ni::NiNode* sceneNode, ni::NiCamera* sceneCamera);   // orig: 0x00025710
    ~MenuLabels();                                    // orig: 0x00025900 (the database unloaded, the text deleted)
    void Init(App& app);                              // orig: 0x00025850
    void Build(App& app);                             // orig: 0x00025970
    void Update();                                    // orig: 0x00025700 (the text's UpdateCamera: nothing in the scene mode)
    void RemoveAll();                                 // orig: 0x000256F0
    void LoadDb(App& app);                            // orig: 0x000256D0 (Language/FRONTEND.ldb -> +0xC)
    std::unique_ptr<text::TextObject> text;           // orig: +0x00
    ni::NiPointer<ni::NiNode> node;                   // orig: +0x04
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x08
    text::LanguageDb* db = nullptr;                   // orig: +0x0C
};

}  // namespace fut::game::app
