// The level object's load sequence; see Session.h.
#include "game/level/Session.h"
#include "game/GameGlobals.h"
#include "game/level/Hud.h"

#include "core/CrtString.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiObjects.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/level/Camera.h"
#include "game/level/Dialogue.h"
#include "game/level/Intermission.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/Stage.h"
#include "game/level/LightManager.h"
#include "game/object/Character.h"
#include "game/object/GenericNpc.h"
#include "game/object/Pickup.h"
#include "game/physics/Physics.h"
#include "game/script/GameNatives.h"
#include "game/script/ScriptVM.h"
#include "core/memory/MemoryManager.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::detail::F;

namespace {

void NotPortedText(const char* what);                // (below)

// orig: _ftol (truncation of the extended value; the low 32 bits of the 64-bit result)
s32 FtoL(double v) { return static_cast<s32>(static_cast<s64>(v)); }

// orig: kinds 5-7 take the first top-level object as an NiAVObject (the original reads it without a count check).
ni::NiAVObject* FirstAVObject(ni::NiStream& s) {
    return s.TopLevelLinks().empty() ? nullptr : dynamic_cast<ni::NiAVObject*>(s.ResolveLink(s.TopLevelLinks()[0]));
}

}  // namespace

Session::Session(LevelLoadOptions& options, LoadedLevel& out, stream::Manager& streams)
    : o_(options), out_(out), streams_(streams) {}

bool Session::Exists(const std::string& path) const {   // orig: a temporary NiUDSStream + IsOpen
    std::vector<u8> data;
    return o_.readFile && o_.readFile(path, data);
}

void Session::Start(const std::string& arg) {         // orig: 0x000C2340
    LevelData& level = *out_.level;
    // NOT PORTED: SoundEvents shutdown, rumble (SoundEvents::Shutdown 0x000CA980, the active pad's SetRumble(0, 0, 1)).
    // _strnicmp(arg, "Level=", 6) (0x002A33DC): a saved state "Level=<name>\n<the carry text>" (L: no caller passes one:
    // LoadLevel's App+0xAC holds at most 8 characters; frontend_panels_port.md 3.10)
    if (crt::StrNICmp(arg.c_str(), "Level=", 6) == 0) {
        const char* n = arg.c_str() + 6;
        const char* nl = std::strchr(n, '\n');        // (no NULL test in the original)
        const size_t len = nl ? size_t(nl - n) : std::strlen(n);
        level.name.assign(n, len);                    // S+0xC: new(len + 1), strncpy, the NUL
        const char* rest = nl ? nl + 1 : n + len;
        // DAT_002EAA2C = DAT_002EAA34 = strlen(rest) + 1; DAT_002EAA28 / DAT_002EAA30 = copies (the old ones freed)
        level.carry->state = std::string(rest);
        level.carry->start = std::string(rest);
    } else {
        level.name = arg;                             // S+0xC = a copy
    }
    const std::string& levelName = level.name;
    // orig: new(0x58) 0x000EFBF0(path, Ingame NiCamera (unused), 1, 1) with sprintf("images\\%s\\%s.nif",
    // TextManager::GetLanguage, S+0xC) 0x002A33C8
    loadingScreen.emplace(o_.renderer, &o_.readFile, "images\\" + o_.defContext.language + "\\" + levelName + ".nif",
                          "images\\" + o_.defContext.language + "\\default.nif");
    // orig: the "Ingame" camera object (0x760 bytes, ctor 0x00019DB0; camera.md 1.1) attaches its NiCamera to the
    // scene (first free slot, 0x0001A5F3) after keeping the scene at NiCamera +0x15C. Camera.h: what is ported.
    level.ingameCamera = new ni::NiCamera;
    level.ingameCamera->port[0] = 0.0f, level.ingameCamera->port[1] = 1.0f;   // +0x14C..+0x158 = (0, 1, 1, 0)
    level.ingameCamera->port[2] = 1.0f, level.ingameCamera->port[3] = 0.0f;
    level.ingameCamera->renderer = o_.renderer;       // +0x1A8 = App->vf14()
    level.renderer = o_.renderer;
    level.scene->AttachChild(level.ingameCamera.Get(), true);
    level.ingameCamera->UpdateProperties();           // 0x0001A61E (the camera ctor's, after its attach)
    level.ingameCamera->UpdateEffects();
    level.camera = std::make_shared<Camera>(level);
    level.camera->sceneCamera = level.ingameCamera;
    level.scriptTriggers = std::make_shared<ScriptTriggers>(level);
    level.updateStageStreaming = [this] { UpdateStageStreaming(); };
    level.changeLevel = [this](const char* name) { ChangeLevel(name); };
    script::SetNativeLevel(&level);                   // orig: the natives act on g_level
    script::SetNativeHandler(script::CallGameNative);
    level.physic = std::make_shared<phys::Physic>();  // orig: S+0x30 = new(0x28) 0x000AE9D0
    // (the sphere query's mesh scratch: freed by ~Session 0x000C83F0, allocated anew by the next session's first mesh
    // query; its address is a test hook, LevelData.h)
    // (port, KNOWN_ISSUES #36) Without the hook: the scratch block's place among the NiBsp triangle arrays that the
    // original's heap gives it in this level's load (RadixSortPtrs 0x000FA020 orders the gathered triangles by address;
    // the NiBsp arrays' default order, newer ones lower, is NiBsp::LinkObject's). A table, not a rule: the block lands
    // in whichever free hole of >= 40000 bytes the allocator finds after the load (often the one near 0x063C2000,
    // above the arrays; that is the default for the levels not listed). Taken from the original's runs of each level
    // (extracted/analysis/start_oracle: the COLLSCRATCH line, or collscratch.txt from the runs with the same COLLBSP
    // addresses); the number of arrays below the block.
    // TODO(#36): a level loaded after another one (the normal route) and level4-1 may order differently from the
    // original; open (KNOWN_ISSUES #36, "Heap layout")
    static constexpr std::pair<const char*, s32> kScratchRank[] = {
        {"level1-1", 0}, {"level1-2", 1}, {"level2-1", 0}, {"level2-2", 0}, {"level2-4", 0}, {"level2-6", 0},
        {"level3-3", 1}, {"level3-4", 0}, {"level5-1", 0}, {"level5-2", 0}, {"level5-3", 0},
    };
    s32 scratchRank = -1;
    for (const auto& [name, rank] : kScratchRank)
        if (crt::StrICmp(levelName.c_str(), name) == 0) scratchRank = rank;
    u32 scratchAddr = 0;                              // (0: the default key)
    ni::ResetMeshScratch(scratchAddr, scratchRank);
    level.physic->world.benderFeet = [&level](ni::NiPoint3& q) {   // orig: the solver's S+0x94 test (0x000AE160)
        if (!level.player || level.player->type != 3) return false;
        q = level.player->GetWorldPosition();         // orig: vf34
        return true;
    };
    effectsFileExists = Exists("Effects\\" + levelName + ".nif");   // orig: +0x198
    kind = -1;
    request_ = streams_.Create(*this, "Session", levelName);
    state = 2;                                        // loading
}

