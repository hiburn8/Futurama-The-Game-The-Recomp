// IngameEvent; see IngameEvent.h.
#include "game/defs/IngameEvent.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "game/defs/CharacterDef.h"
#include "game/level/Dialogue.h"
#include "game/level/Hud.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/text/LanguageDb.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::game {

namespace {

// ---- globals of the original ----------------------------------------------------------------------------------------
std::optional<std::string> g_defaultDb;               // orig: DAT_002EFEF8
LipSyncPlayer* g_lipPlayer = nullptr;                 // orig: DAT_002EFEFC (during a parse)
s32 g_lineCount = 0;                                  // orig: DAT_002EFEF4
std::vector<IngameSoundRequest> g_requests;           // orig: the loader DAT_002F0680

// orig: SoundRequestLoader::Purge 0x000F2470 (line): every request of the line leaves the ring, except the head one,
// which stays at the head with its callback cleared (the loader may be reading its file); the others keep their order.
void PurgeRequests(const void* line) {
    auto& q = g_requests;
    for (size_t i = 0; i < q.size();) {
        if (q[i].line != line) {
            ++i;
        } else if (i == 0) {
            q[i].line = nullptr;                      // (head)->+4 = 0, pushed back first
            ++i;
        } else {
            q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));   // deleted
        }
    }
}

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: the reader 0x0009DA50 over a private copy of the file. The original never checks the size; the port reads
// zeros past the end.
struct Reader {
    const std::vector<u8>& d;
    size_t pos = 0;
    u32 U32() {
        u32 v = 0;
        for (int i = 0; i < 4; ++i, ++pos)
            if (pos < d.size()) v |= u32(d[pos]) << (8 * i);
        return v;
    }
    // orig: 0x0009DB50: u16 length + bytes; length 0 = NULL; an embedded NUL ends the C string.
    std::optional<std::string> Str() {
        u16 n = 0;
        for (int i = 0; i < 2; ++i, ++pos)
            if (pos < d.size()) n = static_cast<u16>(n | (d[pos] << (8 * i)));
        if (n == 0) return std::nullopt;
        std::string s;
        for (u32 i = 0; i < n; ++i, ++pos) {
            const char c = pos < d.size() ? static_cast<char>(d[pos]) : '\0';
            s.push_back(c);
        }
        s.resize(std::strlen(s.c_str()));
        return s;
    }
};

// orig: 0x000F0130 (both lists split in place on ';', _stricmp with the level name; an include list wins).
bool LevelFilter(const std::optional<std::string>& include, const std::optional<std::string>& exclude,
                 const std::string& level) {
    auto listed = [&](const std::string& list, bool& match) {
        size_t start = 0;
        for (;;) {
            const size_t semi = list.find(';', start);
            const std::string tok = list.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            if (crt::StrICmp(tok.c_str(), level.c_str()) == 0) {
                match = true;
                return;
            }
            if (semi == std::string::npos || semi + 1 >= list.size()) return;   // end, or a trailing ';'
            start = semi + 1;
        }
    };
    bool match = false;
    if (include) {
        listed(*include, match);
        return match;
    }
    if (exclude) {
        listed(*exclude, match);
        return !match;
    }
    return true;
}

// orig: 0x000C33F0 (at most 127 characters, upper-cased with the CRT toupper, exact lookup).
const text::LanguageDb* LookupDb(const LevelData& level, const std::string& name) {
    if (name.size() > 127) return nullptr;
    std::string key = name;
    for (char& c : key)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    const auto it = level.databases.find(key);
    return it == level.databases.end() ? nullptr : it->second;
}

// orig: the walk 0x00034290 / 0x00155B60 over the map at S+0xEC: bucket order (hash 0x000C3700 % 37), within a
// bucket the newest key first; the first database that has the id.
const text::LanguageDb* FallbackDb(const LevelData& level, const text::TextManager& tm, const std::string& id) {
    auto hash = [](const std::string& k) {
        u32 h = 0;
        for (char c : k) h = h * 33 + static_cast<u32>(static_cast<s32>(static_cast<signed char>(c)));
        return h % 37;
    };
    for (u32 b = 0; b < 37; ++b)
        for (auto it = level.databaseOrder.rbegin(); it != level.databaseOrder.rend(); ++it) {
            if (hash(*it) != b) continue;
            const auto db = level.databases.find(*it);
            if (db != level.databases.end() && tm.Find(id, db->second)) return db->second;
        }
    return nullptr;
}

