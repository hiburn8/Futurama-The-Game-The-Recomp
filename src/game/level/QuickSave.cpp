// FsQuickSave / FsQuickLoad (natives 319 / 318: Session 0x000C7910 / 0x000C7D40), the level scripts' checkpoint and
// respawn: the player's state, the inventory and the collected-pickup flags as "key=value" lines in an in-memory text
// buffer (DAT_002EAA40, one per level; the original writes no file), read back line by line by the respawn. Transcribed
// from the assembly: docs/analysis/quicksave_port.md 1-2.
#include "game/level/LevelData.h"

#include "core/CrtString.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/IngameEvent.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/object/ChickenWalker.h"
#include "game/object/Pickup.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: the CRT _ftol 0x00167110 (truncation toward zero; the low 32 bits of the 64-bit result).
s32 FtoL(double v) { return static_cast<s32>(static_cast<s64>(v)); }

// The u16 id arrays' copy (FsQuickSave: 0x000C2220 grow + 0x000C22B0 SetAtGrow at the end for S+0x19C, the same code
// inline for S+0x1CC; FsQuickLoad: 0x000C2190 / inline clear, then the same appends): `dst` is cleared (its entries
// zeroed, size and count 0), then every entry of `src` is appended; the count counts the non-zero ones.
void CopyIds(const std::vector<u16>& src, std::vector<u16>& dst, u32& dstCount) {
    dst.clear();
    dstCount = 0;
    for (u16 v : src) {
        dst.push_back(v);
        if (v) ++dstCount;
    }
}

// orig: 0x000A25D0 (cdecl(builder, fmt, ...)): vsprintf into a 0x400-byte stack buffer, then the builder's puts
// 0x001454A0 (byte by byte through vf0C 0x00145B10, the buffer doubling from 0x400); returns the count written, so
// an empty text is a failure (FsQuickSave then keeps the old buffer). (A text over 0x3FF characters overflows the
// original's stack buffer; the port cuts it.)
template <class... A>
bool Put(std::string& out, const char* fmt, A... args) {
    char buf[0x400];
    std::snprintf(buf, sizeof buf, fmt, args...);
    out += buf;
    return buf[0] != '\0';
}

// orig: 0x00077930 (the nibbler map S+0x1FC): the level's cash pickups that sit in an inventory are marked collected
// in the map (state 1) under their keys.
void MarkCollectedCash(LevelData& L) {
    // (the index 5 is a function static set once: DAT_002E0978 / guard bit DAT_002E097C)
    const char c = L.name.size() > 5 ? L.name[5] : '\0';   // name[5] (the original reads it without a length check)
    if (c == '5') return;
    const char* n = c == '1' ? "FryCash" : c == '2' ? "BenderRuby" : c == '3' ? "LeelaGoldbar" : "Fish_skeleton_pickup";
    Def* d = L.defs ? L.defs->Find(n) : nullptr;      // Session::FindDef 0x000C3F10
    if (!d || !d->IsType(4)) return;                  // vf0C(4)
    for (GameObject* o = d->firstInstance; o; o = o->defNext)   // 0x000963E0 (+0xC, then +0x14)
        if (o->owner && o->owner->IsOwnerType(4))     // (+4)->vf0C(4) (no null test in the original)
            if (auto* p = dynamic_cast<Pickup*>(o)) p->StoreKeyState(1);   // 0x000766D0(1)
}

// orig: 0x00077870: every pending nibbler id is entered as "$%03d:%03d"(id, 0) = 1, then the pending list is cleared.
void CommitNibblers(LevelData& L) {
    for (u32 v : L.nibblerPending) {
        char key[0x100];
        std::snprintf(key, sizeof key, "$%03d:%03d", static_cast<int>(v), 0);   // 0x0029CDA8
        L.SetNibblerState(key, 1);                    // NiTStringMap::SetAt 0x0010BD70
    }
    L.nibblerPending.clear();                         // (entries zeroed, size +0x20 / count +0x24 = 0)
}

}  // namespace