bool Session::OnStream(stream::Request& r) {          // orig: 0x000C5BD0 (this = level + 8)
    LevelData& level = *out_.level;
    DefContext& ctx = o_.defContext;
    const std::string& name = level.name;
    if (r.state == stream::State::Loaded) {           // the handler of the kind just loaded
        ni::NiStream& s = *r.Stream();
        switch (kind) {
        case 1:
            LevelData::LoadSoundCache(s, o_.services);
            break;
        case 3:
            // (creates the explosion definition and its request; then the App's 0x000E4C90 (the memory-card sounds)
            // and the pause menu's panel reset 0x000A81F0, LevelLoadOptions::resetPausePanels)
            out_.defs->LoadEffects(level, s, ctx, o_.resetPausePanels);
            level.defs = out_.defs.get();
            break;
        case 4:
            level.LoadStageStream(s);
            break;
        case 5:                                       // orig: 0x000C5E1F
            if (!level.cameraPath) {
                level.cameraPath = FirstAVObject(s);
                if (level.cameraPath) {
                    level.cameraPath->name = "Session::m_spCameraPath";
                    level.cameraPath->UpdateEffects();   // 0x000C5E74
                    level.cameraPath->UpdateProperties();
                    level.cameraPath->UpdateDownwardPass(0.0f, true);
                }
            }
            break;
        case 6:                                       // orig: 0x000C5E90 / 0x000C5F2A
        case 7:
            if (!level.backdrop) {
                level.backdrop = FirstAVObject(s);
                if (level.backdrop) {
                    AttachNoFog(*level.backdrop);
                    level.backdrop->name = "Session::m_spBackdrop";
                    level.scene->AttachChild(level.backdrop.Get(), true);
                    level.backdrop->UpdateEffects();  // 0x000C5F08 / 0x000C5FA2
                    level.backdrop->UpdateProperties();
                    level.backdrop->UpdateDownwardPass(0.0f, true);
                    level.backdropStatic = kind == 6;
                }
            }
            break;
        case 8:
            if (o_.soundLookup) o_.soundLookup(s);    // orig: SoundEvents::LoadLookup(top[0])
            break;
        case 9:
            level.defs = out_.defs.get();
            for (u32 link : s.TopLevelLinks()) {      // orig: definitions in top-level order, each then vf04
                ni::NiObject* obj = s.ResolveLink(link);
                out_.defs->CreateFromNode(obj, ctx);
                auto* net = dynamic_cast<ni::NiObjectNET*>(obj);
                if (!net) continue;
                if (level.ApplyDefsNode(*net, o_.services)) {   // "Materials" (0x000C656F) / "Instances::"
                } else if (crt::StrNICmp(net->name.c_str(), "Stage::", 7) == 0) {
                    streams_.Note("STAGE " + net->name.substr(7) + " c6546");   // (the creator's return address)
                    level.CreateStage(net->name.substr(7))->RequestStream();
                } else if (crt::StrICmp(net->name.c_str(), "Animations") == 0) {
                    if (auto* n = dynamic_cast<ni::NiNode*>(net))
                        for (const auto& c : n->Children())
                            if (c) out_.animations.CreateFromNode(*c);
                }
            }
            break;
        case 10:
            level.LoadDialogue(s, o_.services);       // (only with exactly three top-level objects)
            break;
        default:
            break;
        }
    }
    for (;;) {                                        // orig: kind++ and the jump table 0x000C7044
        ++kind;
        std::string p;
        switch (kind) {
        case 0: {                                     // scripts\<level>.Ful, read synchronously (0x002A36B8)
            std::vector<u8> file;
            if (o_.readFile && o_.readFile("scripts\\" + name + ".Ful", file) && !level.script) {
                auto* prog = static_cast<script::ScriptProgram*>(mem::OperatorNew(sizeof(script::ScriptProgram)));
                script::ProgramInit(prog);               // (the original's program is part of the Session object)
                if (script::ProgramLoad(prog, file.data(), static_cast<u32>(file.size()))) script::ProgramStart(prog);
                else std::fprintf(stderr, "SCRIPT IS NOT RUNNING!\n");   // orig: the debug printf
                level.script = prog;
            }
            continue;
        }
        case 1:
            p = "SoundCache\\" + name + ".nif";
            if (!Exists(p)) continue;
            break;
        case 2:                                       // the HUD elements (0x000C12F0; Hud.h)
            if (!level.hud) level.hud = std::make_shared<Hud>(level, o_.readFile ? o_.readFile : ReadFileFn{});
            level.hud->Build(name);
            continue;
        case 3:
            p = LevelData::EffectsFileName(name, effectsFileExists);
            break;
        case 4:
            p = "Levels\\" + name + "\\" + name + ".nif";
            break;
        case 5:
            p = "Levels\\" + name + "\\camerapathtest.nif";
            if (!Exists(p)) continue;
            break;
        case 6:
            p = "Levels\\" + name + "\\backdrop_static.nif";
            if (!Exists(p)) continue;
            break;
        case 7:
            p = "Levels\\" + name + "\\backdrop.nif";
            if (!Exists(p)) continue;
            break;
        case 8:
            p = "Sounds\\SoundLookup.nif";
            break;
        case 9:
            p = "Levels\\" + name + "\\defs.nif";
            break;
        case 10:
            p = "Levels\\" + name + "\\ingame_dlg_" + ctx.language + ".nif";
            break;
        default:                                      // 11: the Session request is done
            request_.reset();
            return false;
        }
        r.SetPath(p);
        return true;
    }
}

void Session::PreloadNeighbours(bool loadedOnly, bool twoAhead) {   // orig: 0x000C4300
    LevelData& level = *out_.level;
    Stage* cur = level.currentStage;
    if (!cur) return;
    auto find = [&](const std::string& n) { return level.FindStage(n); };   // orig: 0x000C3F70
    auto create = [&](const std::string& n, const char* site) {
        streams_.Note("STAGE " + n + " " + site);     // (Stage::Stage's return address in 0x000C4300)
        Stage* s = level.CreateStage(n);
        s->RequestStream();
        return s;
    };
    Stage* prev = nullptr;
    Stage* next = nullptr;
    if (!cur->linkStart.empty() && !loadedOnly && !(prev = find(cur->linkStart)) && !twoAhead)
        prev = create(cur->linkStart, "c436c");
    // orig: 0x000C4388-0x000C43AD (in this order; StageHasActiveCheckpoint has no side effects)
    if (!cur->linkEnd.empty() &&
        (twoAhead || !loadedOnly || !level.respawnStage || level.respawnStage == cur ||
         !level.StageHasActiveCheckpoint(cur)))
        if (!(next = find(cur->linkEnd))) next = create(cur->linkEnd, "c43e8");
    if (!prev || next || loadedOnly || prev->linkStart.empty() || find(prev->linkStart)) {
        if (!twoAhead || !next || next->linkEnd.empty() || find(next->linkEnd)) return;
        create(next->linkEnd, "c44a5");               // the two-ahead branch (level4-1)
    } else {
        if (twoAhead) return;
        create(prev->linkStart, "c4458");
    }
}

