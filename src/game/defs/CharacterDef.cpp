// Decompiled from default.xbe 0x0003E220-0x00042950 and the lip-sync key loader 0x0009CBD0; see CharacterDef.h.
#include "game/defs/CharacterDef.h"

#include "core/CrtString.h"
#include "core/XbeImage.h"
#include "engine/ni/NiClone.h"
#include "game/object/GameObject.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fut::game {
namespace {

using ni::detail::F;

int StrICmp(const char* a, const char* b) {           // orig: _stricmp (0x001684D1), ASCII folding
    for (;; ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || ca == 0) return ca - cb;
    }
}

// orig: NiObjectNET::GetName (0x000E2F70); an unnamed object has a NULL name.
const char* NameOf(const ni::NiObjectNET& o) { return o.name.empty() ? nullptr : o.name.c_str(); }

const char* const kPlayerNames[] = {"fry", "fry-1-1", "fry-1-2", "fry-1-3", "fry-1-4", "fry-1-5", "fry-1-6",
                                    "fry-5-3", "railgunfry", "leela", "bender", "rodent"};

// Little-endian reader over the key file (orig: 0x0009DAE0; the original does not check the size, the port
// reads past the end as zeros).
struct Reader {
    std::span<const u8> d;
    size_t pos = 0;
    u8 U8() { return pos < d.size() ? d[pos++] : (++pos, 0); }
    u32 U32() {
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(U8()) << (8 * i);
        return v;
    }
};

}  // namespace

const char* AnimationName(u32 id) {                   // orig: the pointer table 0x002D6228
    return id < kAnimationCount ? xbe::GameImage().StringAt(0x002D6228 + 4 * id) : "";
}

// ---- animation entries -------------------------------------------------------------------------------------------
void AnimTransition::Set(AnimEntry* to, const char* typeName, f32 t, u32 m) {   // orig: 0x0003EDD0
    target = to;
    hasType = typeName != nullptr;
    type = typeName ? typeName : "";
    time = t <= 0.0f ? 0.0625f : t;                   // orig: 0x00296C54
    mode = m;
}

const char* AnimEntry::Name() const { return sequence ? sequence->name.c_str() : nullptr; }   // orig: 0x0003F0B0

AnimTransition* AnimEntry::TransitionTo(const AnimEntry* to) {   // orig: 0x0003F1E0
    for (AnimTransition& t : transitions)
        if (t.target == to) return &t;
    return nullptr;
}

// ---- lip-sync key files ------------------------------------------------------------------------------------------
bool LipSyncKeyFile::Load(std::span<const u8> data) {   // orig: 0x0009CBD0
    Reader r{data};
    char header[33] = {};
    for (int i = 0; i < 32; ++i) header[i] = static_cast<char>(r.U8());
    if (std::strcmp(header, "LipSyncKeyFile v1.3") == 0) {
        version = 2;
    } else if (std::strcmp(header, "LipSyncKeyFile v1.2") == 0) {   // orig: 0x002A15A8
        version = 1;
    } else {
        return false;
    }
    auto count = [&] { const s32 n = static_cast<s32>(r.U32()); return n > 0 ? static_cast<size_t>(n) : 0; };
    records1.assign(count(), {});
    for (Record1& x : records1) {
        x.a = r.U8();
        x.b = r.U32();
        x.c = r.U32();
    }
    records2.assign(count(), {});
    for (Record2& x : records2) {
        x.a = r.U8();
        for (u32& v : x.v) v = r.U32();
    }
    records3.assign(count(), {});
    for (Record3& x : records3) {                      // orig: 0x0009D620 reads +4, +0, then the size and bytes
        x.field4 = r.U32();
        x.field0 = r.U32();
        x.data.resize(r.U32());
        for (u8& b : x.data) b = r.U8();
    }
    records4.clear();
    if (version == 2) {
        records4.assign(count(), {});
        for (Record4& x : records4)
            for (u32& v : x.v) v = r.U32();
    }
    return true;
}