// orig: the dialogue builder 0x000D7FF0 (the database, the lip list S+0xE8, the sound map S+0x100 unless `withSounds`
// is false, directories ""), NewDialogue, NewSegment(0), AddLip(lipPlayer) with a player, AddSound, CloseSegment(-1),
// Build (Dialogue.h); null when none is built.
ni::NiPointer<Dialogue> BuildDialogue(const IngameEventContext& ctx, const text::LanguageDb* db, const std::string& id,
                                      LipSyncPlayer* lipPlayer, bool withSounds) {
    DialogueBuilder b(*ctx.level, ctx.text, db);
    b.NewDialogue();
    b.NewSegment(0.0f);
    if (lipPlayer) (void)b.AddLip(lipPlayer, id);
    if (withSounds) (void)b.AddSound(id);
    b.CloseSegment(-1.0f);
    return b.Build();
}

}  // namespace

std::vector<IngameSoundRequest>& IngameSoundRequests() { return g_requests; }

const text::LanguageDb* FindLanguageDb(const LevelData& level, const std::string& name) { return LookupDb(level, name); }
const text::LanguageDb* FindLanguageDbWithId(const LevelData& level, const text::TextManager& tm, const std::string& id) {
    return FallbackDb(level, tm, id);
}
s32 IngameLineCount() { return g_lineCount; }

// ---- lines ------------------------------------------------------------------------------------------------------------
class IngameEventLine {
public:
    virtual ~IngameEventLine() = default;
    virtual bool Load(Reader& r, const IngameEventContext& ctx) = 0;   // vf14
    virtual bool Valid(const IngameEventContext& ctx) { (void)ctx; return true; }   // vf0C (0x000A32A0)
    virtual bool Play(const IngameEventContext& ctx) = 0;              // vf04
    // vf10: DialogueLine / HintLine 0x000B0420 (a bare RET); RandomLine 0x000F06F0.
    virtual void Release() {}
    // The request loader's callback (the random line's interface +0xC, vf00 0x000F08F0): its sound file loaded.
    virtual void SoundLoaded(ni::NiUDSFileObject* file) { (void)file; }
};