void Session::UpdateStageStreaming() {                // orig: 0x000C70C0 (docs/analysis/stage_streaming_port.md 1)
    LevelData& level = *out_.level;
    if (!level.camera) return;                        // (the original always has S+0x8C)
    const ni::NiPoint3 camPos = level.camera->position;   // (S+0x8C)+0x48
    ni::NiPoint3 probe = camPos;
    if (Player* p = level.player) {                   // S+0x94
        const f32 h = p->heightOffset;                // +0x4D0, read before vf34
        const ni::NiPoint3 w = p->GetWorldPosition(); // vf34
        probe = {w.x, F(double(h) + w.y), w.z};       // FLD h; FADD [w+4]; FSTP
    }
    const bool l41 = crt::StrICmp(level.name.c_str(), "Level4-1") == 0;
    if (level.respawnTimer > 0.0f) level.respawnTimer = F(double(level.respawnTimer) - g_frameStep);   // S+0x6C
    if (!level.currentStage) level.currentStage = level.stages.empty() ? nullptr : level.stages.front().get();
    Stage* cur = level.currentStage;
    if (!cur) return;
    PreloadNeighbours(true, l41);
    // (0x000C719B-0x000C71DA: four FindStage calls whose results are unused)
    if (!level.roomGroup) return;                     // S+0x20
    ni::NiRoom* rc = level.roomGroup->FindRoomContaining(camPos);   // 0x0011F1D0
    if (!rc) return;
    ni::NiRoom* rp = level.roomGroup->FindRoomContaining(probe);
    if (level.respawnStage == level.prevRespawnStage && level.StageHasActiveCheckpoint(cur) && cur->ContainsRoom(rc))
        return;
    Stage* sc = nullptr;                              // the LAST stage of the list containing each room
    Stage* sp = nullptr;
    for (const auto& s : level.stages) {
        if (s->ContainsRoom(rc)) sc = s.get();
        if (s->ContainsRoom(rp)) sp = s.get();
    }
    Stage* target;
    if (!l41) target = sc;
    else if (sc == sp) target = sc;
    else if (sc == level.currentStage || sp == level.currentStage) target = level.currentStage;
    else target = sp;
    if (!target) return;
    level.prevRespawnStage = level.respawnStage;      // S+0x64 = S+0x60
    level.currentStage = target;                      // S+0x5C (also when target == cur)
    PreloadNeighbours(true, l41);
    Stage* prev = target->linkStart.empty() ? nullptr : level.FindStage(target->linkStart);
    Stage* pprev = prev && !prev->linkStart.empty() ? level.FindStage(prev->linkStart) : nullptr;
    Stage* next = target->linkEnd.empty() ? nullptr : level.FindStage(target->linkEnd);
    Stage* nnext = next && !next->linkEnd.empty() ? level.FindStage(next->linkEnd) : nullptr;
    for (size_t i = 0; i < level.stages.size();) {    // list order
        Stage* s = level.stages[i].get();
        bool keep;
        if (s == target || s == next || s->IsStreamPending()) {
            keep = true;
        } else if (l41) {
            if (s == nnext) keep = true;
            else if (next) keep = !nnext && !next->IsStreamPending() && next->linkEnd.empty() && s == prev;
            else if (!target->linkEnd.empty()) keep = false;
            else keep = s == pprev || s == prev;
        } else {
            if (!next && s == prev && prev->linkStart.empty()) keep = true;
            else if (s == level.respawnStage) keep = true;
            else if (level.respawnStage == pprev && s == prev) keep = !level.StageHasActiveCheckpoint(s);   // (sic)
            else keep = false;
        }
        if (keep) {
            ++i;
            continue;
        }
        streams_.Note("UNLOAD " + s->name);
        level.DeleteStage(s);                         // orig: (s+4)->vtbl[0](1) 0x000D1180; the next one is at i
    }
}

bool Session::IsReady() const {                       // orig: 0x000C3EB0
    const LevelData& level = *out_.level;
    if (level.stages.empty()) return false;
    if (state != 4)
        for (const auto& st : level.stages)
            if (!st->IsLoaded()) return false;
    return !loadingScreen && !title;
}

void Session::LoadCheck() {                           // orig: 0x000C53D0
    loadTime = F(double(g_frameStep) + loadTime);     // orig: +0x174 += dt
    if (!streams_.AllFinished()) return;
    for (const auto& st : out_.level->stages)
        if (!st->IsLoaded()) return;
    // NOT PORTED: 0x000CA2B0(0x40).
    if (loadingScreen) loadingScreen->FadeOut(1.0f);   // orig: vf10(1.0f)
    if (!title) {                                     // orig: new(0x38) 0x000EF400("images\levels\<level>.nif")
        // (at (0.5, 0.42)); without a renderer only whether the image exists (the play tests)
        const std::string path = "images\\levels\\" + out_.level->name + ".nif";
        if (o_.renderer) {
            title.emplace(*o_.renderer, o_.readFile, path, 0.5f, 0.42f);
        } else {
            title.emplace();
            title->loaded = Exists(path);
        }
        // vf0C SetText(S+0xC, 0.5, 0.3) 0x000EF140 (TitleImage::SetText; 0x3F000000, 0x3E99999A)
        if (o_.renderer) title->SetText(o_.services.text, out_.level->name.c_str(), 0.5f, 0.3f, o_.renderer, o_.readFile);
        title->hold = 0.75f;                          // orig: vf18 SetHold(0.75f)
        title->FadeIn(1.0f);                          // orig: vf14 FadeIn(1.0f)
    }
    state = 4;
    ApplyCarryOver();                                 // orig: 0x000C5554..0x000C59A9
    if (out_.level->input) out_.level->input->SetRumbleEnabled(true, 0.0f);   // Input_SetRumbleEnabled(1, 0)
    out_.level->materialDamage = 10.0f;               // DAT_002D6218 = 0x41200000
}