namespace {
f32 Fl(u32 bits) { return std::bit_cast<f32>(bits); }

// orig: 0x0009CB50 (FaceControl::CalcSmoothCurve, ST0): 2x^2 up to 0.5, 1 - 2(1 - x)^2 above (and for a NaN).
double SmoothCurve(f32 x) {
    if (x < 0.5f || x == 0.5f) {                      // FCOMP [0x00293AA0] 0.5; TEST AH,0x41; JP
        const double s = double(x) * x;               // FMUL; FADD ST(0),ST(0)
        return s + s;
    }
    const double a = 1.0 - x;                         // FLD [0x00293A98] 1.0; FSUB
    const double b = a * a;
    return 1.0 - (b + b);                             // FSUBR 1.0
}

// orig: 0x0009CF30 (t, amplitude, centre, hold, attack, release; ST0): the amplitude within `hold` of the centre,
// the smoothed attack / release ramps around it, 0 outside (and at the centre itself when hold <= 0).
double Envelope(f32 t, f32 amp, f32 centre, f32 hold, f32 attack, f32 release) {
    const double de = std::fabs(double(t) - centre);  // FSUB; FABS; FST [esp] (the comparisons below use both)
    const f32 d = F(de);
    if (de < hold) return amp;                        // FCOMP; TEST AH,5; JP
    if (t < centre) {                                 // TEST AH,5; JP
        if (double(hold) + attack > d)                // FADD; FCOMP [esp]; TEST AH,0x41; JNE
            return SmoothCurve(F(1.0 - (double(d) - hold) / attack)) * amp;   // FSUBR 1.0; FSTP; FMUL
        return 0.0;                                   // (0x0009CF96's test fails: t < centre)
    }
    if (t > centre && double(hold) + release > d)     // TEST AH,0x41; JNE (equal: 0)
        return SmoothCurve(F(1.0 - (double(d) - hold) / release)) * amp;
    return 0.0;                                       // FLD [0x00293AB8]
}
}  // namespace

f32 LipSyncKeyFile::Duration() const {                // orig: 0x0009CA40
    f32 m = 0.0f;                                     // FLD [0x00293AB8]
    for (const Record1& r : records1)
        if (m < Fl(r.b)) m = Fl(r.b);                 // FCOM; TEST AH,5; JP
    for (const Record2& r : records2) {
        const double e = (double(Fl(r.v[0])) + Fl(r.v[4])) + Fl(r.v[2]);   // +4, +0x14, +0xC; FST [esp]
        if (e > m) m = F(e);                          // FCOMP ST(1); TEST AH,0x41; JNE
    }
    return m;
}

s32 LipSyncKeyFile::FindPhoneme(f32 t) const {        // orig: 0x0009CAB0
    const s32 n = static_cast<s32>(records1.size());
    if (n < 1) return -1;
    auto at = [&](s32 i) { return Fl(records1[static_cast<size_t>(i)].b); };
    if (at(0) > t) return -1;                         // TEST AH,0x41; JNE
    if (at(n - 1) < t || at(n - 1) == t) return n - 1;   // TEST AH,0x41; JP
    s32 i = n >> 1, step = n >> 1;                    // SAR
    for (;;) {
        step /= 2;                                    // CDQ; SUB; SAR
        if (step == 0) step = 1;
        if (i < 0 || i >= n) return -1;               // (port guard: the walk stays inside for sorted times)
        if (!(at(i) < t || at(i) == t)) {             // TEST AH,0x41; JP: later (or NaN)
            i -= step;
            continue;
        }
        if (i + 1 >= n) return -1;                    // (port guard, as above)
        if (at(i + 1) > t) return i;                  // TEST AH,0x41; JE
        i += step;
    }
}