namespace {

class DialogueLineBase : public IngameEventLine {     // orig: ctor 0x000F0040, dtor 0x000F0060
public:
    DialogueLineBase() { ++g_lineCount; }
    ~DialogueLineBase() override {
        dialogue = nullptr;                           // (+4 released by the derived dtors first)
        if (--g_lineCount == 0) g_defaultDb.reset();
    }
    ni::NiPointer<Dialogue> dialogue;                 // orig: +0x04
    LipSyncPlayer* lipPlayer = nullptr;               // orig: +0x08
    // orig: vf04 0x000F0100 (DialogueLine) / 0x000F05E0 (RandomLine): false without a dialogue, else the dialogue
    // display DAT_002EBDAC->0x000D7BE0(a reference to the dialogue) decides.
    bool Play(const IngameEventContext& ctx) override {
        if (!dialogue || !ctx.level || !ctx.level->dialogue) return false;
        return ctx.level->dialogue->Play(dialogue.Get());
    }
};

class DialogueLine : public DialogueLineBase {        // orig: 0xC bytes, vtbl 0x002A58D4
public:
    bool Load(Reader& r, const IngameEventContext& ctx) override {   // orig: 0x000F0200
        auto inc = r.Str(), exc = r.Str(), id = r.Str(), db = r.Str();
        v = r.U32();
        if (!LevelFilter(inc, exc, ctx.level->name)) return false;
        if (!db && g_defaultDb) db = g_defaultDb;
        const text::LanguageDb* d = nullptr;
        if (!id) return false;                        // (the original dereferences NULL)
        if (db) {
            d = LookupDb(*ctx.level, *db);
            if (!d || !ctx.text->Find(*id, d)) return false;
        } else {
            d = FallbackDb(*ctx.level, *ctx.text, *id);
            if (!d) return false;
        }
        lipPlayer = g_lipPlayer;                      // +8 = DAT_002EFEFC
        ni::NiPointer<Dialogue> built = BuildDialogue(ctx, d, *id, lipPlayer, true);
        if (!built || built->segments.empty()) {      // (+4 released)
            dialogue = nullptr;
            return false;
        }
        built->priority = static_cast<s32>(v);        // +0x18 = the line's word
        dialogue = built;                             // NiPointer::Assign 0x0002A400
        return true;
    }
    u32 v = 0;
};

class RandomLine : public DialogueLineBase {          // orig: 0x24 bytes, ctor 0x000F04F0, vtbl 0x002A58FC
public:
    ~RandomLine() override {                          // orig: 0x000F0530: its pending sound requests are dropped
        PurgeRequests(this);                          // SoundRequestLoader::Purge 0x000F2470
        *self_ = nullptr;                             // (port: a dialogue's callback outliving the line does nothing)
    }
    bool Load(Reader& r, const IngameEventContext& ctx) override {   // orig: 0x000F0740
        auto inc = r.Str(), exc = r.Str();
        textId = r.Str();
        auto dbName = r.Str();
        v = r.U32();
        if (!LevelFilter(inc, exc, ctx.level->name)) return false;
        if (!dbName && g_defaultDb) dbName = g_defaultDb;
        if (!textId) return false;                    // (the original dereferences NULL)
        if (dbName) {
            this->db = LookupDb(*ctx.level, *dbName);
            if (!this->db || !ctx.text->Find(*textId, this->db)) return false;
        } else {
            this->db = FallbackDb(*ctx.level, *ctx.text, *textId);
            if (!this->db) return false;
        }
        lipPlayer = g_lipPlayer;
        return true;
    }
    void Release() override {                         // orig: vf10 0x000F06F0
        PurgeRequests(this);                          // SoundRequestLoader::Get(this + 0xC)->Purge 0x000F2470
        Drop();                                       // +0x04 released and cleared
        file = nullptr;                               // +0x14 (the loaded sound file) released and cleared
    }
    void SoundLoaded(ni::NiUDSFileObject* f) override {   // orig: 0x000F08F0 (this = line+0xC)
        if (context) Build(*context, f);
    }
    bool Valid(const IngameEventContext& ctx) override {   // orig: 0x000F0610
        const char* s = ctx.text->GetStr4(*textId, nullptr);   // (every database, in load order)
        if (!s || !*s) {
            Build(ctx, nullptr);                      // 0x000F08F0(0) at once: the lip track only
            return true;
        }
        context = std::make_unique<IngameEventContext>(ctx);   // (port: the builder's level / text for the callback)
        Drop();
        char path[0x200];
        std::snprintf(path, sizeof path, "%s\\%s\\%s.wav", "Sounds\\Localized", ctx.text->Language().c_str(), s);
        if (!ctx.fileExists || !ctx.fileExists(path)) return false;   // orig: 0x000F2250
        g_requests.push_back({path, this});
        return true;
    }
    std::optional<std::string> textId;                // orig: +0x18
    const text::LanguageDb* db = nullptr;             // orig: +0x1C
    u32 v = 0xFFFFFFFF;                               // orig: +0x20

private:
    void Drop() { dialogue = nullptr; }
    std::shared_ptr<RandomLine*> self_ = std::make_shared<RandomLine*>(this);
    ni::NiPointer<ni::NiUDSFileObject> file;          // orig: +0x14 (the request loader's file)
    std::unique_ptr<IngameEventContext> context;      // (port) the context of the request's Valid
    // orig: 0x000F08F0 (this = line+0xC; the loaded sound file or 0): the builder (the line's database, the lip list
    // S+0xE8), +0x14 = the file, NewDialogue, NewSegment(0), AddLip(+8, textId) with a player, with a file
    // AddSoundFile 0x000D86E0, CloseSegment(-1), Build; a dialogue with a segment gets the line's priority word
    // (+0x20) and the line's stop callback (+0x10, 0x000D8B00: 0x000F0A80 releases the line's dialogue and file) and
    // becomes +4; else +0x14 and +4 are released.
    void Build(const IngameEventContext& ctx, ni::NiUDSFileObject* f) {
        file = f;                                     // NiPointer::Assign (+0x14)
        DialogueBuilder b(*ctx.level, ctx.text, db);  // 0x000D7FF0, the database 0x000D8150, the lip list 0x000D8080
        b.NewDialogue();                              // 0x000D8200
        b.NewSegment(0.0f);                           // 0x000D8270(0)
        if (lipPlayer) (void)b.AddLip(lipPlayer, *textId);   // 0x000D8340
        if (file) (void)b.AddSoundFile(file.Get());   // 0x000D86E0
        b.CloseSegment(-1.0f);                        // 0x000D8A00
        ni::NiPointer<Dialogue> built = b.Build();    // 0x000D8AB0
        if (!built || built->segments.empty()) {
            file = nullptr;
            Drop();
            return;
        }
        built->priority = static_cast<s32>(v);
        built->onStop = [self = self_](Dialogue&) {   // 0x000F0A80
            if (*self) {
                (*self)->Drop();
                (*self)->file = nullptr;
            }
        };
        dialogue = built;
    }
};
class HintLine : public IngameEventLine {             // orig: 0xC bytes, ctor 0x000F0B30, vtbl 0x002A5938
public:
    bool Load(Reader& r, const IngameEventContext& ctx) override {   // orig: 0x000F0BB0
        (void)ctx;
        a = r.Str();
        b = r.Str();
        return true;
    }
    bool Play(const IngameEventContext& ctx) override {   // orig: vf04 0x000F0AE0 (the HUD's hint display, true)
        // (S+0x118)->0x000E5BC0("info", "add", +4), then (S+0x118)->0x000E5B90("info", +4, 4.0f) (no null tests)
        Hud* hud = ctx.level ? ctx.level->hud.get() : nullptr;
        const char* text = a ? a->c_str() : nullptr;
        if (hud) {
            hud->Command("info", "add", text);       // 0x002A20FC / 0x002A2104
            hud->Command("info", text, 4.0f);        // 0x40800000
        }
        return true;
    }
    std::optional<std::string> a, b;                  // orig: +0x04 / +0x08
};

// ---- selection ----------------------------------------------------------------------------------------------------
void Refill(IngameEvent& e) {                         // orig: 0x000F1460
    e.pool.clear();
    for (s32 i = 0; i < static_cast<s32>(e.lines.size()); ++i) e.pool.push_back(i);
    e.left = static_cast<u32>(e.lines.size());
}

s32 Pick(IngameEvent& e) {                            // orig: 0x000F1530
    if (e.left == 0) return -1;
    s32 tries = 10, k = 0, v = 0;
    for (;;) {
        const s32 n = static_cast<s32>(e.left);
        k = crt::Rand() % n;                          // orig: 0x000F154B (CDQ / IDIV)
        --tries;
        v = k < static_cast<s32>(e.pool.size()) ? e.pool[k] : 0;
        if (tries == 0 || n <= 1) break;
        if (v != e.last) break;
    }
    if (v == e.last) {
        k = (k + 1) % static_cast<s32>(e.left);
        v = k < static_cast<s32>(e.pool.size()) ? e.pool[k] : 0;
    }
    e.last = v;
    if (static_cast<u32>(k) < e.pool.size()) {        // RemoveAtAndFill
        e.pool[k] = e.pool.back();
        e.pool.pop_back();
    }
    if (--e.left == 0) Refill(e);
    return v;
}

void Select(IngameEvent& e, const IngameEventContext& ctx) {   // orig: 0x000F15C0
    const LevelData& level = *ctx.level;
    if (e.type == 3) {
        const s32 idx = level.nibblersPickedUp;
        if (idx == level.nibblerCount) return;
        e.current = static_cast<s32>(e.lines.size()) <= idx ? static_cast<s32>(e.lines.size()) - 1 : idx;
    } else {
        e.current = Pick(e);
    }
    while (!e.lines.empty() && e.current >= 0) {
        IngameEventLine* l = e.lines[e.current].get();
        if (l->Valid(ctx)) return;
        // orig: delete, then RemoveAtAndFill(current) (the last line takes its slot).
        e.lines[e.current] = std::move(e.lines.back());
        e.lines.pop_back();
        e.last = -1;
        if (e.type == 2) {
            Refill(e);
            e.current = Pick(e);
        } else {
            e.current = e.type == 3 ? level.nibblersPickedUp : 0;
        }
        if (static_cast<s32>(e.lines.size()) <= e.current) e.current = static_cast<s32>(e.lines.size()) - 1;
    }
}

}  // namespace