// orig: the load check's carry-over apply 0x000C5554..0x000C59A9 (docs/analysis/frontend_port.md 2.6): the player-state
// text DAT_002EAA28 (FsChangeLevel's, or a loaded save's), one "key=value\n" line at a time.
void Session::ApplyCarryOver() {
    LevelData& level = *out_.level;
    CarryOver& carry = *level.carry;
    Player* P = level.player;                         // S+0x94
    if (!P || !carry.state) return;
    IngameEventSet& events = P->cdef.ingameEvents;    // (P+0x10)+0x28
    events.enabled = false;                           // 0x000E0220(0)
    if (!carry.start) carry.start = *carry.state;     // DAT_002EAA30 = a copy (DAT_002EAA34 = strlen + 1)
    P->ClearInventory();                              // 0x00050580
    std::array<s32, 5> ammo{};                        // esp+0x30..+0x40
    level.lives = 5;                                  // DAT_002D687C = 5 (before the lines)
    GameObject* equip = nullptr;                      // esp+0x18
    const char* p = carry.state->c_str();             // esp+0x2C (the text itself is not modified)
    while (*p) {
        const char* nl = std::strchr(p, '\n');        // strchr 0x00167050
        if (!nl) break;                               // (port guard: the original copies up to NULL; every line of
                                                      //  0x000C2770's text ends with "\n")
        char line[0x100];                             // esp+0x50
        size_t n = static_cast<size_t>(nl - p);
        if (n >= sizeof line) n = sizeof line - 1;    // (port guard: no length check in the original)
        std::memcpy(line, p, n);                      // strncpy 0x001672C0
        line[n] = '\0';
        p = nl + 1;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;                            // (port guard: the original writes through NULL)
        *eq = '\0';
        char* val = eq + 1;
        // the keys compared with REPE CMPSB over their length + 1 (an exact match)
        if (std::strcmp(line, "Lives") == 0) {
            level.lives = FtoL(std::strtod(val, nullptr));   // atof 0x00167609, _ftol
        } else if (std::strcmp(line, "Health") == 0) {
            P->health = P->cdef.maxHealth;            // FLD (P+0x10)+0x60; FSTP P+0x4F4 (the value is not read)
        } else if (std::strcmp(line, "Ammo") == 0) {
            if (char* bar = std::strchr(val, '|')) {
                *bar = '\0';
                const s32 v = FtoL(std::strtod(bar + 1, nullptr));   // (atof before atoi)
                const s32 slot = std::atoi(val);      // atoi 0x0016752A
                if (slot >= 0 && slot < 5) ammo[static_cast<size_t>(slot)] = v;   // (port guard: no index check)
            } else {
                ammo.fill(FtoL(std::strtod(val, nullptr)));   // every slot
            }
        } else if (std::strcmp(line, "Power") == 0) {
            P->SetChargeLevel(F(std::strtod(val, nullptr)));   // vf168(FSTP atof)
        } else if (std::strcmp(line, "Pickup") == 0 || std::strcmp(line, "Equip") == 0) {
            // (H only: no oracle's text has these lines)
            const bool isEquip = std::strcmp(line, "Equip") == 0;   // SETE [esp+0x17]
            Def* d = out_.defs ? out_.defs->Find(val) : nullptr;    // Session::FindDef 0x000C3F10
            if (!d) continue;
            if (d->IsType(8)) {                       // a weapon already owned is not spawned again
                GameObject* o = P->inventory.NextObject(nullptr);
                while (o && o->def != d) o = P->inventory.NextObject(o);   // (+0x10 == d)
                if (o) {
                    if (isEquip && !equip) {
                        equip = o;
                        if (auto* w = dynamic_cast<Weapon*>(o)) (void)w->TakeOut();   // vf64
                    }
                    continue;
                }
            }
            const ni::NiMatrix3 rot{};                // 12 dwords of 0x002D7820 (identity)
            const ni::NiPoint3 pos{};                 // esp+0x44..+0x4C
            Owner* owner = level.stages.empty() ? nullptr : level.stages.front().get();   // S+0x58 (+4)
            GameObject* o = d->Spawn(level, owner, pos, rot, nullptr);   // vf14(owner, &pos, &rot, 0)
            if (!o) {                                 // (the original dereferences it)
                NotPortedText("load check: a carried inventory object whose definition spawns nothing");
                continue;
            }
            o->SetOwner(&P->inventory);               // vf10(P+0x38)
            if (isEquip) {
                if (o->def->IsType(8) && !equip) {
                    equip = o;
                    if (auto* w = dynamic_cast<Weapon*>(o)) (void)w->TakeOut();   // vf64
                }
            } else if (o->def->IsType(8)) {
                if (auto* w = dynamic_cast<Weapon*>(o)) (void)w->PutAway();   // vf60
                ni::NiAVObject* nd = o->GetNode();    // vf30
                if (nd && nd->parent) (void)nd->parent->DetachChild(nd);   // (+0x14)->vfA4(&tmp, node); released
            }
        } else if (std::strcmp(line, "AgeSeconds") == 0) {
            (void)std::strtod(val, nullptr);          // atof; FSTP ST(0)
            playSeconds = F(std::strtod(val, nullptr));   // S+0x178 (atof again)
        } else if (std::strcmp(line, "AgeHours") == 0) {
            playHours = std::atoi(val);               // S+0x17C
        }
    }
    carry.state.reset();                              // DAT_002EAA28 freed, DAT_002EAA2C = 0
    if (equip && !dynamic_cast<Weapon*>(equip))       // (only IsType(8) objects get here: the weapons)
        NotPortedText("load check: an Equip object that is not a weapon (vfB4)");
    (void)P->SetWeapon(dynamic_cast<Weapon*>(equip)); // vfB4(equip) (NULL when none)
    for (s32 i = 0; i < 5; ++i) P->SetAmmo(i, ammo[static_cast<size_t>(i)]);   // 0x00051D50
    events.enabled = true;                            // 0x000E0220(1)
}

void Session::Update() {                              // orig: 0x000C8C30
    if (title) title->HoldTick(g_frameStep);          // orig: (S+0x120)->vf04
    if (state == 2) {
        UpdateStageStreaming();
        LoadCheck();
        return;
    }
    if (loadingScreen || title) {
        // NOT PORTED: TriggerManager::BindCameraRecords 0x000DAE50.
        if (out_.level->script) script::ProgramUpdate(out_.level->script, 0.0f);   // orig: S+0xA8, dt 0
        return;
    }
    UpdatePlay();
}