s32 LipSyncKeyFile::Channels(char* codes, f32* values, s32 max, f32 t) const {   // orig: 0x0009CFD0
    s32 n = 0;
    for (const Record2& r : records2) {
        const f32 centre = Fl(r.v[0]), amp = Fl(r.v[1]), hold = Fl(r.v[2]), attack = Fl(r.v[3]),
                  release = Fl(r.v[4]);
        if (!(double(centre) - (double(attack) + hold) < t)) continue;   // FADD; FSUBR; FCOMP; TEST AH,5; JP
        if (!((double(release) + hold) + centre > t)) continue;          // TEST AH,0x41; JNE
        s32 k = 0;
        while (k < n && codes[k] != static_cast<char>(r.a)) ++k;
        if (k == n) {                                 // a new channel
            codes[n] = static_cast<char>(r.a);
            values[n] = 0.0f;
            k = n++;
        }
        values[k] = F(Envelope(t, amp, centre, hold, attack, release) + values[k]);   // FADD; FSTP
        if (n == max) break;
    }
    return n;
}

ni::NiPoint2 LipSyncKeyFile::EyeOffset(f32 t) const {   // orig: 0x0009D0C0
    const s32 n = static_cast<s32>(records4.size());
    s32 k = 0;
    while (k < n && !(Fl(records4[static_cast<size_t>(k)].v[2]) > t)) ++k;   // TEST AH,0x41; JE
    const s32 i = k - 1;
    if (i == -1) return {0.0f, 0.0f};
    const Record4& a = records4[static_cast<size_t>(i)];
    if (i == n - 1) return {Fl(a.v[0]), Fl(a.v[1])};
    const Record4& b = records4[static_cast<size_t>(i + 1)];
    const double d1 = double(Fl(b.v[1])) - Fl(a.v[1]);
    const double u = (double(t) - Fl(a.v[2])) / (double(Fl(b.v[2])) - Fl(a.v[2]));   // FDIVP
    return {F((double(Fl(b.v[0])) - Fl(a.v[0])) * u + Fl(a.v[0])), F(u * d1 + Fl(a.v[1]))};
}

// ---- the definition ----------------------------------------------------------------------------------------------
CharacterDef::CharacterDef(std::string characterName) {   // orig: 0x000409C0
    name = std::move(characterName);
    // orig: also appended to the level's character-definition list (g_level +0x3C, linked through +0xFC); the
    // port's level keeps its definitions in DefRegistry.
    path = "Chars\\" + name + ".ucf";                 // orig: 0x00296CDC + name + 0x00296CD4, set by 0x0003EEB0
}

void CharacterDef::RequestStream() {                  // orig: 0x0003FE00
    if (request || path.empty()) return;              // orig: only without a request and with a path
    streamRequested = true;
    if (stream::Manager* m = stream::Manager::Current()) request = m->Create(*this, "CharacterDef", name);   // 0x000D2160
}

bool CharacterDef::OnStream(stream::Request& r) {     // orig: 0x00041E30
    if (r.state == stream::State::New) {
        // orig: done at once when the model root already has a child named like the file.
        if (model)
            for (const auto& c : model->Children())
                if (c && c->name == path) {
                    request.reset();
                    return false;
                }
        r.SetPath(path);
        return true;
    }
    DefContext none;
    if (ni::NiStream* s = r.Stream()) OnStreamLoaded(*s, streamContext ? *streamContext : none);
    request.reset();
    return false;
}

ni::NiNode* CharacterDef::EnsureRoot() {
    if (!model) {
        model = new ni::NiNode;                       // orig: 0x00105C10
        model->name = "grunt";                        // orig: 0x00296F48
    }
    return model.Get();
}

AnimEntry* CharacterDef::FindAnimation(const char* sequenceName) const {   // orig: 0x0003F340
    if (!sequenceName || !*sequenceName) return nullptr;
    for (const auto& a : animations)
        if (a && std::strcmp(sequenceName, a->Name()) == 0) return a.get();
    return nullptr;
}

AnimEntry* CharacterDef::FindAnimationById(s32 id) const {   // orig: 0x0003F280
    if (id < 0 || id >= static_cast<s32>(kAnimationCount)) return nullptr;
    const char* wanted = AnimationName(static_cast<u32>(id));   // orig: sprintf("%s", name)
    for (const auto& a : animations)
        if (a && std::strcmp(wanted, a->Name()) == 0) return a.get();
    return nullptr;
}