namespace {
// orig: 0x000D2C20 (QueryPerformanceCounter into a 64-bit value; the frequency's reciprocal is computed once).
u64 Clock(const IngameEventContext* ctx) { return ctx && ctx->queryCounter ? ctx->queryCounter() : 0; }
}  // namespace

IngameEvent::~IngameEvent() = default;

bool IngameEvent::TestCond(u32 kind, s32 n) {         // orig: 0x000F1000
    auto it = conditions.find(kind);                  // 0x001077C0 (the first condition of the type)
    if (it == conditions.end()) return true;
    for (IngameEventCondition& c : it->second) {      // (the chain +4 is in file order: AddCondition 0x000F0CD0)
        if (n == 0) return false;
        bool ok = false;
        switch (kind) {
        case 1: {                                     // orig: vf04 0x000F19A0, a counter
            const s32 m = static_cast<s32>(std::bit_cast<f32>(c.value));   // FLD +8; _ftol (truncation)
            const s32 k = static_cast<s32>(c.count);  // FLD +0xC; _ftol
            c.count = static_cast<f32>(double(c.count) + 1.0f);   // FADD [0x00293A98]; FSTP +0xC
            if (m == 0) {                             // (the original's IDIV faults)
                NotPorted("IngameEvent: a type-1 condition with the value 0 (IDIV by zero)");
                break;
            }
            ok = k % m == 0;                          // CDQ; IDIV
            break;
        }
        case 2: {                                     // orig: vf04 0x000F1D40 (the triggering character's label)
            const GameObject* ch = arg ? arg->character : nullptr;   // 0x000F0C10
            if (!ch) break;
            const char* s = c.text ? c.text->c_str() : "";   // (a NULL string: the original reads address 0)
            const char* label = ch->label ? ch->label->c_str() : "";   // (+0x34 NULL: the original crashes)
            const size_t len = std::strlen(s);
            if (len == 1 && s[0] == '*') ok = true;
            else if (len >= 2 && s[len - 1] == '*') ok = crt::StrNICmp(label, s, len - 1) == 0;   // _strnicmp
            else ok = crt::StrICmp(label, s) == 0;    // _stricmp
            break;
        }
        case 3: {                                     // orig: vf04 0x000F1DE0 (the triggering object's definition name)
            const GameObject* ch = arg ? arg->character : nullptr;   // 0x000F0C10
            if (!ch) break;
            const char* s = c.text ? c.text->c_str() : "";   // (a NULL string: the original reads address 0)
            const char* name = ch->def->name.c_str(); // (+0x10)->+0x10
            const size_t len = std::strlen(s);
            if (len == 1 && s[0] == '*') ok = true;
            else if (len >= 2 && s[len - 1] == '*') ok = crt::StrNICmp(name, s, len - 1) == 0;   // _strnicmp
            else ok = crt::StrICmp(name, s) == 0;     // _stricmp
            break;
        }
        case 4: {                                     // orig: vf04 0x000F1E90 (the object word +4: a player's weapon)
            const GameObject* o = arg ? arg->object : nullptr;   // 0x000F0C20
            if (!o) break;
            const char* s = c.text ? c.text->c_str() : "";
            ok = crt::StrICmp(s, "*") == 0 || crt::StrICmp(o->def->name.c_str(), s) == 0;   // 0x002A5A48; _stricmp
            break;
        }
        default: {                                    // type 0: vf04 0x000F1910, a minimum time since the last play
            const u64 now = Clock(context);           // 0x000D2C20
            const f32 minTime = std::bit_cast<f32>(c.value);   // +8
            if (minTime < 0.0f || std::isnan(minTime)) break;   // FCOMP [0.0]; TEST AH,1; JNE (< or NaN: false)
            // 0x000D2BC0: now - stamp (SUB / SBB); 0x000D2C10: FILD qword; FMUL [0x002EB4F0] (the float 1 / frequency)
            const f32 inv = static_cast<f32>(1.0 / double(context ? context->counterFrequency : 1000000));
            const double elapsed = double(static_cast<s64>(now - c.stamp)) * inv;
            ok = !(elapsed < minTime);                // FCOMP; TEST AH,1; JNE (<: false)
            break;
        }
        }
        if (ok) return true;                          // c->vf4(e)
        --n;
    }
    return false;
}