void Session::UpdatePlay() {                          // orig: 0x000C8C30, the play branch (session_update.md 2.2)
    LevelData& level = *out_.level;
    const f32 dt = g_frameStep;
    out_.animations.Advance(dt);                      // 1: LevelAnimation::Advance for each
    // 2: the AI manager update 0x00031850 (S+0x114 set): Group::Update 0x00030E30 on every group, newest first (the
    // next one is read after the update).
    for (size_t i = 0; i < level.aiGroups.size(); ++i) level.aiGroups[i]->Update(level);
    for (size_t i = 0; i < level.stages.size(); ++i) level.stages[i]->Update();   // 3: Stage::Update 0x000CDBE0
    // NOT PORTED (4): the sound listener (the scene camera, App+0xF0 vf20).
    // 5 (0x000C8D12): a backdrop that is not backdrop_static.nif (+0x2C) follows the scene camera: its translate =
    // the camera's world translate (S+0x8C +0x30 +0xA0, the last frame's), then Update(0, 1). (No null test of the
    // camera in the original: a UB guard.)
    if (level.backdrop && !level.backdropStatic && level.camera && level.camera->sceneCamera) {
        level.backdrop->local.translate = level.camera->sceneCamera->world.translate;
        level.backdrop->UpdateDownwardPass(0.0f, true);
    }
    // 6: characters, pass 0 (vf58(dt, 0)), in list order.
    for (size_t i = 0; i < level.characters.size(); ++i) level.characters[i]->Update(dt, 0);
    // 7: the level's own objects: vf08 Tick, deleted when it returns false (the next one is taken first).
    for (GameObject* o = level.NextObject(nullptr); o;) {
        GameObject* next = level.NextObject(o);
        if (!o->Tick()) delete o;
        o = next;
    }
    // 8: the physics step (Physic vf01 / vf02).
    if (level.physic) {
        level.physic->Accumulate(dt);
        level.physic->Step();
    }
    // 9: the dialogue display 0x000D7800 (DAT_002EBDAC->Update(dt): the current dialogue's tracks, the lip sync).
    if (level.dialogue) level.dialogue->Update(dt);
    // 10: characters, pass 1 (vf58(dt, 1)); false = delete (the next one is taken first).
    for (size_t i = 0; i < level.characters.size();) {
        Character* c = level.characters[i];
        if (c->Update(dt, 1)) {
            ++i;
        } else {
            delete c;                                 // (unlinks itself from the list)
        }
    }
    // 11: Camera::Update for each camera of S+0x88 (one per level).
    if (level.camera) level.camera->Update(dt);
    // 12: the HUD's elements vf01 (0x000E58E0, gated by the HUD's byte +0: 1 from its ctor 0x000E5470).
    if (level.hud) level.hud->Update();
    // NOT PORTED (13): BindCameraRecords 0x000DAE50.
    // 14: the active pad's disabled countdown (0x000C8E4A: Input_GetActivePad 0x0009B720 -> DecrementDisabled(1)
    // 0x0009B9F0, no null check): a button a script Button trigger polls stays hidden from the player for 0x14 steps.
    if (input::InputSystem* in = level.input)
        if (input::Pad* pad = in->activePad ? in->activePad : in->pads[0]) pad->DecrementDisabled(1);
    if (level.script) script::ProgramUpdate(level.script, dt);   // 15: ScriptProgram::Update(S+0xA8, dt)
    // 16: the play clocks.
    playSeconds = F(double(dt) + playSeconds);
    if (3600.0f <= playSeconds) {
        playSeconds = F(double(playSeconds) - 3600.0f);
        ++playHours;
    }
    playSeconds2 = F(double(dt) + playSeconds2);
    if (3600.0f <= playSeconds2) {
        playSeconds2 = F(double(playSeconds2) - 3600.0f);
        ++playHours2;
    }
}

namespace {

// A file's first top-level object as a NiSourceTexture (null when the file is missing or holds something else).
ni::NiPointer<ni::NiSourceTexture> LoadSourceTexture(const ReadFileFn& readFile, const std::string& path) {
    std::vector<u8> bytes;
    if (!readFile || !readFile(path, bytes)) return {};
    ni::NiStream s;
    if (!s.Load(bytes) || s.TopLevelLinks().empty()) return {};
    return dynamic_cast<ni::NiSourceTexture*>(s.ResolveLink(s.TopLevelLinks()[0]));
}

// orig: 0x000EE9A0 (screen, texture, tiles[6]): the texture's pixel data (level 0) cut into tiles of 256 / 256 / 128
// columns by 256 / 256 rows (the rows read with the source's pitch), each a new NiPixelData (0x001112F0: one level,
// the source's format and palette) in a new NiSourceTexture (0x00118690 with the preferences DAT_002D79A4 (5, 2, 3);
// the precache when DAT_002D79A0 is set: the renderers' 0x00118050). False without pixel data.
bool CutImage(LoadingScreen& ls, const ni::NiSourceTexture* tex, ni::NiPointer<ni::NiSourceTexture> out[6]) {
    if (!tex || !tex->pixelDataObj) return false;     // (+0x34)
    const ni::NiPixelData& src = *tex->pixelDataObj;
    if (src.mips.empty()) return false;               // (the original reads the arrays unchecked)
    const u32 w0 = src.mips[0].width, h0 = src.mips[0].height;
    const u32 end0 = src.mips.size() > 1 ? src.mips[1].offset : u32(src.pixels.size());   // offsets[1]
    const u32 bpp = w0 && h0 ? (end0 - src.mips[0].offset) / (h0 * w0) : 0;
    static constexpr u32 kRows[2] = {0x100, 0x100}, kCols[3] = {0x100, 0x100, 0x80};
    u32 n = 0, y = 0;
    for (u32 r = 0; r < 2; ++r) {
        const u32 h = kRows[r];
        u32 x = 0;
        for (u32 c = 0; c < 3; ++c) {
            const u32 w = kCols[c];
            ni::NiPointer<ni::NiPixelData> tile(new ni::NiPixelData);   // 0x001112F0(w, h, src +0xC, 1)
            tile->format = src.format;
            tile->palette = src.palette;              // (+0x2C, AddRef'd)
            tile->bytesPerPixel =
                tile->format.format >= 4 && tile->format.format <= 6 ? 0 : tile->format.bitsPerPixel >> 3;
            tile->mips = {{w, h, 0}};
            const size_t pitch = size_t(w) * bpp;
            tile->pixels.assign(pitch * h, 0);
            size_t at = size_t(src.mips[0].offset) + (size_t(x) + size_t(y) * w0) * bpp;
            for (u32 row = 0; row < h; ++row) {
                for (size_t b = 0; b < pitch; ++b)    // (UB-avoiding: bytes past the source read as 0)
                    tile->pixels[row * pitch + b] = at + b < src.pixels.size() ? src.pixels[at + b] : u8(0);
                at += pitch + (size_t(w0) - w) * bpp; // (w0 < w: the original's pointer steps back)
            }
            // (+0x48 revision ++)
            ni::NiPointer<ni::NiSourceTexture> t(new ni::NiSourceTexture);   // 0x00118690(tile, (5, 2, 3))
            t->pixelLayout = 5, t->mipMapFormat = 2, t->alphaFormat = 3;
            t->hasPixelData = true;
            t->pixelDataObj = tile.Get();
            if (ni::g_precacheTextures) ni::PrecacheTexture(*t);   // (0x00118690 -> 0x00118050)
            out[n++] = t;
            x += w;
        }
        y += h;
    }
    ls.fadeLength = -1.0f;                            // +0x14 = +0x10 = -1.0f
    ls.fadeTime = -1.0f;
    return true;
}

constexpr ni::NiColorA kWhite{1.0f, 1.0f, 1.0f, 1.0f};   // DAT_002D7BEC

}  // namespace

LoadingScreen::LoadingScreen(ni::NiRenderer* renderer, const ReadFileFn* readFile, const std::string& path,
                             const std::string& defaultPath) {   // orig: 0x000EFBF0 -> 0x000EF710
    if (!renderer) return;
    ni::NiPointer<ni::NiCamera> cam(new ni::NiCamera);   // new(0x250) 0x001009D0: frustum / port defaults, no scene
    Base(renderer, readFile, path, defaultPath, cam.Get());
    Derived(readFile);
}