bool CharacterDef::IsAnimationNameFree(const char* sequenceName) const {   // orig: 0x0003F220
    for (const auto& a : animations)
        if (a && StrICmp(sequenceName, a->Name()) == 0) return false;
    return true;
}

void CharacterDef::CreateEvents(AnimEntry& entry, DefContext& ctx) {   // orig: 0x000401B0
    if (!entry.sequence) return;
    auto* keys = dynamic_cast<ni::NiTextKeyExtraData*>(entry.sequence->textKeysObj.Get());
    if (!keys) return;
    for (const auto& key : keys->keys) {
        // A C string in the original: some shipped keys carry bytes after a NUL.
        const char* text = key.text.c_str();
        if (std::strlen(text) <= 6) continue;         // events are "<kind>:<name>" with a 6-character kind
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "%s:%s", name.c_str(), text + 6);
        AnimEvent ev;
        ev.text = text;                               // orig: 0x0003ED70 (copy)
        ev.soundEvent = ctx.lookupSoundEvent ? ctx.lookupSoundEvent(buffer) : 0;   // orig: 0x000CAFF0
        ev.time = key.time;
        entry.events.push_back(std::move(ev));
    }
}

AnimEntry* CharacterDef::AddAnimation(ni::NiSequence* sequence, DefContext& ctx) {   // orig: 0x000407E0
    if (!sequence) return nullptr;
    auto entry = std::make_unique<AnimEntry>();
    entry->sequence = sequence;
    // orig: frequency of the controller of the sequence's main entry (no bounds check).
    if (sequence->textKeyReference < sequence->entries.size())
        if (auto* c = dynamic_cast<ni::NiTimeController*>(sequence->entries[sequence->textKeyReference].controllerObj.Get()))
            entry->speed = c->frequency;
    CreateEvents(*entry, ctx);
    // orig: 0x00040100 adds a transition (0.0625 s, blend, no type): every animation to the new one and the new one
    // to every animation.
    for (const auto& a : animations)
        if (a) {
            AnimTransition t;
            t.target = entry.get();
            a->transitions.push_back(t);
        }
    for (const auto& a : animations)
        if (a) {
            AnimTransition t;
            t.target = a.get();
            entry->transitions.push_back(t);
        }
    animations.push_back(std::move(entry));
    return animations.back().get();
}

bool CharacterDef::LoadConfig(ni::NiUDSFileObject& file, ni::NiStream* stream, DefContext& ctx) {   // orig: 0x00041A50
    cameraDistance = 15.0f;
    runSpeed = 15.0f;
    cameraLookAhead = 2.0f;
    sneakSpeed = 5.0f;
    walkSpeed = 10.0f;
    rotSpeed = 4.0f;
    jumpSpeed = 45.0f;
    cameraHeight = 4.5f;
    lookAtHeight = 2.5f;
    maxHealth = 100.0f;
    aimSpeed = 1.0f;
    ConfigFile cfg;                                   // orig: 0x001429D0, parsed in the file's own buffer
    cfg.Parse(file.data);
    return ApplyConfig(cfg, stream, ctx);
}