bool IngameEvent::Test(const IngameEventArg& ev, const IngameEventContext& ctx) {   // orig: 0x000F1250
    arg = &ev;                                        // +0x2C
    context = &ctx;
    return TestCond(0, 1) && TestCond(4, -1) && TestCond(3, -1) && TestCond(2, -1) && TestCond(1, 1);
}

void IngameEvent::Play(const IngameEventContext& ctx) {   // orig: 0x000F16C0
    const s32 n = static_cast<s32>(lines.size());     // +0x10
    if (n == 0) return;
    switch (type) {                                   // +0x1C
    case 0:
        for (s32 i = 0; i < n; ++i) (void)lines[static_cast<size_t>(i)]->Play(ctx);   // vf04
        last = -1;
        return;
    case 1: {
        s32 k;
        do k = crt::Rand() % n;                       // rand 0x000F1701 (CDQ; IDIV)
        while (n > 1 && k == last);
        if (lines[static_cast<size_t>(k)]->Play(ctx)) last = k;
        return;
    }
    case 3: {
        IngameEventLine* l = lines[static_cast<size_t>(current)].get();   // (+0x24 unchecked, as the original)
        if (!l->Play(ctx)) l->Release();                 // vf10 after a failed play (0x000F174D)
        Select(*this, ctx);                           // 0x000F15C0
        return;
    }
    case 2: {
        bool played = false;
        if (static_cast<u32>(n) > 1) {                // CMP; JBE
            if (lines[static_cast<size_t>(current)]->Play(ctx)) {
                Select(*this, ctx);
                played = true;
            }
        } else {
            if (lines[0]->Play(ctx)) {
                played = true;
                Select(*this, ctx);
            }
            last = 0;
        }
        if (played) {                                 // 0x000F17AF: the first type-0 condition (0x001077C0) ->vf08
            auto it = conditions.find(0);
            if (it != conditions.end() && !it->second.empty()) it->second.front().stamp = Clock(&ctx);   // 0x000F18E0
        }
        return;
    }
    default:
        return;
    }
}