LoadingScreen::LoadingScreen(ni::NiRenderer* renderer, const ReadFileFn* readFile, const std::string& path,
                             const std::string& defaultPath, ni::NiCamera* sharedCamera) {   // orig: 0x000EF710 alone
    spinner = false;                                  // (no derived part: no spinner)
    if (!renderer || !sharedCamera) return;
    Base(renderer, readFile, path, defaultPath, sharedCamera);
}

LoadingScreen::~LoadingScreen() {                     // orig: the base dtor 0x000EFAD0
    if (attached && camera) {                         // +4: the tiles off the camera (0x001002D0)
        attached = false;
        for (auto& t : tiles) camera->RemoveScreenTexture(t.Get());
    }
    // the tiles released; DAT_002EFDDC = 3 (L: a texture loader's mode); the camera released (the derived class's
    // own camera with it, a shared one stays with its owner)
}

void LoadingScreen::Base(ni::NiRenderer* renderer, const ReadFileFn* readFile, const std::string& path,
                         const std::string& defaultPath, ni::NiCamera* cam) {   // orig: 0x000EF710(path, cam, 1)
    camera = cam;                                     // +0x18 (AddRef)
    if (!camera->renderer) camera->renderer = renderer;   // +0x1A8 was 0: App->vf14()
    // the image: the path's; when its stream fails to load, the language's default (TextManager::FormatLocalized
    // "images\%s\default.nif")
    ni::NiPointer<ni::NiSourceTexture> image;
    if (readFile) {
        std::vector<u8> probe;
        image = LoadSourceTexture(*readFile, (*readFile)(path, probe) ? path : defaultPath);
    }
    (void)renderer->TargetWidth();                    // vf54 / vf58 (the results are not used)
    (void)renderer->TargetHeight();
    ni::NiPointer<ni::NiSourceTexture> cut[6];
    CutImage(*this, image.Get(), cut);                // 0x000EE9A0
    for (s32 i = 0; i < 6; ++i) {
        tiles[i] = new ni::NiScreenTexture(cut[i].Get());   // new(0x20) 0x0010B2E0
        // (H) the width and height arguments are top + 256 and left + 256 (as in the original)
        const s16 top = s16((i / 3) << 8), left = s16((i % 3) << 8);
        tiles[i]->AddRect(top, left, u16(top + 0x100), u16(left + 0x100), 0 /* +0xC */, 0, kWhite);
        tiles[i]->texturing->applyMode = 2;           // (+0x14)+0x18 = 2 (modulate)
        tiles[i]->dirty |= 0xF;
    }
    if (!attached) {                                  // the 3rd argument 1: +4 = 1, the tiles on the camera
        attached = true;
        for (auto& t : tiles) camera->AddScreenTexture(t.Get());   // 0x0002BE40
    }
    camera->UpdateDownwardPass(0.0f, true);           // Update(0, 1) 0x001018D0 (the flag argument 1)
    camera->UpdateWorldBound();
    // (0x000B0420 on DAT_002E8DF4: nothing; +8 = 0)
    time = 0;
}

void LoadingScreen::Derived(const ReadFileFn* readFile) {   // orig: 0x000EFBF0 after the base
    // the derived part: the spinner's frames (images\slrm%02d.nif: a top-level NiSourceTexture each), its screen
    // texture at (top, 540) 64 x 128 (top 0x145 when App +0xE8 (NTSC) is set, else 0x15E: the port runs NTSC)
    for (s32 i = 0; i < 6; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "images\\slrm%02d.nif", static_cast<int>(i));   // 0x002A587C
        if (readFile) spinnerFrames[i] = LoadSourceTexture(*readFile, name);
    }
    spinnerFrame = 0;                                 // +0x50
    spinnerTexture = new ni::NiScreenTexture(spinnerFrames[0].Get());   // frames[+0x50]
    spinnerTexture->AddRect(0x145, 0x21C, 0x40, 0x80, 0, 0, kWhite);
    spinnerTexture->texturing->applyMode = 2;
    if (spinner) camera->AddScreenTexture(spinnerTexture.Get());
}

void LoadingScreen::Update(f32 dt) {                  // orig: 0x000EED90
    time = F(double(dt) + time);
    // the fade's colour (1 - t / length, alpha 1) into a rect's colour with its dirty bit 4
    auto shade = [](ni::NiScreenTexture* t, f32 v) {
        if (!t || t->rects.empty()) return;           // (the original writes the first rect unchecked)
        ni::NiColorA& c = t->rects[0].color;          // ((+0x1C..)+8)+0xC
        c.a = 1.0f;
        c.r = v, c.g = v, c.b = v;
        t->dirty |= 4;
    };
    if (IsFading()) {
        const double t = double(dt) + fadeTime;       // (compared unrounded, x87)
        fadeTime = double(fadeLength) < t ? fadeLength : F(t);
        const f32 v = F(1.0 - double(fadeTime) / fadeLength);   // [0x00293A98] = 1.0f
        for (auto& tile : tiles) shade(tile.Get(), v);
    }
    if (spinner) {
        ++spinnerFrame;                               // +0x50; the frame ((+0x50 >> 3) % 6) into +0x4C's base map
        if (spinnerTexture) {
            spinnerTexture->texturing->SetBaseTexture(spinnerFrames[(spinnerFrame >> 3) % 6].Get());
            spinnerTexture->dirty |= 2;
            if (IsFading()) shade(spinnerTexture.Get(), F(1.0 - double(fadeTime) / fadeLength));
        }
    }
    if (camera) ni::VisitForCulling(*camera, nullptr);   // 0x00100CE0 (+0x15C: no scene)
}

TitleImage::TitleImage(ni::NiRenderer& renderer, const ReadFileFn& readFile, const std::string& path, f32 px,
                       f32 py)
    : x(px), y(py) {                                  // orig: 0x000EF400
    camera = new ni::NiCamera;                        // +0x20 = new(0x250) 0x001009D0
    camera->renderer = &renderer;                     // App->vf14()
    // (DAT_002EFDD8 = 1 around the load: L, the texture loader's flag)
    ni::NiPointer<ni::NiSourceTexture> tex = LoadSourceTexture(readFile, path);
    if (!tex) return;
    const ni::NiPixelData* pd = tex->pixelDataObj.Get();
    // vf2C / vf30: the texture's renderer data's size, else its pixel data's (L: no texture renderer data in the port)
    const u32 w = pd && !pd->mips.empty() ? pd->mips[0].width : 0;
    const u32 h = pd && !pd->mips.empty() ? pd->mips[0].height : 0;
    const s32 W = s32(renderer.TargetWidth()), H = s32(renderer.TargetHeight());   // vf54 / vf58
    image = new ni::NiScreenTexture(tex.Get());       // new(0x20) 0x0010B2E0
    // FILD W; FMUL x; FILD w; FMUL 0.5 [0x00293AA0]; FSUBP; _ftol (the same for y)
    const s32 left = s32(double(W) * x - double(u16(w)) * 0.5f);
    const s32 top = s32(double(H) * y - double(u16(h)) * 0.5f);
    image->AddRect(s16(top), s16(left), u16(w), u16(h), 0, 0, kWhite);
    image->texturing->applyMode = 2;
    image->dirty |= 0xF;
    camera->AddScreenTexture(image.Get());            // 0x0002BE40
    loaded = true;
}