bool CharacterDef::ApplyConfig(const ConfigFile& cfg, ni::NiStream* stream, DefContext& ctx) {   // orig: 0x000411A0
    cameraDistance = 15.0f;
    cameraLookAhead = 2.0f;
    sneakSpeed = 5.0f;
    walkSpeed = 10.0f;
    runSpeed = 15.0f;
    rotSpeed = 4.0f;
    jumpSpeed = 45.0f;
    cameraHeight = 4.5f;
    lookAtHeight = 2.5f;
    maxHealth = 100.0f;
    aimSpeed = 1.0f;
    if (!cfg.ok) return false;

    control = cfg.GetInt("control");
    sneakSpeed = F(cfg.GetFloat("sneakspeed"));
    walkSpeed = F(cfg.GetFloat("walkspeed"));
    runSpeed = F(cfg.GetFloat("runspeed"));
    runJumpSpeed = F(cfg.GetFloat("runjumpspeed"));
    ladderClimbSpeed = F(cfg.GetFloat("ladderclimbspeed"));
    ledgeSlideSpeed = F(cfg.GetFloat("ledgeslidespeed"));
    double v = cfg.GetFloat("boundwidth");            // the bounds only when positive
    if (v > 0.0) boundWidth = F(v);
    v = cfg.GetFloat("boundheight");
    if (v > 0.0) boundHeight = F(v);
    v = cfg.GetFloat("bounddepth");
    if (v > 0.0) boundDepth = F(v);
    boundCenter = F(cfg.GetFloat("boundcenter"));
    fpsHeight = F(cfg.GetFloat("fpsheight"));
    jumpSpeed = F(cfg.GetFloat("jumpspeed"));
    jumpWalkSpeed = F(cfg.GetFloat("jumpwalkspeed"));
    jumpRunSpeed = F(cfg.GetFloat("jumprunspeed"));
    ledgeGrabDistance = F(cfg.GetFloat("ledgegrabdistance"));
    ledgeGrabHeight = F(cfg.GetFloat("ledgegrabheight"));
    v = cfg.GetFloat("stepheight");
    stepHeight = F(v);
    if (v < static_cast<double>(0.1f)) stepHeight = 1.8f;   // orig: x87 compare with the float 0.1 (0x00293AB0)
    rotSpeed = F(cfg.GetFloat("rotspeed"));
    aimSpeed = F(cfg.GetFloat("aimspeed"));
    lookAtHeight = F(cfg.GetFloat("lookatheight"));
    cameraHeight = F(cfg.GetFloat("cameraheight"));
    cameraDistance = F(cfg.GetFloat("cameradistance"));
    cameraLookAhead = F(cfg.GetFloat("cameralookahead"));
    maxHealth = static_cast<f32>(cfg.GetInt("maxhealth"));

    const s32 n = cfg.Count();
    // Pass 1: "anim:<name>  <file> <top-level index>" registers the sequences.
    s32 index = 0;                                    // orig: a stack slot kept across lines (uninitialised at first)
    for (s32 i = 0; i < n; ++i) {
        const char* key = cfg.Key(i);
        if (!key || crt::StrNICmp(key, "anim:", 5) != 0) continue;
        const char* value = cfg.Value(i);
        std::vector<char> file(std::strlen(value) + 1);
        std::sscanf(value, "%s %d", file.data(), &index);
        if (!stream) continue;                        // orig: 0x000410E0 loads the sequence from <file> (not ported)
        const auto& top = stream->TopLevelLinks();
        // orig: no bounds check.
        ni::NiObject* obj = static_cast<u32>(index) < top.size() ? stream->ResolveLink(top[index]) : nullptr;
        if (!obj) continue;
        if (auto* seq = dynamic_cast<ni::NiSequence*>(obj)) {
            if (IsAnimationNameFree(seq->name.c_str())) AddAnimation(seq, ctx);
        }
        // orig: a NiSequenceStreamHelper is turned into a sequence by 0x0012F140; no shipped file has one.
    }

    // Pass 2: an "anim:<name>" line followed by a "{ ... }" block sets damage and the transitions to other
    // animations ("blend:<target>  <type> <time>", "morph:<target>  <x> <time>", "immed:<target>  <x> <time>").
    char type[256] = {};                              // orig: stack buffers kept across lines
    f32 time = 0.0f;
    for (s32 i = 0; i < n; ++i) {
        const char* key = cfg.Key(i);
        if (!key || crt::StrNICmp(key, "anim:", 5) != 0) continue;
        AnimEntry* entry = FindAnimation(key + 5);
        if (!entry) continue;
        const char* open = cfg.Key(i + 1);
        if (!open || crt::StrNICmp(open, "{", 1) != 0) continue;   // port-only: NULL check
        s32 j = i + 1;
        for (; j < n; ++j) {
            const char* value = cfg.Value(j);
            const char* k = cfg.Key(j);
            if (!k) break;                            // port-only: the original would fault on the spare slot
            if (crt::StrNICmp(k, "}", 1) == 0) break;
            if (StrICmp(k, "damage") == 0) {
                f32 d = 0.0f;
                std::sscanf(value, "%f", &d);
                entry->hasDamage = true;
                entry->damage = d;
            } else if (StrICmp(k, "damagetype") == 0) {
                s32 t = 0;
                std::sscanf(value, "%d", &t);
                entry->hasDamage = true;
                entry->damageType = t;
            } else if (crt::StrNICmp(k, "blend:", 6) == 0 || crt::StrNICmp(k, "morph:", 6) == 0 ||
                       crt::StrNICmp(k, "immed:", 6) == 0) {
                std::sscanf(value, "%s %f", type, &time);
                const u32 mode = k[0] == 'b' ? 1 : k[0] == 'm' ? 2 : 0;
                const char* typeName = mode == 1 ? type : "";
                for (AnimTransition& t : entry->transitions)   // orig: 0x0003F640 (strcmp with the target's name)
                    if (t.target && std::strcmp(k + 6, t.target->Name()) == 0) t.Set(t.target, typeName, time, mode);
            } else if (crt::StrNICmp(k, "anim:", 5) == 0) {
                --j;
                break;
            }
        }
        i = j;
    }

    // Variant counts.
    auto count = [&](s32 first, s32 num, s32 start) {
        s32 c = start;
        for (s32 id = first; id < first + num; ++id)
            if (FindAnimationById(id)) ++c;
        return c;
    };
    attack1Count = count(47, 5, 0);
    attack2Count = count(52, 5, 1);
    closeAttack1Count = count(59, 2, 0);
    closeAttack2Count = count(61, 2, 0);
    dyingCount = count(77, 5, 0);
    damageCount = count(71, 3, 0);
    idleCount = count(0, 9, 0);
    longIdleCount = count(9, 4, 0);
    if (StrICmp(name.c_str(), "bender") == 0) idleCount = 2;
    if (idleCount == 0) idleCount = 1;
    return true;
}