void LevelData::QuickSave() {                         // orig: 0x000C7910 (FsQuickSave, native 319)
    Player* P = player;                               // S+0x94
    if (!P) return;
    CopyIds(enemyIds, savedEnemyIds, savedEnemyCount);    // S+0x19C -> S+0x1B4
    CopyIds(killedIds, savedKilledIds, savedKilledCount); // S+0x1CC -> S+0x1E4
    std::string out;                                  // the builder 0x00145A00 (vtbl 0x002AA6D0)
    bool ok = Put(out, "Level=%s\n", name.c_str())    // 0x002A344C, S+0x0C
              && Put(out, "CollPickedup=%d\n", static_cast<int>(pickupValue))   // S+0x188
              && Put(out, "NibPickedup=%d\n", static_cast<int>(nibblersPickedUp))   // S+0x18C
              && Put(out, "Health=%f\n", double(P->health));   // FLD +0x4F4; FSTP qword
    for (s32 i = 0; ok && i < 5; ++i)                 // vf94(i), FILD: "%d|%f" (the slot, the count as a double)
        ok = Put(out, "Ammo=%d|%f\n", static_cast<int>(i), double(P->GetAmmo(i)));
    ok = ok && Put(out, "Power=%f\n", double(P->ChargeLevel()));   // vf164 (FSTP qword)
    if (ok && P->chicken) ok = Put(out, "ChickRockets=%d\n", static_cast<int>(P->chicken->rockets));   // 0x00042730
    if (ok) {
        MarkCollectedCash(*this);                     // (S+0x1FC)->0x00077930
        CommitNibblers(*this);                        // (S+0x1FC)->0x00077870
        for (GameObject* o = P->inventory.NextObject(nullptr); o; o = P->inventory.NextObject(o)) {   // P+0x38
            if (!o->def->IsType(4)) continue;         // vf0C(4): pickups
            const char* defName = o->def->name.c_str();   // (+0x10)+0x10
            if (static_cast<GameObject*>(P->weapon) == o) {   // P+0x528 (the result is not tested)
                (void)Put(out, "Equip=%s|%d|NULL\n", defName, static_cast<int>(o->id));
                continue;
            }
            std::string key = "NULL";                 // 0x002A3888 + zeros (0x40 bytes)
            if (auto* pk = dynamic_cast<Pickup*>(o)) (void)pk->GetKey(key);   // 0x00076600
            (void)Put(out, "Pickup=%s|%d|%s", defName, static_cast<int>(o->id), key.c_str());
            if (o->label) (void)Put(out, "|%s", o->label->c_str());   // +0x34
            (void)Put(out, "\n");
        }
        for (u32 i = 0; i < instanceSlots; ++i)       // S+0x124 (read each pass)
            if (GetObjFlags(i) & 2) (void)Put(out, "Flag_Pickup=%d\n", static_cast<int>(i));   // collected
        // the terminating NUL (0x00145B10(&0, 1)); the old buffer freed, the builder's kept (0x00145BB0)
        quickSave = std::move(out);
    }
    // (the builder's dtor 0x00145A60 frees its buffer unless handed over)
}