void IngameEvent::ResetIndexed(const IngameEventContext& ctx) {   // orig: 0x000F17D0
    if (type != 3) return;                            // +0x1C
    if (current >= 0 && current < static_cast<s32>(lines.size()))   // +0x24 against +0x10 (signed)
        lines[static_cast<size_t>(current)]->Release();   // vf10
    Select(*this, ctx);                               // tail jump 0x000F15C0
}

void IngameEventSet::ResetIndexed(const IngameEventContext& ctx) {   // orig: 0x000F2980
    const size_t n = events.size();                   // +0x10 (read once)
    for (size_t i = 0; i < n; ++i)
        if (events[i]->type == 3) events[i]->ResetIndexed(ctx);   // +0x1C == 3
}

void IngameEventSet::Trigger(u32 kind, const IngameEventArg& ev, const IngameEventContext& ctx) {   // 0x000F28F0
    if (!enabled) return;                             // +0x1C
    const size_t n = events.size();                   // +0x10 (read once)
    for (size_t i = 0; i < n; ++i) {
        IngameEvent& e = *events[i];
        if (e.id != kind || !e.Test(ev, ctx)) continue;   // +0x40
        if (e.id == 9 && ctx.level && std::strncmp(ctx.level->name.c_str(), "Level3", 6) == 0 &&   // S+0x0C
            crt::Rand() % 3 != 0)                     // rand 0x000F294E
            continue;
        e.Play(ctx);
    }
}