bool CharacterDef::TakeDataFile(ni::NiAVObject* object, ni::NiStream* stream, DefContext& ctx) {   // orig: 0x00041B00
    if (!object) return false;
    if (auto* file = dynamic_cast<ni::NiUDSFileObject*>(object)) {
        const char* n = NameOf(*file);
        if (!n) {
            LoadConfig(*file, stream, ctx);
        } else if (std::strstr(n, "lipsync")) {
            lipSyncFile = file;
        } else if (std::strcmp(n, "char.cfg") == 0) {
            LoadConfig(*file, stream, ctx);
        } else if (std::strcmp(n, "IngameEvent") == 0) {
            ingameEventFile = file;
        } else if (std::strcmp(n, "HUD_Head") == 0) {
            hudHeadFile = file;
        } else {
            ni::NiPointer<LipSyncKeyFile> key(new LipSyncKeyFile);
            if (key->Load(file->data)) {
                // orig: "single_blink.key" is compared twice (modes 0 and 1); the second can never match.
                if (StrICmp(n, "single_blink.key") == 0) {
                    keyFiles[0] = key;                // orig: 0x0003FFE0 (replaces an existing entry)
                } else if (StrICmp(n, "electric.key") == 0) {
                    keyFiles[2] = key;
                }
            }
        }
        return true;
    }
    if (auto* node = dynamic_cast<ni::NiNode*>(object)) {
        bool any = false;
        for (u32 i = 0; i < node->Children().size(); ++i) {
            ni::NiAVObject* child = node->Children()[i].Get();
            if (!child) continue;
            any |= TakeDataFile(child, stream, ctx);
            if (dynamic_cast<ni::NiUDSFileObject*>(child)) node->DetachChild(child);
        }
        return any;
    }
    return false;
}