void LevelData::QuickLoad() {                         // orig: 0x000C7D40 (FsQuickLoad, native 318)
    Player* P = player;                               // S+0x94
    if (!P) return;
    P->Reset();                                       // Player::Reset 0x00057680
    if (!quickSave) return;                           // DAT_002EAA40
    IngameEventSet& events = P->cdef.ingameEvents;    // (P+0x10)+0x28
    events.enabled = false;                           // 0x000E0220(0): +0x1C
    quickLoading = true;                              // DAT_002EAA48 = 1
    P->ClearInventory();                              // 0x00050580
    std::array<s32, 5> ammoIn{};                      // esp+0x2C..+0x3C
    GameObject* equip = nullptr;                      // esp+0x28
    CopyIds(savedEnemyIds, enemyIds, enemyCount);     // S+0x19C cleared (0x000C2190), S+0x1B4 copied back
    CopyIds(savedKilledIds, killedIds, killedCount);  // S+0x1CC cleared inline, S+0x1E4 copied back
    const char* p = quickSave->c_str();               // esp+0x24 (the buffer itself is not modified)
    while (*p) {
        const char* nl = std::strchr(p, '\n');        // strchr 0x00167050
        if (!nl) break;                               // (port guard: every saved line ends with "\n")
        char line[0x100];                             // esp+0x50
        size_t len = static_cast<size_t>(nl - p);
        if (len >= sizeof line) len = sizeof line - 1;   // (port guard: the original's buffer is 0x100 bytes)
        std::memcpy(line, p, len);                    // strncpy 0x001672C0
        line[len] = '\0';
        p = nl + 1;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;                            // (port guard: the original writes through NULL)
        *eq = '\0';
        char* val = eq + 1;
        // the keys compared with REPE CMPSB over their length + 1 (an exact match)
        if (std::strcmp(line, "CollPickedup") == 0) {
            if (crt::StrICmp(name.c_str(), "level1-1") != 0) pickupValue = std::atoi(val);   // S+0x188
        } else if (std::strcmp(line, "NibPickedup") == 0) {
            if (crt::StrICmp(name.c_str(), "level1-1") != 0) nibblersPickedUp = std::atoi(val);   // S+0x18C
        } else if (std::strcmp(line, "ChickRockets") == 0) {
            if (P->chicken) P->chicken->SetRockets(static_cast<u32>(std::atoi(val)));   // P+0x7B0: 0x000426E0
        } else if (std::strcmp(line, "Health") == 0) {
            P->health = P->cdef.maxHealth;            // +0x4F4 = D+0x60 (the saved value is not read)
        } else if (std::strcmp(line, "Ammo") == 0) {
            if (char* bar = std::strchr(val, '|')) {
                *bar = '\0';
                const s32 v = FtoL(std::strtod(bar + 1, nullptr));   // atof 0x00167609; _ftol
                const s32 slot = std::atoi(val);
                if (slot >= 0 && slot < 5) ammoIn[static_cast<size_t>(slot)] = v;   // (no range check in the original)
            } else {
                ammoIn.fill(FtoL(std::strtod(val, nullptr)));   // every slot
            }
        } else if (std::strcmp(line, "Power") == 0) {
            P->SetChargeLevel(F(std::strtod(val, nullptr)));   // vf168(FSTP atof)
        } else if (std::strcmp(line, "Flag_Pickup") == 0) {
            const s32 id = std::atoi(val);
            if (id > 0 && id < static_cast<s32>(instanceSlots)) instances[static_cast<size_t>(id)] = 3;
        } else if (std::strcmp(line, "Pickup") == 0 || std::strcmp(line, "Equip") == 0) {
            char* idText = std::strchr(val, '|');     // "<definition>|<id>|<key>[|<label>]"
            s32 id = 0;                               // esp+0x20
            if (idText) {
                *idText++ = '\0';
                id = std::atoi(idText);
            }
            char* key = idText ? std::strchr(idText, '|') : nullptr;   // (strchr(NULL) faults in the original)
            if (key) *key++ = '\0';
            char* label = key ? std::strchr(key, '|') : nullptr;
            if (label) *label++ = '\0';
            const bool isEquip = std::strcmp(line, "Equip") == 0;   // SETE
            Def* d = defs ? defs->Find(val) : nullptr;   // Session::FindDef 0x000C3F10
            if (!d) continue;
            const ni::NiMatrix3 rot{};                // 12 dwords of 0x002D7820 (identity)
            const ni::NiPoint3 pos{};
            Owner* owner = stages.empty() ? nullptr : stages.front().get();   // S+0x58 (+4: the stage's Owner part)
            GameObject* o = d->Spawn(*this, owner, pos, rot, nullptr);   // vf14(owner, &pos, &rot, 0)
            if (!o) {                                 // (the original dereferences it)
                NotPorted("FsQuickLoad: a saved inventory object whose definition spawns nothing");
                continue;
            }
            if (id) {
                o->SetId(static_cast<u32>(id));       // vf14
                SetObjFlags(static_cast<u32>(id), 3); // 0x000C1A40
            }
            if (key && std::strcmp(key, "NULL") != 0 && o->def->IsType(4))   // (REPE CMPSB, 5 bytes)
                if (auto* pk = dynamic_cast<Pickup*>(o)) pk->ApplyKey(key);   // 0x00076650
            if (label) o->SetName(label);             // 0x000968C0
            if (isEquip) {
                if (equip) equip->SetOwner(&P->inventory);   // the previous one into the inventory (vf10(P+0x38))
                equip = o;
            } else {
                o->SetOwner(&P->inventory);           // vf10(P+0x38)
            }
        }
    }
    // the inventory's weapons put away and taken off their parent (the hand)
    for (GameObject* o = P->inventory.NextObject(nullptr); o; o = P->inventory.NextObject(o)) {
        if (!o->def->IsType(8)) continue;             // vf0C(8)
        if (auto* w = dynamic_cast<Weapon*>(o)) (void)w->PutAway();   // vf60
        ni::NiAVObject* n = o->GetNode();             // vf30
        if (n && n->parent) (void)n->parent->DetachChild(n);   // (+0x14)->vfA4(&tmp, node); tmp released
    }
    if (equip) {
        equip->SetOwner(&P->inventory);               // vf10(P+0x38)
        if (auto* w = dynamic_cast<Weapon*>(equip)) (void)P->SetWeapon(w);   // vfB4
        else NotPorted("FsQuickLoad: an Equip line whose object is not a weapon (vfB4)");
    }
    quickLoading = false;                             // DAT_002EAA48 = 0
    for (s32 i = 0; i < 5; ++i) P->SetAmmo(i, ammoIn[static_cast<size_t>(i)]);   // 0x00051D50
    events.ResetIndexed(P->IngameContext());          // (D+0x28)->0x000F2980
    nibblerPending.clear();                           // (S+0x1FC)->0x00077900 (entries zeroed, size / count 0)
    events.enabled = true;                            // 0x000E0220(1)
}

}  // namespace fut::game