bool IngameEventSet::Parse(const std::vector<u8>* file, LipSyncPlayer* lipPlayer, const IngameEventContext& ctx) {
    if (!events.empty()) return false;                // orig: 0x000F2A80 (mgr+0x10 != 0)
    if (!file) return false;
    Reader r{*file};
    char magic[33] = {};
    for (int i = 0; i < 32; ++i) magic[i] = r.pos + i < file->size() ? static_cast<char>((*file)[r.pos + i]) : '\0';
    r.pos += 32;
    if (std::strcmp(magic, "IngameEvent v0.30") != 0) return false;
    g_defaultDb = r.Str();                            // orig: 0x000F0020
    g_lipPlayer = lipPlayer;                          // orig: 0x000F0010
    const u32 n = r.U32();
    for (u32 i = 0; i < n; ++i) {
        auto e = std::make_unique<IngameEvent>();     // orig: 0x000F10C0(-1, 1), Load 0x000F1800
        e->id = r.U32();
        e->random = r.U32() == 1;
        e->type = r.U32();
        const u32 conds = r.U32();                    // orig: LoadConditions 0x000F0E80
        for (u32 c = 0; c < conds; ++c) {
            IngameEventCondition cond;
            cond.type = r.U32();
            if (cond.type > 4) break;                 // (the rest is not read: the stream loses its place)
            if (cond.type <= 1) cond.value = r.U32();
            else cond.text = r.Str();
            e->conditions[cond.type].push_back(std::move(cond));
            ++e->conditionCount;
        }
        const u32 total = r.U32();                    // orig: LoadLines 0x000F12D0
        for (u32 k = 0; k < total; ++k) {
            const u32 lt = r.U32();
            std::unique_ptr<IngameEventLine> line;
            if (lt == 0) {
                if (e->random && total >= 2) line = std::make_unique<RandomLine>();
                else line = std::make_unique<DialogueLine>();
            } else if (lt == 1) {
                line = std::make_unique<HintLine>();
            } else {
                continue;
            }
            if (line->Load(r, ctx)) e->lines.push_back(std::move(line));
            // (else deleted here: the line counter may free the default database name)
        }
        if (e->type == 2) {
            Refill(*e);
            Select(*e, ctx);
        } else if (e->type == 3) {
            Select(*e, ctx);
        }
        events.push_back(std::move(e));
    }
    g_lipPlayer = nullptr;
    return true;
}

namespace {
bool g_requestStream = false;                         // orig: loader +0x14 (the open NiUDSStream)
bool g_requestLoading = false;                        // orig: loader +0x20 (its job not finished)
std::vector<u8> g_requestData;                        // orig: loader +0x18 / +0x1C (the job's buffer and size)
}  // namespace

void ResetSoundRequests() {
    g_requests.clear();
    g_requestStream = g_requestLoading = false;
    g_requestData.clear();
}

void StepSoundRequests(const LevelData* level, s32 sessionState,
                       const std::function<bool(const std::string& path, std::vector<u8>& out)>& stream) {
    // orig: 0x000F2710
    if (!level || sessionState == 2) return;          // DAT_002DAC64, S+0x10
    if (sessionState == 4)                            // Session::NextStage 0x000C26C0 walk, Stage::IsLoaded 0x000CE400
        for (const auto& st : level->stages)
            if (st && !st->IsLoaded()) return;
    if (!g_requestStream && !g_requests.empty()) {    // +0x14 == 0, the ring not empty
        g_requestLoading = true;                      // +0x20 = 1
        g_requestStream = true;                       // new(0x6C) NiUDSStream(front's path, loader) -> +0x14
        g_requestData.clear();
        if (stream && stream(g_requests.front().path, g_requestData))
            g_requestLoading = false;                 // the job 0x000F2030: the file read, +0x20 = 0
    }
    if (!g_requestStream) return;
    if (g_requestLoading) return;                     // NiUDSStream::Pump 0x0013F620 (a job that never ends)
    // orig: 0x000F2330 (with a level): the file object of the bytes, named by the path, to the line's callback
    const IngameSoundRequest req = g_requests.front();
    ni::NiPointer<ni::NiUDSFileObject> file(new ni::NiUDSFileObject);   // new(0xD0) 0x0011FA50, 0x0011FA70(data, size, 0)
    file->data = std::move(g_requestData);
    file->name = req.path;                            // NiObjectNET::SetName
    if (req.line) req.line->SoundLoaded(file.Get());  // (+4)->vf00(file); else the file released
    g_requestStream = false;                          // the stream released, +0x18 / +0x1C = 0
    g_requestData.clear();
    if (!g_requests.empty()) g_requests.erase(g_requests.begin());   // the ring's head advanced, the request deleted
}

}  // namespace fut::game