void CharacterDef::RemoveTextureEffects(ni::NiNode* node) {   // orig: 0x0003F730
    if (!node) return;
    for (u32 i = 0; i < node->Children().size(); ++i) {
        ni::NiAVObject* child = node->Children()[i].Get();
        if (!child) continue;
        if (auto* fx = dynamic_cast<ni::NiTextureEffect*>(child)) {
            envEffect = fx;                           // orig: +0x118 (the last one found wins)
            fx->DetachAllAffectedNodes();             // orig: 0x00141A30
            node->DetachChild(fx);
        } else if (auto* sub = dynamic_cast<ni::NiNode*>(child)) {
            RemoveTextureEffects(sub);
        }
    }
}

void CharacterDef::OnStreamLoaded(ni::NiStream& stream, DefContext& ctx) {   // orig: 0x00041E30
    std::vector<ni::NiObject*> top;
    for (u32 link : stream.TopLevelLinks()) top.push_back(stream.ResolveLink(link));

    bool handled = false;
    s32 part = 0;
    for (ni::NiObject* obj : top) {
        if (!obj || dynamic_cast<ni::NiCamera*>(obj)) continue;
        if (auto* node = dynamic_cast<ni::NiNode*>(obj)) {
            EnsureRoot()->AttachChild(node, true);
            char buffer[1024];
            std::snprintf(buffer, sizeof buffer, "%s:%d", path.c_str(), part++);
            node->name = buffer;
            handled |= TakeDataFile(node, &stream, ctx);
        } else if (auto* fx = dynamic_cast<ni::NiTextureEffect*>(obj)) {
            EnsureRoot()->AttachChild(fx, true);      // orig: 0x00042159-0x000422BF (TakeDataFile not called)
            envEffect = fx;                           // orig: +0x118 (0x0004227F)
        } else if (auto* file = dynamic_cast<ni::NiUDSFileObject*>(obj)) {
            handled |= TakeDataFile(file, &stream, ctx);
        }
    }
    if (!handled) {                                   // no data file: every top-level sequence is an animation
        for (ni::NiObject* obj : top) {
            // orig: NiSequenceStreamHelper -> 0x0012F140 first (no shipped file has one).
            if (auto* seq = dynamic_cast<ni::NiSequence*>(obj)) AddAnimation(seq, ctx);
        }
    }
    if (!model) return;

    model->name = "CharacterDef::" + name;            // orig: sprintf 0x00296F2C
    RemoveTextureEffects(model.Get());
    envEffect = nullptr;                              // orig: 0x000424D0-0x000424E5 (released right away)
    PrepareCharacterProperties(model.Get());
    for (u32 i = 0; i < model->Children().size(); ++i)
        if (ni::NiAVObject* child = model->Children()[i].Get()) PrepareCharacterPartProperties(child);
    PrepareModelLighting(*this, model.Get());
    model->UpdateProperties();                        // 0x000425B3
    model->UpdateEffects();                           // 0x000425BB
    model->UpdateDownwardPass(0.0f, true);            // Update(0, controllers) 0x001018D0
    path.clear();
    // orig: the objects waiting for this definition (list at +0x0C, linked through +0x14, 0x000963E0) are
    // initialised (vtbl+4).
    for (GameObject* o = firstInstance; o; o = o->defNext) o->Init();
}