void TitleImage::FadeOut(f32 length) {                // orig: 0x000EF1F0
    fadeLength = length;                              // +0xC
    fadeTime = 0;                                     // +0x10
    direction = -1.0f;                                // +8
    alpha = 1.0f;                                     // +0x14
    if (!image || image->rects.empty()) return;       // ((+0x24)+8)+0xC: the first rect's colour
    image->rects[0].color = ni::NiColorA{1.0f, 1.0f, 1.0f, 1.0f};
    image->dirty |= 4;
}

void TitleImage::FadeIn(f32 length) {                 // orig: 0x000EF230
    fadeLength = length;
    fadeTime = 0;
    direction = 1.0f;
    alpha = 0;
    if (!image || image->rects.empty()) return;
    image->rects[0].color = ni::NiColorA{1.0f, 1.0f, 1.0f, 0.0f};
    image->dirty |= 4;
}

void TitleImage::SetText(const text::TextManager* tm, const char* key, f32 px, f32 py, ni::NiRenderer* renderer,
                         const ReadFileFn& readFile) {   // orig: 0x000EF140
    if (!image) return;                               // +0x24
    if (!text) {                                      // +0x34: TextObject::Create(0), SetRenderer(App->vf14())
        text.reset(text::TextObject::Create(0, readFile, renderer));
        if (!text) return;
        text->SetRenderer(renderer);
        text->style.hAlign = 0;                       // +0x60
        text->style.vAlign = 3;                       // +0x64
        text->style.wordWrap = false;                 // +0x68
        text->SetZBufferTest(false);                  // 0x00099660(0)
        text->SetScale(1.0f, 1.5f);                   // (0x3F800000, 0x3FC00000)
    }
    text->RemoveAll();                                // 0x00099080
    label = nullptr;                                  // +0x30
    if (!key) return;
    label = tm ? tm->GetText(key, nullptr) : nullptr; // TextManager::GetText(key, 0)
    textX = px, textY = py;                           // +0x28 / +0x2C
}

void TitleImage::Draw(f32 realDt) {                   // orig: 0x000EF010
    if (!loaded) return;
    if (hold <= 0.0f && direction != 0.0f) {
        alpha = 0.0f < direction ? 1.0f : 0.0f;
        fadeTime = 0.05f <= realDt ? F(double(fadeTime) + 0.05f) : F(double(realDt) + fadeTime);   // orig: [0x293BDC]
        if (fadeLength <= fadeTime) {
            direction = 0.0f;
        } else if (0.0f <= direction) {
            alpha = F(double(fadeTime) / fadeLength);
        } else {
            alpha = F((double(fadeLength) - fadeTime) / fadeLength);
        }
        if (image && !image->rects.empty()) {         // ((+0x24)+8): the first rect's colour (1, 1, 1, alpha)
            ni::NiColorA& c = image->rects[0].color;
            c.a = alpha;
            c.r = 1.0f, c.g = 1.0f, c.b = 1.0f;
            image->dirty |= 0xF;
        }
        if (text && label) {                          // +0x34 and +0x30: rebuilt at the position with the alpha
            text->RemoveAll();
            text->x = textX, text->y = textY;         // +0x4C = +0x28, +0x50 = +0x2C
            (void)text->AddString(label, alpha);      // 0x0009A070(+0x30, +0x14)
        }
    }
    if (camera) ni::VisitForCulling(*camera, nullptr);   // 0x00100CE0
    if (text) {                                       // 0x00098D70, 0x00098DB0
        text->UpdateCamera();
        text->Click();
    }
}