ni::NiPointer<ni::NiAVObject> CharacterDef::AcquireModel(u32 idx) {   // orig: 0x00040450
    if (!model) return nullptr;
    ni::NiNode* root = model.Get();
    if (idx >= root->Children().size()) return nullptr;
    ni::NiAVObject* src = root->Children()[idx].Get();
    if (!src) {
        bool anyNode = false;                         // (L) the original then clones the same empty slot
        for (const auto& c : root->Children()) anyNode |= dynamic_cast<ni::NiNode*>(c.Get()) != nullptr;
        if (!anyNode) model = nullptr;                // orig: releases D+0x20
        return nullptr;
    }
    auto clone = [&] { return ni::NiPointer<ni::NiAVObject>(dynamic_cast<ni::NiAVObject*>(ni::Clone(src).Get())); };
    for (auto& c : modelCache) {                      // (g_cloneNameMode = 1)
        if (!c) {
            c = clone();
            return c;
        }
        if (c->RefCount() == 1 && StrICmp(c->name.c_str(), src->name.c_str()) == 0) return c;
        if (c->parent == root && StrICmp(c->name.c_str(), src->name.c_str()) == 0) return c;
    }
    ni::NiPointer<ni::NiAVObject> out = clone();
    modelCache.push_back(out);
    return out;
}

std::unique_ptr<CharacterDef> CreateCharacterDef(const std::string& name) {   // orig: 0x000C62EF
    for (const char* p : kPlayerNames)
        if (StrICmp(name.c_str(), p) == 0) return std::make_unique<PlayerDef>(name);
    if (StrICmp(name.c_str(), "kamikaze") == 0) return std::make_unique<SimpleNPCDef>(name);
    return std::make_unique<GenericNPCDef>(name);
}

// ---- model preparation -------------------------------------------------------------------------------------------
void PrepareCharacterProperties(ni::NiAVObject* object) {   // orig: 0x0003F8D0
    if (!object) return;
    if (auto* mat = dynamic_cast<ni::NiMaterialProperty*>(GetProperty(*object, 3))) {
        mat->emissive = mat->diffuse;
        ++mat->revision;
    }
    if (GetProperty(*object, 11)) RemoveProperty(*object, 11);
    if (GetProperty(*object, 5)) RemoveProperty(*object, 5);
    if (auto* vc = dynamic_cast<ni::NiVertexColorProperty*>(GetProperty(*object, 9))) {
        vc->sourceVertexMode = 1;
        vc->lightingMode = 0;
    } else {
        auto* p = new ni::NiVertexColorProperty;      // orig: ctor flags 0, modes 0 / 1
        p->sourceVertexMode = 1;
        p->lightingMode = 0;
        AttachProperty(*object, p);
    }
    object->UpdateProperties();                       // 0x0003FAA4
    auto* node = dynamic_cast<ni::NiNode*>(object);
    if (!node) return;
    node->DetachAllEffects();
    if (NameOf(*node) && node->name == "QDOutline") return;
    const size_t size = node->Children().size();
    for (size_t i = 0; i < size; ++i)
        if (i < node->Children().size() && node->Children()[i]) PrepareCharacterProperties(node->Children()[i].Get());
}

void PrepareCharacterPartProperties(ni::NiAVObject* object) {   // orig: 0x0003FB30
    if (!object) return;
    if (auto* mat = dynamic_cast<ni::NiMaterialProperty*>(GetProperty(*object, 3))) {
        mat->emissive = mat->diffuse;
        ++mat->revision;
    }
    if (auto* zb = dynamic_cast<ni::NiZBufferProperty*>(GetProperty(*object, 11))) {
        zb->flags |= 3;
    } else {
        auto* p = new ni::NiZBufferProperty;          // orig: 0x0003F460: flags 3, test function 3
        p->flags = 3;
        p->testFunction = 3;
        AttachProperty(*object, p);
    }
    if (auto* shade = dynamic_cast<ni::NiShadeProperty*>(GetProperty(*object, 5)))
        shade->flags = static_cast<u16>(shade->flags & ~1u);
    if (auto* vc = dynamic_cast<ni::NiVertexColorProperty*>(GetProperty(*object, 9))) {
        vc->sourceVertexMode = 0;
        vc->lightingMode = 0;
    } else {
        auto* p = new ni::NiVertexColorProperty;
        p->sourceVertexMode = 0;
        p->lightingMode = 0;
        AttachProperty(*object, p);
    }
    object->UpdateProperties();                       // 0x0003FCFC
}

}  // namespace fut::game