namespace {
void NotPortedText(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: 0x000A25D0 (cdecl(builder, fmt, ...)): vsprintf into a 0x400-byte stack buffer, then the builder's puts;
// returns the count written (an empty text is a failure). (QuickSave.cpp has the same helper.)
template <class... A>
bool PutText(std::string& out, const char* fmt, A... args) {
    char buf[0x400];
    std::snprintf(buf, sizeof buf, fmt, args...);
    out += buf;
    return buf[0] != '\0';
}
}  // namespace

// orig: native 161 FsChangeLevel 0x000B05B0 (cdecl(name); docs/analysis/object_gaps.md 21.3, frontend_port.md 2.5).
void Session::ChangeLevel(const char* name) {
    LevelData& level = *out_.level;
    // the play time in seconds: _ftol(S+0x180 + 0.5) + S+0x184 * 3600 (FLD; FADD [0x00293AA0] (0.5); _ftol truncates
    // the extended sum; IMUL 0xE10)
    const s32 seconds = FtoL(double(playSeconds2) + 0.5f) + playHours2 * 3600;
    if (level.endLevel) {
        // the App (game/app): the level record of S+0xC (the all-nibblers flag when S+0x190 > 0 && S+0x18C ==
        // S+0x190, the best time when none or a longer one is recorded), App+0xAC = name, the end-of-level menu made
        // (new(0x94) 0x000A4E70, the first time only) and opened (0x000A6340, then +1 = 1)
        level.endLevel(name, seconds);
    } else {
        NotPortedText("FsChangeLevel without the App (the record and the end-of-level menu are the App's)");
    }
    CarryOverText();                                  // 0x000C2770(0): after the menu's lives top-up
}

void Session::CarryOverText() { (void)SaveText(nullptr, nullptr); }   // orig: 0x000C2770(0)

u32 Session::SaveText(std::string* save, const char* appLevel) {   // orig: 0x000C2770
    LevelData& level = *out_.level;
    Player* P = level.player;                         // S+0x94
    if (!P) return 0;
    std::string out;                                  // the builder 0x00145A00
    if (save) {
        // "Level=%s\n" (0x002A344C) with App+0xAC when set, else S+0xC
        const char* name = appLevel && *appLevel ? appLevel : level.name.c_str();
        if (!PutText(out, "Level=%s\n", name)) return 0;
        if (level.carry->state) {                     // DAT_002EAA28: appended whole (0x00145B10: its DAT_002EAA2C
            out += *level.carry->state;               //  bytes, the terminating NUL included: L for a text not made
            out += '\0';                              //  by this function)
            *save = std::move(out);                   // 0x00145BB0 (the builder's buffer)
            return static_cast<u32>(save->size());
        }
    }
    bool ok = PutText(out, "Health=%f\n", double(P->health));   // 0x002A3440; FLD +0x4F4; FSTP qword
    for (s32 i = 0; ok && i < 5; ++i)                 // vf94(i), FILD
        ok = PutText(out, "Ammo=%d|%f\n", static_cast<int>(i), double(P->GetAmmo(i)));   // 0x002A3434
    ok = ok && PutText(out, "Power=%f\n", double(P->ChargeLevel()));   // 0x002A3428; vf164
    if (ok) {
        for (GameObject* o = P->inventory.NextObject(nullptr); o; o = P->inventory.NextObject(o)) {   // P+0x38
            if (!o->def->IsType(4) || !o->def->IsType(8)) continue;   // vf0C(4), vf0C(8): the weapons
            (void)PutText(out, static_cast<GameObject*>(P->weapon) == o ? "Equip=%s\n" : "Pickup=%s\n",   // P+0x528
                          o->def->name.c_str());     // 0x002A341C / 0x002A3410; (+0x10)+0x10 (results not tested)
        }
        ok = PutText(out, "Lives=%d\n", static_cast<int>(level.lives))   // 0x002A3404, DAT_002D687C
             && PutText(out, "AgeSeconds=%f\n", double(playSeconds))      // 0x002A33F4, S+0x178
             && PutText(out, "AgeHours=%d\n", static_cast<int>(playHours));   // 0x002A33E4, S+0x17C
    }
    if (!ok) return 0;                                // (the builder's dtor 0x00145A60; the old text stays)
    // the terminating NUL (0x00145B10(&0, 1)); out: the builder's buffer to the caller; else the old DAT_002EAA28
    // freed and the buffer kept (0x00145BB0); the size returned either way
    const u32 size = static_cast<u32>(out.size()) + 1;
    if (save) {
        out += '\0';
        *save = std::move(out);
        return size;
    }
    level.carry->state = std::move(out);              // (the port keeps the text without its NUL)
    return size;
}

void Session::Render(f32 realDt) {                    // orig: 0x000C8F20
    ni::RefreshAllBsps();                             // orig: 0x000FCF10 (every frame: loading screen, title, play)
    if (loadingScreen) {
        loadingScreen->Update(0.1f < realDt ? 0.1f : realDt);   // orig: [0x293AB0] = 0.1
        if (loadingScreen->IsDone()) loadingScreen.reset();
    }
    if (title) {
        ni::NiRenderer* r = o_.renderer;              // App->vf14()
        if (!loadingScreen && r) {                    // the black clear: vf6C(0, 0, 0), vf44(0, 1)
            r->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
            r->ClearBuffer(nullptr, 1);
        }
        title->Draw(realDt);
        if (title->IsVisible() && !title->IsFading()) {
            title->hold = 0.75f;                      // orig: vf18 SetHold(0.75f)
            title->FadeOut(1.0f);                     // orig: vf10 FadeOut(1.0f)
        }
        if (!title->IsVisible() && !title->IsFading()) {
            if (r) {                                  // black, then two clears (vf44(0, 1)) and presents (vf40)
                r->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
                r->ClearBuffer(nullptr, 1);
                r->DisplayFrame();
                r->ClearBuffer(nullptr, 1);
                r->DisplayFrame();
            }
            title.reset();
            return;
        }
    }
    if (state == 2 || loadingScreen || title) return;
    if (Intermission* im = out_.level->intermission; im && im->active && im->movie) {   // orig: 0x000C910C
        im->Render();                                 // I->vf0C 0x000D6370 (the movie camera's Click)
        blackFrames = 5;                              // orig: DAT_002EAA3C = 5
        return;
    }
    if (blackFrames > 0) {                            // orig: 0x000C9140
        --blackFrames;
        if (ni::NiRenderer* r = o_.renderer) {        // App->vf14(): vf6C(0x0030E940: black), vf44(0, 1)
            r->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
            r->ClearBuffer(nullptr, 1);
        }
        // NOT PORTED: TriggerManager::BindCameraRecords 0x000DAE50 (as in the overlay state).
        if (out_.level->script) script::ProgramUpdate(out_.level->script, 0.0f);   // ScriptProgram::Update(S+0xA8, 0)
        return;
    }
    UpdateStageStreaming();
    // the scene root's NiNode children and the root: UpdateWorldBound (vf98).
    LevelData& level = *out_.level;
    for (const auto& c : level.scene->Children())
        if (c && c->ClassName() == ni::NiNode::kClassName) c->UpdateWorldBound();
    level.scene->UpdateWorldBound();
    if (level.camera) level.camera->Apply();         // orig: 0x00012210
    // orig: 0x000C9211: while the pause menu is shown ((Ingame ctx+0x14)+0x24) the level's animations, objects,
    // stages, lights and the player's vf0C are skipped (LevelLoadOptions::pauseShown)
    const bool paused = o_.pauseShown && o_.pauseShown();
    if (!paused) RenderObjects(realDt);               // (the pause jumps straight to the clear, 0x000C9223)
    // orig: 0x000C92E7: App->vf14() (the renderer; no null test) vf44(0, DAT_002D67E0): the clear between the objects'
    // render and the camera's Click (the flags Camera::Apply just set: 4, or 5 with the fog)
    if (ni::NiRenderer* r = o_.renderer) r->ClearBuffer(nullptr, g_clearFlags);
    if (level.camera) level.camera->Click();          // orig: Camera 0x00012410 (the cull pass: NiCull.h)
    if (level.hud) level.hud->Draw();                 // orig: 0x000E5900 (S+0x118)
    if (level.camera) level.camera->DrawOverlay();    // orig: 0x00012450 (the overlay camera +0x34: the fade)
    if (level.player) level.player->RenderView(realDt);
}

void Session::RenderObjects(f32 realDt) {             // orig: 0x000C9229..0x000C92E7
    LevelData& level = *out_.level;
    // orig: ApplyPose 0x000BA990 culls the auto animations against the camera S+0x8C: its position +0x48 and the scene
    // NiCamera's (+0x30) world direction +0x110 (level_animation.md 2.1; no null test of the NiCamera)
    level::AnimCullCamera cull;
    const bool haveCam = level.camera && level.camera->sceneCamera;
    if (haveCam) cull = {level.camera->position, level.camera->sceneCamera->worldDir};
    out_.animations.ApplyPose(haveCam ? &cull : nullptr);
    // orig: 0x000C9256-0x000C92A9: the level's own objects' vf0C (the next one is taken first). Players (definition
    // IsType(2)) are skipped: their vf0C runs after the light manager. Characters (IsType(1)) are deleted when it
    // returns false; the others' result is ignored.
    for (GameObject* o = level.NextObject(nullptr); o;) {
        GameObject* next = level.NextObject(o);
        if (!o->def->IsType(2)) {
            if (o->def->IsType(1)) {
                if (!o->PostUpdate()) delete o;       // vf00(1)
            } else {
                o->PostUpdate();
            }
        }
        o = next;
    }
    for (size_t i = 0; i < level.stages.size(); ++i) level.stages[i]->Render();   // orig: Stage::Render 0x000CE340
    if (level.lightManager) level.lightManager->Update(level, realDt);   // orig: LightManager::Update 0x000C0B50 (vf0C)
    if (level.player) level.player->RenderUpdate();   // orig: player vf0C
}

}  // namespace fut::game
