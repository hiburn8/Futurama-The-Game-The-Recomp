// The in-game HUD; see Hud.h. x87 arithmetic in the original's order (double = the register, F() = a float store).
#include "game/level/Hud.h"

#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiStream.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/object/ChickenWalker.h"
#include "game/object/GenericNpc.h"
#include "game/sound/GameSound.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fut::game {

using ni::detail::F;

namespace {

// orig: [0x0029BC18] / [0x0029BC14]: 1 / 640 and 1 / 480 as floats (not exact)
const f32 kInv640 = std::bit_cast<f32>(0x3ACCCCCDu);
const f32 kInv480 = std::bit_cast<f32>(0x3B088889u);
constexpr f32 kPi = std::bit_cast<f32>(0x40490FDBu);   // [0x002A53B4]
// DAT_002EED64: pi / 180 from a static initialiser (0x3C8EFA36, one ulp above the nearest float: O, the f6 snapshot)
const f32 kDegToRad = std::bit_cast<f32>(0x3C8EFA36u);

// The layout globals (static initialisers; the values the f6 snapshot holds, O).
constexpr f32 kHeadX = 36.0f;                         // 0x002EEE2C = [0x002A5314] (0x00217580)
constexpr f32 kHeadY = 49.0f;                         // 0x002EEDC0 = [0x002A5318] (0x00217590)
constexpr f32 kHeadSize = 107.0f;                     // 0x002EED6C = [0x002A531C] + [0x002A53A8] 19 (0x00217560)
constexpr f32 kLivesX = 56.0f;                        // 0x002EF44C = (88 0.5 + 36) - 48 0.5 (0x0021A420)
constexpr f32 kLivesY = 161.0f;                       // 0x002EF36C = (88 + 49) + 24 (0x0021A450)
const f32 kLivesFlashX = std::bit_cast<f32>(0x3E39999Au);   // 0x002EF384 = ((48 + 56) + 12) / 640 (0x0021A470)
const f32 kLivesFlashY = std::bit_cast<f32>(0x3F255555u);   // 0x002EF3D8 = 1 - ((48 0.5 + 161) - 15) / 480
// 0x002EF464 = [0x002F9274] 4 (0x0021A4C0): 0, because 0x002F9274 (= FPATAN(1, 1) 4 = pi, 0x002268B0) is set by a
// later static initialiser (O: the snapshot holds 0)
constexpr f32 kLivesFlashSpin = 0.0f;
constexpr f32 kItemsX = 480.0f;                       // 0x002EF1B4 = 640 - ([0x002A54FC] 32 + 128) (0x00219720)
const f32 kItemsFlashSpin = std::bit_cast<f32>(0x41490FDBu);   // 0x002EF294 = [0x002A54E4] pi 4 (0x00219700)
constexpr f32 kNibblerX = 56.0f;                      // 0x002EF53C = [0x002EF620] (0x0021B1F0)
constexpr f32 kNibblerY = 352.0f;                     // 0x002EF5E8 = 480 - (64 + 64) (0x0021B200)
constexpr f32 kCollectX = 520.0f;                     // 0x002EEB50 = 640 - (64 + 56) (0x00216780)
constexpr f32 kCollectY = 352.0f;                     // 0x002EEBFC = 480 - (64 + 64) (0x002167A0)
constexpr f32 kWeaponsX = 516.0f;                     // 0x002EFAC0 = 640 - (88 + 36) (0x0021DBC0)
constexpr f32 kWeaponsY = 49.0f;                      // 0x002EFAD8 = [0x002A56FC] (0x0021DBE0)
// the counters' text positions (static initialisers; O: the f6 snapshot)
const f32 kCollectTextX = std::bit_cast<f32>(0x3F5CCCCDu);   // 0x002EEB94
const f32 kCollectTextY = std::bit_cast<f32>(0x3E088886u);   // 0x002EEBBC
const f32 kNibblerTextX = std::bit_cast<f32>(0x3E0CCCCDu);   // 0x002EF580
const f32 kNibblerTextY = std::bit_cast<f32>(0x3E088886u);   // 0x002EF5A8

// The counters' text object (the ctors 0x000EB3C0 / 0x000E8300 / 0x000EAF00): TextObject::Create(0), App->vf14(),
// +0x60 = 0 (centred), +0x68 = 0 (no wrap), SetScale(1.5, 1.7).
std::unique_ptr<text::TextObject> MakeCounterText(Hud& hud) {
    std::unique_ptr<text::TextObject> t(text::TextObject::Create(0, hud.readFile, hud.level.renderer));
    if (!t) return t;
    t->style.hAlign = 0;
    t->style.wordWrap = false;
    t->SetScale(1.5f, 1.7f);                          // 0x3FC00000, 0x3FD9999A
    return t;
}

// sprintf, then 0x000D2CB0 (the 8-bit string widened, at most 0x40 characters), at (x, y): AddString(.., -1).
void SetCounterText(text::TextObject* t, const char* s, f32 x, f32 y) {
    if (!t) return;
    t->RemoveAll();                                   // 0x00099080
    std::u16string w;
    for (const char* c = s; *c && w.size() < 0x3F; ++c) w.push_back(char16_t(u8(*c)));
    t->x = x, t->y = y;                               // +0x4C / +0x50
    t->AddString(w, -1.0f);                           // 0x0009A070
}

// The elements' Draw (0x000EB3A0 / 0x000EADB0): when active, the text's camera update and Click.
void DrawText(const HudElement& e, text::TextObject* t) {
    if (!e.IsActive() || !t) return;
    t->UpdateCamera();                                // 0x00098D70
    t->Click();                                       // 0x00098DB0
}

// The pulse of a counter's icon (HUDLives 0x000EB175.., HUDNibblerCnt 0x000EB6D5.., HUDCollectableCnt 0x000E8605..):
// the polygon re-centred at its vertices' mean with the half size `half` (+0x48 / +0x44) shrinking by 10 pixels a
// second to size / 2; `pulsing` cleared when it got there.
void Pulse(ni::NiScreenPolygon& p, f32 size, f32& half, u8& pulsing) {
    auto& v = p.vertices;                             // (+0x10)
    const f32 t8 = F(double(v[0].x) + v[1].x);
    const f32 t4 = F((double(v[0].y) + v[1].y) + v[2].y);
    const double cx = ((double(t8) + v[2].x) + v[3].x) * 0.25f;   // [0x00293B50] (kept)
    const f32 cy = F((double(t4) + v[3].y) * 0.25f);
    const double h = double(half) - double(g_frameStep) * 10.0f;   // FLD dt; FMUL [0x00293C2C]; FSUBR +0x48
    half = F(h);                                      // FST (the register keeps h)
    const f32 lo = F(double(size) * 0.5f);
    if (!(h > lo)) half = lo;                         // FCOMP; TEST AH,0x41; JP
    v[0].x = F(cx - double(half) * kInv640);
    v[0].y = F(double(cy) - double(half) * kInv480);
    v[1].x = F(double(half) * kInv640 + cx);
    v[1].y = F(double(cy) - double(half) * kInv480);
    v[2].x = F(double(half) * kInv640 + cx);
    v[2].y = F(double(half) * kInv480 + cy);
    v[3].x = F(cx - double(half) * kInv640);
    v[3].y = F(double(half) * kInv480 + cy);
    p.flags |= 1;
    if (half == lo) pulsing = 0;                      // FCOMP; TEST AH,0x44; JP
}

// The material alpha of a polygon (its property state's material slot, when there is one), one revision.
void SetPolygonAlpha(ni::NiScreenPolygon* p, f32 alpha) {
    if (ni::NiMaterialProperty* m = MaterialOf(p)) m->alpha = alpha, ++m->revision;
}

// orig: 0x000E8DA0 (HUDHead's and HUDHealth's vf05): a change of +0x15 sets the polygon +0x34's alpha to 1 / 0.
void SetActiveWithPolygon(HudElement& e, bool on, ni::NiScreenPolygon* p) {
    if (u8(on) == e.active) return;
    e.active = u8(on);
    SetPolygonAlpha(p, on ? 1.0f : 0.0f);
}

// orig: the flash (0x54 bytes; ctor 0x000EBF40(position, size, owner, duration, split, spin) -> 0x000EBBD0): two
// identical polygons of the level's "respawn.tga" (alpha 0x0D, no z test) spinning in opposite directions, their
// alpha rising to 1 at `split` of the duration and falling back to 0.
struct HudFlash {
    f32 time = 0, duration = 0, split = 0, spin = 0;  // +0x00 / +0x04 / +0x08 / +0x0C
    ni::NiPoint3 offset[4];                           // +0x10 (polygon 0's vertices minus the centre)
    ni::NiPoint3 centre;                              // +0x40
    ni::NiPointer<ni::NiScreenPolygon> poly[2];       // +0x4C / +0x50

    HudFlash(Hud& hud, const f32 pos[2], const f32 size[2], f32 dur, f32 sp, f32 spinSpeed)
        : time(dur), duration(dur), split(sp), spin(spinSpeed) {   // orig: 0x000EBF40 (+0 = +4 = duration)
        ni::NiObject* tex = hud.level.FindTexture("respawn.tga");   // 0x000C2A80 (0x002A281C)
        if (!tex) return;
        for (auto& p : poly)                          // e5c00(tex, w, h, x, y, white, alpha, 0, 0, 0, scene camera)
            p = hud.MakePolygon(tex, size[0], size[1], pos[0], pos[1], {1, 1, 1}, true, nullptr, nullptr);
        if (!poly[0] || !poly[1]) return;
        const auto& v = poly[0]->vertices;            // the centre of vertices 1 and 3
        const double z13 = double(v[3].z) + v[1].z;
        const f32 y13 = F(double(v[3].y) + v[1].y);
        const f32 x13 = F(double(v[3].x) + v[1].x);
        centre = {F(double(x13) * 0.5f), F(double(y13) * 0.5f), F(z13 * 0.5f)};
        for (int k = 0; k < 4; ++k)
            offset[k] = {F(double(v[k].x) - centre.x), F(double(v[k].y) - centre.y),
                         F(double(v[k].z) - centre.z)};
        auto* a = new ni::NiAlphaProperty;            // flags (0xEC & 0xFC0D) | 0x0D
        a->flags = 0x000D;
        auto* z = new ni::NiZBufferProperty;          // flags 3 & ~3, test 3
        z->flags = 0;
        z->testFunction = 3;
        for (auto& p : poly) {
            p->AttachProperty(a);
            p->AttachProperty(z);
            if (ni::NiMaterialProperty* m = MaterialOf(p.Get())) m->alpha = 0.0f, ++m->revision;
        }
    }
    void Attach(ni::NiCamera* cam) {                  // orig: 0x000EC130 (both again: a second slot each)
        if (!cam) return;
        cam->AttachScreenPolygon(poly[0].Get());
        cam->AttachScreenPolygon(poly[1].Get());
    }
    void Detach(ni::NiCamera* cam) {                  // orig: 0x000EB950
        if (!cam) return;
        cam->DetachScreenPolygon(poly[0].Get());
        cam->DetachScreenPolygon(poly[1].Get());
    }
    // orig: 0x000EB980 (polygon, speed): the vertices = Rz(speed time) (0x001030F0) offset + centre
    void Spin(ni::NiScreenPolygon* p, f32 speed) {
        if (!p) return;
        const f32 angle = F(double(speed) * time);
        const f32 c = F(std::cos(double(angle))), s = F(std::sin(double(angle)));   // FCOS / FSIN (#35)
        for (int k = 0; k < 4; ++k) {
            const ni::NiPoint3& o = offset[k];
            const double x = (double(s) * o.y + 0.0 * o.z) + double(c) * o.x;
            const double y = (double(-s) * o.x + double(c) * o.y) + 0.0 * o.z;
            const f32 z = F((0.0 * o.x + 0.0 * o.y) + 1.0 * o.z);
            p->vertices[k] = {F(x + centre.x), F(y + centre.y), F(double(z) + centre.z)};
        }
        p->flags |= 1;
    }
    bool Update() {                                   // orig: 0x000EBFC0
        if (!(time < duration)) {                     // FCOMP; TEST AH,1; JNE
            for (auto& p : poly)
                if (ni::NiMaterialProperty* m = MaterialOf(p.Get())) m->alpha = 0.0f, ++m->revision;
            return false;
        }
        const double t = double(time) / duration;
        const f32 a = F(t > split || std::isnan(t) ? 1.0 - (t - split) / (1.0 - split) : t / split);
        for (auto& p : poly) {
            if (!p) continue;
            f32 v = a;
            if (!(a <= 1.0f) && !std::isnan(a)) v = 1.0f;   // FCOMP 1; TEST AH,0x41; JNE
            else if (a < 0.0f) v = 0.0f;              // FCOMP 0; TEST AH,5; JP
            if (ni::NiMaterialProperty* m = MaterialOf(p.Get())) m->alpha = v, ++m->revision;
        }
        Spin(poly[0].Get(), spin);
        Spin(poly[1].Get(), F(-double(spin)));
        time = F(double(g_frameStep) + time);
        return true;
    }
};

// ---- the elements -----------------------------------------------------------------------------------------------
// orig: the shared queue of the flash texts (DAT_002EF744: new(0x10) by the first HUDText, deleted by the last; the
// instance count DAT_002EF74C, the ticket counter DAT_002EF748 = 0 at the queue's creation): a ring of tickets, the
// one at the head is the text on screen.
struct HudTextQueue {
    std::vector<s32> data = std::vector<s32>(1);      // +0x00 (new(4))
    u32 capacity = 1;                                 // +0x04
    s32 head = -1;                                    // +0x08 (-1: empty)
    u32 tail = 0;                                     // +0x0C
    s32 Front() const { return data[u32(head)]; }
    // orig: 0x000EC500 (ticket): into an empty ring at 0; a full one (head == tail) grows to twice its capacity first;
    // the tail wraps at the capacity.
    void Push(s32 v) {
        if (head == -1) {
            head = 0;
            data[0] = v;
            tail = 1;
        } else {
            if (u32(head) == tail) Grow(capacity * 2);   // 0x000EC200
            data[tail] = v;
            ++tail;
        }
        if (tail == capacity) tail = 0;
    }
    // orig: 0x000EC200 (n): the tickets from the head on copied into new(4 n) in order (head .. tail, or head .. the
    // end and 0 .. tail when wrapped); head 0, tail the count.
    void Grow(u32 n) {
        std::vector<s32> d(n);
        u32 k = 0, i = u32(head);
        if (i < tail) {
            do d[k++] = data[i++];
            while (i < tail);
        } else {
            for (; i < capacity; ++i) d[k++] = data[i];
            for (u32 j = 0; j < tail; ++j) d[k++] = data[j];
        }
        tail = k;
        head = 0;
        capacity = n;
        data = std::move(d);
    }
    // orig: 0x000EC1C0: the head moves on (wrapping at the capacity); meeting the tail empties the ring.
    void Pop() {
        ++head;
        if (u32(head) == capacity) head = 0;
        if (u32(head) == tail) head = -1, tail = 0;
    }
};
std::unique_ptr<HudTextQueue> g_hudTextQueue;         // DAT_002EF744
s32 g_hudTextCount = 0;                               // DAT_002EF74C
s32 g_hudTextTicket = 0;                              // DAT_002EF748

// orig: HUDText (0x44 bytes, ctor 0x000EC2A0(name), vtable 0x002A55C8: vf00 0x000EC740, vf01 0x000EC3C0, vf02
// 0x000EC160, vf03 0x00011280, vf04 0x000EC610, vf05 0x000EC190; dtor 0x000EC560): a language-database string
// flashed in the screen's centre, growing and fading out, one element's at a time (the shared queue). The bases of
// HUD1Up (0x000E5FF0), HUDChargeText (0x000E6890) and HUDCheckpoint (0x000E6950).
struct HudText : HudElement {
    const char* id = nullptr;                         // +0x24 (the language database's id)
    f32 duration = -1.0f;                             // +0x28
    f32 time = 0.0f;                                  // +0x2C
    f32 scale = 0.0f;                                 // +0x30
    std::unique_ptr<text::TextObject> object;         // +0x34
    const char16_t* string = nullptr;                 // +0x38
    s32 ticket = -1;                                  // +0x3C
    u32 sound = 0;                                    // +0x40

    HudText(Hud& h, const char* n) : HudElement(h, n) {
        if (++g_hudTextCount == 1) {                  // the first: the ticket counter 0, the queue
            g_hudTextTicket = 0;
            g_hudTextQueue = std::make_unique<HudTextQueue>();   // new(0x10): {new(4), 1, -1, 0}
        }
        // TextObject::Create(0), App->vf14() (0x00098F80); +0x60 = +0x64 = 0, the colour (1, 1, 1), SetScale(2.0, 2.3),
        // +0x68 = 0
        object.reset(text::TextObject::Create(0, hud.readFile, hud.level.renderer));
        if (object) {
            object->style.hAlign = 0;
            object->style.vAlign = 0;
            object->color = {1.0f, 1.0f, 1.0f};
            object->SetScale(2.0f, std::bit_cast<f32>(0x40133333u));
            object->style.wordWrap = false;
        }
    }
    ~HudText() override {                             // orig: 0x000EC560
        if (--g_hudTextCount == 0) g_hudTextQueue.reset();
        if (object) object->RemoveAll();
    }
    // orig: vf01 0x000EC3C0: active (vf18), the time not past the duration, a string, and its ticket at the queue's
    // head: at time 0 the sound +0x40 (Sound_PlaySFX(+0x40, 0)); r = time / duration, the scale F(r +0x30 2.5)
    // ([0x00293B10]) at most +0x30, the alpha 1, past 0.4 ([0x00293B28]) F((1 - r) 1.6667) ([0x0029C098]); the text
    // emptied, SetScale(s, F(s 1.2)) ([0x0029A520]), at (0.5, 0.5), AddString(+0x38, alpha); time += dt; past the
    // duration the string is dropped and the queue moves on.
    void Update() override {
        if (!IsActive()) return;
        if (time > duration) return;                  // FCOMP +0x28; TEST AH,0x41; JE (a NaN goes on)
        if (!string) return;
        HudTextQueue* q = g_hudTextQueue.get();
        if (!q || q->head == -1 || ticket != q->Front()) return;
        if (time == 0.0f && sound) (void)hud.level.Sound().PlaySfx(sound, false);   // TEST AH,0x44; JP
        const double r = double(time) / duration;
        const double sr = r * scale * 2.5f;           // FLD ST(0); FMUL +0x30; FMUL [0x00293B10]
        f32 s = F(sr);                                // FST
        if (!(sr <= scale)) s = scale;                // FCOMP +0x30; TEST AH,0x41; JNE (NaN kept)
        f32 alpha = 1.0f;
        if (r > 0.4f) alpha = F((1.0 - r) * std::bit_cast<f32>(0x3FD55555u));   // FCOM; TEST AH,0x41; JNE
        if (object) {
            object->RemoveAll();                      // 0x00099080
            object->SetScale(s, F(double(s) * 1.2f)); // 0x00098C00
            object->x = 0.5f, object->y = 0.5f;       // +0x4C / +0x50 = 0x3F000000
            (void)object->AddString(string, alpha);   // 0x0009A070
        }
        const double t = double(g_frameStep) + time;  // FLD dt; FADD +0x2C; FST
        time = F(t);
        if (!(t > duration)) return;                  // FCOMP +0x28; TEST AH,0x41; JNE
        string = nullptr;
        q->Pop();                                     // 0x000EC1C0
        ticket = -1;
    }
    // orig: vf02 0x000EC160: active (vf18) with a string: the text's camera update and Click.
    void Draw() override {
        if (!IsActive() || !string || !object) return;
        object->UpdateCamera();                       // 0x00098D70
        object->Click();                              // 0x00098DB0
    }
    // orig: vf04 0x000EC610 (cmd, v; _stricmp): "show" (0x0029A420): queued already: only the text on screen takes
    // the duration v (1.5 ([0x002A55C4]) for v < 0) and restarts; else the duration, time 0, the string of +0x24
    // (TextManager::GetText(+0x24, 0) 0x000D9C80; none: time = duration + 1) and a new ticket at the queue's tail.
    // "activate" / "deactivate" (0x002A55F0 / 0x002A55E4): vf05(1) / vf05(0).
    void Command(const char* cmd, f32 v) override {
        if (!cmd) return;
        if (crt::StrICmp(cmd, "show") == 0) {
            HudTextQueue* q = g_hudTextQueue.get();
            if (ticket != -1) {
                if (!q || ticket != q->Front()) return;
                duration = v;
                if (v < 0.0f) duration = std::bit_cast<f32>(0x3FC00000u);   // FCOMP 0; TEST AH,5; JP
                time = 0.0f;
                return;
            }
            duration = v;
            if (v < 0.0f) duration = std::bit_cast<f32>(0x3FC00000u);
            time = 0.0f;
            string = hud.level.text && id ? hud.level.text->GetText(id, nullptr) : nullptr;
            if (!string) {
                time = F(double(duration) + 1.0f);    // FADD [0x00293A98]
                return;
            }
            ticket = g_hudTextTicket++;
            if (q) q->Push(ticket);                   // 0x000EC500
        } else if (crt::StrICmp(cmd, "activate") == 0) {
            SetActive(true);
        } else if (crt::StrICmp(cmd, "deactivate") == 0) {
            SetActive(false);
        }
    }
    // orig: vf05 0x000EC190: +0x15 follows (nothing else).
    void SetActive(bool on) override {
        if (active && !on) active = 0;
        else if (!active && on) active = 1;
    }
};

// orig: HUD1Up (0x000E5FF0, vtable 0x002A5134): "1up", the colour (0.9, 0.7, 0), "1_UP", scale 1.5.
struct Hud1Up : HudText {
    explicit Hud1Up(Hud& h) : HudText(h, "1up") {     // 0x002A3478
        if (object) object->color = {std::bit_cast<f32>(0x3F666666u), std::bit_cast<f32>(0x3F333333u), 0.0f};
        id = "1_UP";                                  // 0x002A5150
        scale = 1.5f;
    }
};

// orig: HUDChargeText (0x000E6890, vtable 0x002A5198): "chargetext", the colour (0, 0.5, 0.7), "CHARGE_UP", 1.5.
struct HudChargeText : HudText {
    explicit HudChargeText(Hud& h) : HudText(h, "chargetext") {   // 0x0029CCF8
        if (object) object->color = {0.0f, 0.5f, std::bit_cast<f32>(0x3F333333u)};
        id = "CHARGE_UP";                             // 0x002A51B4
        scale = 1.5f;
    }
};

// orig: HUDCheckpoint (0x000E6950, vtable 0x002A51C0, dtor 0x000E68F0: Sound_Release(+0x40) first): "checkpoint", the
// colour (1, 1, 1), "CHECKPOINT", 1.8, the sound Sound_Load("sounds/HUD/Checkpoint.wav", 0).
struct HudCheckpoint : HudText {
    explicit HudCheckpoint(Hud& h) : HudText(h, "checkpoint") {   // 0x002A5204
        if (object) object->color = {1.0f, 1.0f, 1.0f};
        id = "CHECKPOINT";                            // 0x002A51DC
        scale = std::bit_cast<f32>(0x3FE66666u);
        sound = hud.level.loadSound ? hud.level.loadSound("sounds/HUD/Checkpoint.wav", 0) : 0u;   // 0x002A51E8
    }
    ~HudCheckpoint() override { hud.level.Sound().Release(sound); }   // 0x000C9880
};

// orig: HUDInfo (0x3C bytes, ctor 0x000EA100, vtable 0x002A53EC: vf00 0x000EA770, vf01 0x000EA200, vf02 0x000EA320,
// vf03 0x000EA610, vf04 0x000EA750, vf05 0x000EA3E0): up to three hint texts (the language database's string of
// an id, natives 381 / 382 / 383) stacked above the screen's bottom, each fading in from its add.
struct HudInfo : HudElement {
    // orig: an entry (0x28 bytes, ctor 0x000EA4A0(name, text object), delete 0x000EA1A0)
    struct Entry {
        u16 handle = 0;                               // +0x00 (the text object's string id)
        std::string name;                             // +0x04 (a copy)
        std::u16string text;                          // +0x08
        s32 refs = 0;                                 // +0x0C
        u8 dead = 0;                                  // +0x10
        std::unique_ptr<f32[]> timer;                 // +0x14 (new(8): elapsed, limit)
        text::TextObject* object = nullptr;           // +0x18
        f32 fade = std::bit_cast<f32>(0x3FE66666u);   // +0x1C (1.8)
        s32 lines = 0;                                // +0x20
        s32 slot = -1;                                // +0x24
        // orig: 0x000EA030: TextManager::Find(name, all databases) 0x000D9C30; its string, else the name widened
        // (0x000D2CB0); the lines = 1 + the '\n' characters.
        void Load(const text::TextManager* tm) {
            const text::LanguageDb::Entry* f = tm ? tm->Find(name, nullptr) : nullptr;
            text.clear();
            if (f && f->hasText) {
                text = f->text;
            } else {
                for (char c : name) text.push_back(char16_t(u8(c)));
            }
            lines = 1;
            for (char16_t c : text) lines += c == u'\n';
        }
        void Unshow() {                               // (the inline block of 0x000EA1A0 / 0x000EA3E0 / 0x000E9F80)
            if (!handle) return;
            object->RemoveString(handle);             // 0x00098FD0
            handle = 0;
            slot = -1;
        }
        // orig: 0x000E9EE0: false when dead, or when its timer ran out (elapsed += dt, not below the limit); else
        // the fade F(+0x1C - dt 3) ([0x0029563C]), 1.0 once the register is below 1.
        bool Update() {
            if (dead) return false;
            if (timer) {
                timer[0] = F(double(g_frameStep) + timer[0]);
                if (timer[0] >= timer[1]) return false;   // FCOMP +4; TEST AH,1; JNE (NaN kept)
            }
            const double f = double(fade) - double(g_frameStep) * 3.0f;
            fade = F(f);                              // FST
            if (f < 1.0) fade = 1.0f;                 // FCOMP [0x00293A98]; TEST AH,5; JP
            return true;
        }
        // orig: 0x000E9F40 (limit): the timer replaced by {0, limit}.
        void SetTimer(f32 limit) {
            timer = std::make_unique<f32[]>(2);
            timer[0] = 0.0f, timer[1] = limit;
        }
        // orig: 0x000E9F80 (slot): nothing when +0x24 is the slot (never: nothing sets +0x24 but -1); a shown string is
        // removed and the text object's scale set to (s, s + s), s = [App+0xE8 == 1 (NTSC)] 0.65 (0x3F266667) else
        // 0.55 (0x3F0CCCCD); then AddString(+8, alpha = (1 - fade) 1.25 + 1 / fade ([0x00297024]), 0 when not above
        // 0) at the text object's position.
        void Show(s32 at) {
            if (slot == at) return;
            if (handle) {
                object->RemoveString(handle);
                handle = 0;
                slot = -1;
                const f32 s = std::bit_cast<f32>(0x3F266667u);   // (the port runs the NTSC step: App+0xE8 = 1)
                object->SetScale(s, F(double(s) + s));
            }
            const double a = (1.0 - fade) * 1.25f + 1.0 / fade;
            const f32 alpha = a < 0.0 ? 0.0f : F(a);  // FCOMP [0x00293AB8]; TEST AH,5; JP (NaN kept)
            handle = object->AddString(text, alpha);  // 0x0009A070
        }
    };
    std::unique_ptr<text::TextObject> object;         // +0x24
    Entry* entries[3] = {};                           // +0x28 / +0x2C / +0x30
    u8 shown = 0;                                     // +0x34 (set by an add, never cleared)
    s32 count = 0;                                    // +0x38

    explicit HudInfo(Hud& hud) : HudElement(hud, "info") {   // 0x002A20FC
        // TextObject::Create(0), App->vf14() (0x00098F80); +0x60 = 0, +0x64 = 4, +0x68 = 0, +4 = 0x3ADED289
        object.reset(text::TextObject::Create(0, hud.readFile, hud.level.renderer));
        if (object) {
            object->style.hAlign = 0;
            object->style.vAlign = 4;
            object->style.wordWrap = false;
            object->style.letterSpacing = std::bit_cast<f32>(0x3ADED289u);
        }
    }
    ~HudInfo() override {                             // orig: 0x000EA510
        for (s32 i = 0; i < count; ++i) DeleteEntry(entries[i]);
        count = 0;
        if (object) object->RemoveAll();
    }
    static void DeleteEntry(Entry*& e) {              // orig: 0x000EA1A0(1) (the string removed, the blocks freed)
        if (!e) return;
        e->Unshow();
        delete e;
        e = nullptr;
    }
    Entry* Find(const char* n) const {                // orig: 0x000EA440 (_stricmp)
        for (s32 i = 0; i < count; ++i)
            if (crt::StrICmp(n, entries[i]->name.c_str()) == 0) return entries[i];
        return nullptr;
    }
    // orig: vf01 0x000EA200 (+0x15 and entries): each entry's 0x000E9EE0; with any finished, those are deleted and
    // the kept ones moved to the front in their order.
    void Update() override {
        if (!active || count <= 0) return;
        Entry* keep[3] = {};
        Entry* gone[3] = {};
        s32 nk = 0, ng = 0;
        for (s32 i = 0; i < count; ++i) {
            if (entries[i]->Update()) keep[nk++] = entries[i];
            else gone[ng++] = entries[i];
        }
        if (ng <= 0) return;
        for (s32 i = 0; i < ng; ++i) DeleteEntry(gone[i]);
        for (s32 i = 0; i < count; ++i) entries[i] = i < nk ? keep[i] : nullptr;
        count = nk;
    }
    // orig: vf02 0x000EA320 (+0x15): before the first add only the text's camera; then each frame the entries laid
    // out from the last one up: x 0.5, y from F(0.9 - (lines - 1) 0.05) ([0x00293C08], [0x00293BDC]), each entry
    // shown at its slot and y += F(its lines 0.05 + y); the camera's Update and Click; +0x34 = 1.
    void Draw() override {
        if (!active || !object) return;
        if (shown) {
            if (count > 0) {
                s32 total = 0;
                for (s32 i = 0; i < count; ++i) total += entries[i]->lines;
                f32 y = F(0.9f - double(total - 1) * 0.05f);   // FILD; FMUL; FSUBR; FSTP
                for (s32 i = count - 1; i >= 0; --i) {
                    Entry* e = entries[i];
                    object->y = y;                    // +0x50
                    object->x = 0.5f;                 // +0x4C
                    e->Show(i);
                    y = F(double(e->lines) * 0.05f + y);
                }
            }
        }
        object->UpdateCamera();                       // 0x00098D70
        object->Click();                              // 0x00098DB0
    }
    // orig: vf03 0x000EA610 (cmd, name): "add" (memcmp 4, 0x002A2104): +0x34 = 1; a full list drops its third entry;
    // the entry of the name (0x000EA440), else a new one appended; its +0xC + 1. "remove" (7, 0x002A2108): the
    // entry's +0xC - 1, dead at 0. "remove_all" (11, 0x002A2110): every entry's count to 0, dead.
    void Command(const char* cmd, const char* n) override {
        if (!n) return;
        if (std::memcmp(cmd, "add", 4) == 0) {
            shown = 1;
            if (count == 3) {
                DeleteEntry(entries[2]);
                --count;
            }
            Entry* e = Find(n);
            if (!e) {
                e = new Entry;                        // new(0x28) 0x000EA4A0(name, +0x24)
                e->name = n;
                e->object = object.get();
                e->Load(hud.level.text);              // 0x000EA030
                entries[count++] = e;
            }
            ++e->refs;
        } else if (std::memcmp(cmd, "remove", 7) == 0) {
            if (Entry* e = Find(n))
                if (--e->refs == 0) e->dead = 1;
        } else if (std::memcmp(cmd, "remove_all", 11) == 0) {
            for (s32 i = 0; i < count; ++i)
                while (entries[i]->refs > 0)
                    if (--entries[i]->refs == 0) entries[i]->dead = 1;
        }
    }
    // orig: vf04 0x000EA750 (name, limit): the entry's timer (0x000E9F40).
    void Command(const char* n, f32 limit) override {
        if (Entry* e = Find(n)) e->SetTimer(limit);
    }
    // orig: vf05 0x000EA3E0: a change of +0x15; hiding takes every entry's string off the text object.
    void SetActive(bool on) override {
        if (u8(on) == active) return;
        active = u8(on);
        if (on) return;
        for (s32 i = 0; i < count; ++i) entries[i]->Unshow();
    }
};

// ---- HUDHead's head model: the lip-sync face and the model preparation ------------------------------------------
template <class P>
P* OwnProperty(ni::NiAVObject* o) {                   // NiAVObject::GetProperty(type): the object's own list
    for (const auto& p : o->properties)
        if (auto* q = dynamic_cast<P*>(p.Get())) return q;
    return nullptr;
}

// orig: 0x000966A0 (the HUD head's model, cdecl): every geometry below the object (the nodes recursed, a node named
// "QDOutline" (strncmp, 10 characters) skipped with its subtree): the material's ambient = its diffuse and the
// emissive = DAT_0030E940 (zero, O: the f6 snapshot; one revision each), the vertex-colour property's lighting mode 1
// and source mode 0.
void PrepareHeadModel(ni::NiAVObject* o) {
    if (!o) return;
    if (auto* n = dynamic_cast<ni::NiNode*>(o)) {
        if (!n->name.empty() && n->name == "QDOutline") return;
        const size_t count = n->Children().size();   // +0xCC sampled once
        for (size_t i = 0; i < count; ++i)
            PrepareHeadModel(i < n->Children().size() ? n->Children()[i].Get() : nullptr);
    }
    if (dynamic_cast<ni::NiGeometry*>(o)) {
        if (auto* m = OwnProperty<ni::NiMaterialProperty>(o)) {   // GetProperty(3)
            m->ambient = m->diffuse;
            ++m->revision;
            m->emissive = {0.0f, 0.0f, 0.0f};
            ++m->revision;
        }
        if (auto* vc = OwnProperty<ni::NiVertexColorProperty>(o)) {   // GetProperty(9): +0x1C = 1, +0x18 = 0
            vc->lightingMode = 1;
            vc->sourceVertexMode = 0;
        }
    }
}

// orig: HUDHead (0x60 bytes, ctor 0x000E94E0, vtable 0x002A5324): the player's head in a small viewport at the
// top left (the camera +0x50 looking at the head model +0x4C), over the backdrop polygon +0x34 (scene camera) and
// the mask polygons +0x38 / +0x3C / +0x40 of the camera +0x54.
struct HudHead : HudElement {
    f32 x = 0, y = 0, w = 0, h = 0;                   // +0x24 / +0x28 / +0x2C / +0x30
    ni::NiPointer<ni::NiScreenPolygon> backdrop;      // +0x34
    ni::NiPointer<ni::NiScreenPolygon> mask[3];       // +0x38 / +0x3C / +0x40
    std::unique_ptr<LipSyncFace> face;                // +0x44
    const Player* player = nullptr;                   // +0x48
    ni::NiPointer<ni::NiNode> node;                   // +0x4C (the head camera's scene)
    ni::NiPointer<ni::NiCamera> headCamera;           // +0x50
    ni::NiPointer<ni::NiCamera> maskCamera;           // +0x54
    u8 talking = 0, b59 = 0;                          // +0x58 / +0x59
    f32 time = 0;                                     // +0x5C

    HudHead(Hud& hud, const std::string& level) : HudElement(hud, "head") {   // 0x00295A44
        // the head camera's port: (0, t, 1, 1 - t) with t = (107 + 49) / 480 (x87: t stored, 1 - t of the register)
        const double te = (double(kHeadSize) + 49.0f) * kInv480;   // [0x002EED6C] + [0x002A5318], FMUL [0x0029BC14]
        const f32 t = F(te);
        const f32 bottom = F(1.0 - te);               // FLD [0x00293A98] 1.0; FSUB ST(1)
        // the frustum: a = [0x002EED64] (pi / 180, 0x3C8EFA36: O, its static initialiser) 45 0.5; c = F(FCOS a), q =
        // FSIN a / c (kept), (-q, q, 0.75 q, -0.75 q, 0.1, 100) ([0x00293A94] = 0.75)
        const double a = double(kDegToRad) * 45.0f * 0.5f;   // [0x00297410], [0x00293AA0]
        const f32 c = F(std::cos(a));                 // (#35: the oracle's FCOS / FSIN are double)
        const double q = std::sin(a) / double(c);
        const double qy = 0.75f * q;
        const f32 frustum[6] = {F(-q), F(q), F(qy), F(-qy), 0.1f, 100.0f};   // 0x3DCCCCCD, 0x42C80000
        headCamera = new ni::NiCamera;                // new(0x250) 0x001009D0
        headCamera->renderer = hud.level.renderer;    // App->vf14() (+0x1A8)
        headCamera->port[0] = 0.0f, headCamera->port[1] = t, headCamera->port[2] = 1.0f, headCamera->port[3] = bottom;
        headCamera->SetFrustum(frustum);              // 0x00011960
        maskCamera = new ni::NiCamera;                // new(0x250) 0x001009D0, port (0, 1, 1, 0)
        maskCamera->renderer = hud.level.renderer;
        maskCamera->port[0] = 0.0f, maskCamera->port[1] = 1.0f, maskCamera->port[2] = 1.0f, maskCamera->port[3] = 0.0f;
        y = F(kHeadY), x = kHeadX, h = F(kHeadSize), w = kHeadSize;   // [0x002EEDC0] / [0x002EEE2C] / [0x002EED6C]
        static const ni::NiPoint2 kUV[4] = {{0, 0.8359375f}, {0.8359375f, 0.8359375f}, {0.8359375f, 0}, {0, 0}};
        // Level1-6 / Level4-1: HUD\head_bg2.nif (0x002A5384), else HUD\head_bg.nif (0x002A5398)
        const bool alt = crt::StrICmp(level.c_str(), "Level1-6") == 0 || crt::StrICmp(level.c_str(), "Level4-1") == 0;
        backdrop = hud.MakeIcon(alt ? "HUD\\head_bg2.nif" : "HUD\\head_bg.nif", w, h, x, y, true, kUV, nullptr);
        MakeMasks(level);                             // 0x000E8A20
        if (ni::NiMaterialProperty* m = MaterialOf(backdrop.Get())) m->alpha = 1.0f, ++m->revision;
    }

    // orig: 0x000E8A20: the masks on the camera +0x54 (no alpha property from 0x000E5F60; uvs default): head_mask
    // (128 x 64 at (0, y0)), then by the level name's sixth character: '4' head_mask2 (16 x 32 at (128, y0 + 64 -
    // 32)), '1' or "Level5-3" ('5' and the eighth character '3') head_mask3 (128 x 32 at (0, y0 + 64)), with y0 =
    // F(88 0.5 + 49) ([0x002A531C], [0x00293AA0], [0x002EEDC0]); then one alpha property (0xEC & 0xEFFF | 0xE01 =
    // 0xEED: blended, alpha test LEQUAL, reference 0x40) and one z-buffer property ((3 & ~1) | 2: no test, write,
    // function 3) into each mask's property state.
    void MakeMasks(const std::string& level) {
        const double y0e = double(88.0f) * 0.5f + 49.0f;
        const f32 y0 = F(y0e);
        mask[0] = hud.MakeIcon("HUD\\head_mask.nif", 128.0f, 64.0f, 0.0f, y0, false, nullptr, maskCamera.Get());
        const int d5 = level.size() > 5 ? level[5] - '0' : -'0', d7 = level.size() > 7 ? level[7] - '0' : -'0';
        if (d5 == 4) {
            mask[1] = hud.MakeIcon("HUD\\head_mask2.nif", 16.0f, 32.0f, 128.0f, F((double(y0) + 64.0f) - 32.0f), false,
                                   nullptr, maskCamera.Get());
        } else if (d5 == 1 || (d5 == 5 && d7 == 3)) {
            mask[2] = hud.MakeIcon("HUD\\head_mask3.nif", 128.0f, 32.0f, 0.0f, F(double(y0) + 64.0f), false, nullptr,
                                   maskCamera.Get());
        }
        auto* a = new ni::NiAlphaProperty;            // new(0x1C) (0x00013D80: flags 0xEC, ref 0)
        a->alphaTestRef = 0x40;
        a->flags = u16((a->flags & 0xEFFF) | 0x0E01);
        auto* z = new ni::NiZBufferProperty;          // new(0x1C): flags 3, function 3
        z->flags = 3, z->testFunction = 3;
        z->flags = u16((z->flags & 0xFFFE) | 2);
        ni::NiPointer<ni::NiProperty> keepA(a), keepZ(z);
        for (auto& m : mask)
            if (m) m->AttachProperty(a);
        for (auto& m : mask)
            if (m) m->AttachProperty(z);
    }

    // orig: vf01 0x000E98D0 (+0x15 only): a new player (S+0x94) loads its head (0x000E87E0: the definition's
    // HUD_Head file +0x114, then 0x000E8F60, then the definition's file released); with a face, the lip sync
    // (0x000D9340: the player's lip-sync player +0x40 and its current key file +4) and the head's turn 0x000E8E00.
    void Update() override {
        if (!active) return;
        const Player* p = hud.level.player;
        if (player != p) {
            player = p;
            ni::NiPointer<ni::NiUDSFileObject> file = p ? p->cdef.hudHeadFile : nullptr;
            LoadHead(file.Get());
            if (file && p) p->cdef.hudHeadFile = nullptr;   // (+0x114 released)
        }
        if (!face) return;
        // the player's lip-sync player (+0x40): its key file (+0x44, 0x000D9340), set by a lip track and kept until
        // that track's dialogue is deleted
        ni::NiPointer<LipSyncKeyFile> key = p ? p->lip.keyFile : nullptr;
        Turn();                                       // 0x000E8E00
        if (key) {
            // 0x0009D2D0(face, F(+0x4C - dt)): the time the player's own face was evaluated at this step
            EvaluateKeyFile(*key, *face, F(double(p->lip.time) - g_frameStep));   // FSUB [0x002D67E8]
            talking = 1;                              // +0x58
        } else if (talking) {
            talking = 0;
            face->Begin();                            // 0x0009DD50
            face->End();                              // 0x0009DD70
        }
    }

    // orig: 0x000E8E00: the face's mesh turned by the player's move intent (Rz(|+0x4DC|) Ry((+0x4E0 + +0x4D8) 5)
    // in degrees: [0x00293C24] = 5, the x87 sum kept), translate DAT_002F915C (zero), scale |1.0|, Update(0, 1); +0x5C
    // += dt.
    void Turn() {
        ni::NiAVObject* mesh = face->mesh.Get();
        if (mesh && player) {
            const double ay = (double(player->strafe) + player->moveX) * 5.0f;
            f32 az = player->moveY;
            if (az < 0.0f) az = F(-double(az));       // FCOMP [0x00293AB8] 0; TEST AH,5; JP (NaN kept)
            const ni::NiMatrix3 r1 = ni::AxisAngle(F(double(kDegToRad) * ay), 0.0f, 1.0f, 0.0f);   // 0x00103130
            const ni::NiMatrix3 r2 = ni::AxisAngle(F(double(kDegToRad) * az), 0.0f, 0.0f, 1.0f);
            mesh->local.translate = {0.0f, 0.0f, 0.0f};
            mesh->local.rotate = ni::Multiply(r2, r1);   // 0x00013B30 (this = r2)
            mesh->local.scale = 1.0f;                 // FABS of [0x00293CC0] (double 1.0)
            mesh->UpdateDownwardPass(0.0f, true);     // Update(0, 1)
        }
        time = F(double(g_frameStep) + time);
    }

    // orig: 0x000E8F60 (file): the old face and node released; the file loaded (NiStream 0x00109230, Load from the
    // file's bytes 0x001094C0): the first top-level object as a NiUDSFileObject (the face file), the second as a
    // NiNode (the model); 0x000966A0(model); LipSyncFace::Create(face file, model); with a face: the node +0x4C (new
    // NiNode, local rotate Rz(10) Ry(160) in degrees ([0x002A5380] 160, [0x00293C2C] 10), translate zero, scale |1.0|),
    // the face's mesh attached to it (vfA0(mesh, 1)), its outlines (0x0013E5E0), the face's mesh found again by its
    // name (vf54: the outline pass may have replaced it), every effect of the player's node (vf30 +0xD8, in list
    // order) and the player's environment map +0x5A4 affecting the node, a z-buffer property (flags 3, function 3)
    // at the head of its properties, Update(0, 1), UpdateProperties, UpdateEffects, the node as the head camera's
    // scene, the camera placed by the player's type (1 Fry (-7.8, 8.4, 0.8), 2 Leela (-7.8, 8.2, 0.8), 3 Bender
    // (-7.8, 7.3, 0.8), 4 the rodent 0x00034210(-7.8, 7.3, 0.9)) and Update(0, 1).
    void LoadHead(ni::NiUDSFileObject* file) {
        face.reset();                                 // 0x0009DE10, delete
        talking = 0;
        node = nullptr;
        if (!file) return;
        ni::NiStream s;
        if (!s.Load(file->data)) return;
        const std::vector<u32>& top = s.TopLevelLinks();
        auto* faceFile = top.size() > 0 ? dynamic_cast<ni::NiUDSFileObject*>(s.ResolveLink(top[0])) : nullptr;
        ni::NiPointer<ni::NiNode> model(top.size() > 1 ? dynamic_cast<ni::NiNode*>(s.ResolveLink(top[1])) : nullptr);
        ni::NiPointer<ni::NiUDSFileObject> keepFile(faceFile);
        PrepareHeadModel(model.Get());                // 0x000966A0
        face = LipSyncFace::Create(faceFile, model.Get());
        if (!face) return;
        node = new ni::NiNode;                        // new(0xE0) 0x00105C10
        const ni::NiMatrix3 r1 = ni::AxisAngle(F(double(kDegToRad) * 160.0f), 0.0f, 1.0f, 0.0f);
        const ni::NiMatrix3 r2 = ni::AxisAngle(F(double(kDegToRad) * 10.0f), 0.0f, 0.0f, 1.0f);
        node->local.translate = {0.0f, 0.0f, 0.0f};   // DAT_002F915C
        node->local.rotate = ni::Multiply(r2, r1);    // 0x00013B30 (this = r2)
        node->local.scale = 1.0f;
        ni::NiPointer<ni::NiAVObject> mesh = face->mesh;   // 0x000E8800
        const std::string meshName = mesh ? mesh->name : std::string();
        if (mesh) node->AttachChild(mesh.Get(), true);   // vfA0(mesh, 1)
        ni::ConvertToOutlines(mesh.Get());            // 0x0013E5E0
        face->mesh = GetObjectByName(node.Get(), meshName.c_str());   // vf54
        if (player) {
            if (ni::NiNode* pn = player->GetNode())
                for (size_t i = 0; i < pn->effects.size(); ++i)   // (+0xD8 from the head)
                    if (pn->effects[i]) pn->effects[i]->AttachAffectedNode(node.Get());   // 0x001419B0
            if (player->envEffect) player->envEffect->AttachAffectedNode(node.Get());
        }
        auto* z = new ni::NiZBufferProperty;          // new(0x1C): flags 3, function 3 (vtable 0x00296C80)
        z->flags = 3, z->testFunction = 3;
        node->properties.insert(node->properties.begin(), ni::NiPointer<ni::NiProperty>(z));   // AddHead 0x00013A50
        node->UpdateDownwardPass(0.0f, true);         // Update(0, 1)
        node->UpdateProperties();
        node->UpdateEffects();
        switch (player ? player->type : 0) {          // +0x774: the camera's translate (+0x60)
        case 1: headCamera->local.translate = {std::bit_cast<f32>(0xC0F9999Au), std::bit_cast<f32>(0x41066666u),
                                               std::bit_cast<f32>(0x3F4CCCCDu)}; break;
        case 2: headCamera->local.translate = {std::bit_cast<f32>(0xC0F9999Au), std::bit_cast<f32>(0x41033333u),
                                               std::bit_cast<f32>(0x3F4CCCCDu)}; break;
        case 3: headCamera->local.translate = {std::bit_cast<f32>(0xC0F9999Au), std::bit_cast<f32>(0x40E9999Au),
                                               std::bit_cast<f32>(0x3F4CCCCDu)}; break;
        case 4: headCamera->local.translate = {std::bit_cast<f32>(0xC0F9999Au), std::bit_cast<f32>(0x40E9999Au),
                                               std::bit_cast<f32>(0x3F666666u)}; break;   // 0x00034210
        default: break;
        }
        headCamera->UpdateDownwardPass(0.0f, true);   // Update(0, 1)
    }

    void SetActive(bool on) override { SetActiveWithPolygon(*this, on, backdrop.Get()); }   // orig: vf05 0x000E8DA0
    // orig: vf02 0x000E89B0 (+0x15 and a face): the head camera's ClearBuffer(4) 0x000FEF20 (its port, the depth),
    // the mask camera's Click, the head camera's Click (0x00100CE0); the profiler marks 0x000AEDC0 / 0x000B0420 are
    // empty.
    void Draw() override {
        if (!active || !face) return;
        if (headCamera->renderer) headCamera->renderer->ClearBuffer(headCamera->port, 4);
        ni::VisitForCulling(*maskCamera, nullptr);    // (+0x15C: no scene)
        ni::VisitForCulling(*headCamera, node.Get());
    }
};

// orig: HUDHealth (0x4C bytes, ctor 0x000E9AB0, vtable 0x002A53D0)
struct HudHealth : HudElement {
    f32 x = 0, y = 0, w = 64.0f, h = 64.0f;           // +0x24 / +0x28 / +0x2C / +0x30
    ni::NiPointer<ni::NiScreenPolygon> bar;           // +0x34
    ni::NiMaterialProperty* material = nullptr;       // +0x38
    f32 flashUp = 0, flashDown = 0, health = 1.0f;    // +0x3C / +0x40 / +0x44
    u8 dirty = 1;                                     // +0x48
    explicit HudHealth(Hud& hud) : HudElement(hud, "health") {   // 0x00297208
        y = F(double(49.0f) + 39.0f);                 // [0x002A53B0] + [0x002A53CC]
        x = F(double(36.0f) + 41.0f);                 // [0x002A53AC] + [0x0029A694]
        bar = hud.MakeIcon("HUD\\health_bar2.nif", w, h, x, y, true, nullptr, nullptr);
        material = MaterialOf(bar.Get());
        if (bar) {
            auto* vc = new ni::NiVertexColorProperty; // (vtable 0x00293AD0: +0x18 = +0x1C = 0)
            vc->sourceVertexMode = 0, vc->lightingMode = 0;
            bar->AttachProperty(vc);
        }
        if (material) material->alpha = 1.0f, ++material->revision;
    }
    void SetActive(bool on) override { SetActiveWithPolygon(*this, on, bar.Get()); }   // orig: vf05 0x000E8DA0
    void Command(const char* cmd, f32 v) override {   // orig: vf04 0x000E9A20
        if (std::strncmp(cmd, "set", 4) != 0) return; // 0x0029A6A0
        if (v == health) return;                      // TEST AH,0x44; JNP
        if (v < health) flashDown = 0.5f;             // TEST AH,5; JP: +0x40 when lower, else +0x3C
        else flashUp = 0.5f;
        dirty = 1;
        health = v;
        if (!(v > 0.0f)) health = 0.0f;
        else if (!(v <= 1.0f)) health = 1.0f;         // TEST AH,0x41; JNE
    }
    void Update() override {                          // orig: vf01 0x000E9CE0 (+0x15 only)
        if (!active) return;
        if (flashUp > 0.0f) flashUp = F(double(flashUp) - g_frameStep);
        if (!(flashUp > 0.0f)) flashUp = 0;
        if (flashDown > 0.0f) flashDown = F(double(flashDown) - g_frameStep);
        if (!(flashDown > 0.0f)) flashDown = 0;
        const double r3 = double(health) * 3.0f;      // [0x0029563C]
        const f32 A = !(r3 <= 1.0) ? 1.0f : F(r3);    // TEST AH,0x41; JNE
        double G = double(1.2f) - double(health) * 0.5f;   // [0x0029A520] - h 0.5
        if (!(G <= 1.0)) G = 1.0;
        const f32 q = F(double(flashDown) * flashDown * 4.0f);
        double gb = G + q;
        if (!(gb <= 1.0)) gb = 1.0;
        else if (!(gb > 0.0)) gb = 0.0;
        double rr = double(A) - q;
        if (!(rr <= 1.0)) rr = 1.0;
        else if (!(rr > 0.0)) rr = 0.0;
        if (material) {                               // +0x40 / +0x44 / +0x48: (gb, rr, 0)
            material->emissive = {F(gb), F(rr), 0.0f};
            ++material->revision;
        }
        if (!dirty || !bar) return;
        // the dial: vertex 1 at angle 0, vertex 2 at pi / 2 - pi h / 2 (FSIN of the register, FCOS of the float)
        const double a = double(kPi) * 0.5f - double(kPi) * health * 0.5f;
        const f32 af = F(a);
        const double s = std::sin(a), c = std::cos(double(af));   // (#35: the oracle's FSIN / FCOS are double)
        auto& v = bar->vertices;
        v[1].x = F(c * w + x);
        v[2].x = v[1].x;
        v[1].y = F(double(y) + h);
        v[2].y = F(s * h + y);
        v[1].x = F(double(v[1].x) * kInv640);
        v[2].x = F(double(v[2].x) * kInv640);
        v[1].y = F(1.0 - double(v[1].y) * kInv480);
        v[2].y = F(1.0 - double(v[2].y) * kInv480);
        if (bar->uvs.size() >= 3) {
            bar->uvs[1].u = F(c);
            bar->uvs[2].u = F(c);
            bar->uvs[2].v = F(s);
        }
        bar->flags |= 5;
        dirty = 0;
    }
};

// orig: a HUD element's screen object (0x1C bytes, ctor 0x000E5680(name, polygon, owner), vtable 0x002A5114, dtor
// 0x000E5720): a named polygon in its owner's list (+0x18; +0x10 next / +0x14 previous), shown (+0xC) at first.
struct HudScreenObject {
    std::string name;                                 // +0x04 (0x000E5410: a heap copy)
    ni::NiPointer<ni::NiScreenPolygon> poly;          // +0x08
    u8 shown = 1;                                     // +0x0C
    Hud& hud;
    HudScreenObject(Hud& h, const char* n, ni::NiPointer<ni::NiScreenPolygon> p) : name(n), poly(std::move(p)), hud(h) {}
    // orig: 0x000E5720: the name freed, the polygon off the scene camera (S+0x8C +0x30, 0x00100FC0) and released, the
    // object out of its owner's list.
    ~HudScreenObject() {
        if (poly)
            if (ni::NiCamera* c = hud.SceneCamera()) c->DetachScreenPolygon(poly.Get());
    }
    // orig: 0x000E5AB0 (on): a change of +0xC sets the polygon's material alpha 1 / 0 (one revision).
    void Show(bool on) {
        if (shown == u8(on)) return;
        shown = u8(on);
        SetPolygonAlpha(poly.Get(), on ? 1.0f : 0.0f);
    }
};

// HUDWeapons' ammo text position (static initialisers 0x0021DBF0 / 0x0021DC20: (640 - (88 0.5 + 36)) / 640 and
// (480 - (88 + 49)) / 480 with the inexact reciprocals; .rdata constants only)
const f32 kAmmoTextX = std::bit_cast<f32>(0x3F600000u);   // 0x002EFB3C
const f32 kAmmoTextY = std::bit_cast<f32>(0x3F36EEF0u);   // 0x002EFB0C

// orig: HUDWeapons (0x74 bytes, ctor 0x000ED220, vtable 0x002A5708: vf00 0x000ECFF0, vf01 0x000ED010, vf02 0x000ECE50,
// vf03 0x000ED410, vf04 0x000ECF50, vf05 0x000ED0F0; dtor 0x000ECE70): Fry's weapon box at the top right: the backdrop,
// the selected weapon's icon (fading over the previous one), its ammo count and the pickup flash.
struct HudWeapons : HudElement {
    f32 rx = 0, ry = 0, rw = 223.0f, rh = 128.0f;    // +0x24 / +0x28 / +0x2C / +0x30
    f32 x = kWeaponsX, y = F(kWeaponsY), w = 88.0f, h = F(88.0f);   // +0x34 / +0x38 / +0x3C / +0x40
    std::unique_ptr<text::TextObject> text;           // +0x44
    ni::NiPointer<ni::NiScreenPolygon> backdrop;      // +0x48
    // (+0x4C: an array of three NiPointers, 0x00166E90 / 0x00166E30 with 0x00013C70 / 0x0010DE50; not used)
    std::unique_ptr<HudScreenObject> selected;        // +0x58
    std::unique_ptr<HudScreenObject> previous;        // +0x5C (fading out)
    f32 fade = 1.0f;                                  // +0x60
    s32 ammo = 0;                                     // +0x64
    u8 coolEffect = 0;                                // +0x68
    ni::NiPointer<ni::NiCamera> camera;               // +0x6C (the scene camera at construction)
    std::unique_ptr<HudFlash> flash;                  // +0x70 (not deleted by the original's dtor)

    explicit HudWeapons(Hud& hud) : HudElement(hud, "weapons") {   // 0x0029A45C
        rx = F(640.0 - double(rw));                   // FLD [0x002A527C]; FSUB +0x2C; FSTP +0x24
        // TextObject::Create(0), App->vf14() (0x00098F80), +0x60 = 0, +0x68 = 0, SetScale(1.5, 1.7)
        text.reset(text::TextObject::Create(0, hud.readFile, hud.level.renderer));
        if (text) {
            text->style.hAlign = 0;
            text->style.wordWrap = false;
            text->SetScale(1.5f, std::bit_cast<f32>(0x3FD9999Au));
        }
        const f32 u = F(double(88.0f) * 0.0078125f);  // 88 / 128 ([0x002A5738]) for the uvs (0, u) (u, u) (u, 0) (0, 0)
        const ni::NiPoint2 uv[4] = {{0, u}, {u, u}, {u, 0}, {0, 0}};
        backdrop = hud.MakeIcon("HUD\\w_backdr.nif", w, h, x, y, true, uv, nullptr);   // 0x002A5724
        camera = hud.SceneCamera();                   // S+0x8C ? (S+0x8C)+0x30
        Command("ammo", -1.0f);                       // vf04 0x000ECF50 (0x00297214)
        active = 1;                                   // +0x15 = 1, then vf05(0)
        SetActive(false);
    }
    ~HudWeapons() override {                          // orig: 0x000ECE70
        selected.reset();
        previous.reset();
        if (text) text->RemoveAll();                  // 0x00099080
        (void)flash.release();                        // (the original never deletes +0x70)
    }
    // orig: vf01 0x000ED010 (+0x15): the flash (0x000EBFC0); with a previous icon the fade F((dt + dt) + +0x60):
    // below 1 its alpha 1 - fade, else fade 1 and the previous icon deleted; the selected icon's alpha = the fade (1
    // without a previous one).
    void Update() override {
        if (!active) return;
        if (flash) (void)flash->Update();
        if (previous) {
            const f32 v = F((double(g_frameStep) + g_frameStep) + fade);   // FADD ST(0), ST(0); FADD +0x60; FSTP
            fade = v;
            if (v < 1.0f) {                           // FCOMP [0x00293A98]; TEST AH,5; JP
                SetPolygonAlpha(previous->poly.Get(), F(1.0 - double(v)));
            } else {
                fade = 1.0f;
                previous.reset();                     // vf00(1)
            }
            if (selected) SetPolygonAlpha(selected->poly.Get(), fade);
            return;
        }
        if (selected) SetPolygonAlpha(selected->poly.Get(), 1.0f);
    }
    // orig: vf02 0x000ECE50 (+0x15 only): the text's camera update and Click.
    void Draw() override {
        if (!active || !text) return;
        text->UpdateCamera();                         // 0x00098D70
        text->Click();                                // 0x00098DB0
    }
    // orig: vf03 0x000ED410 (cmd, name; strncmp): "run_cool_effect" (16, 0x002A3D8C): +0x68 = 1. "select" (7,
    // 0x0029A464): the definition of the name (Session::FindDef 0x000C3F10; none: the icon deleted); a pickup family
    // one (vf0C(4)): Hammer / Shotgun / Railgun / Machinegun 128 wide, Pistol 64 wide and 32 to the right, any other
    // hides the element (+0x15 = 0) and deletes the icon; with +0x68 the flash (made once: 0x000EBF40 at the box's
    // (+0x24 + 78, +0x28 + 29) ([0x0009ED50]), 128 x 128, 1 s, split 0.75, spin pi 4), restarted and +0x68 = 0; then
    // with the definition's icon texture (+0x28): the old icon becomes the previous one (fade 0; its own previous
    // deleted) or, without one, the backdrop opaque; the new icon (0x000E5C00: w x 64 at (+0x24 + 72 (+ 32), +0x28 +
    // 93 - 32), alpha, the scene camera) as "selected" (0x002A573C); the previous icon, the new one and the flash put
    // back on the camera +0x6C in that order; a hidden element makes the new icon transparent.
    void Command(const char* cmd, const char* n) override {
        if (std::strncmp(cmd, "select", 7) != 0) {
            if (std::strncmp(cmd, "run_cool_effect", 16) == 0) coolEffect = 1;
            return;
        }
        const f32 bx = F(double(rx) + 72.0f);         // [0x002A574C] -> esp+0x10 (and esp+0x18)
        const f32 by = F(double(ry) + 93.0f);         // [0x002A5748] -> esp+0x14
        f32 px = bx, wide = 0;                        // esp+0x18 / esp+0x10
        const Def* d = hud.level.defs ? hud.level.defs->Find(n) : nullptr;
        if (!d) {
            selected.reset();                         // vf00(1); +0x58 = 0
            return;
        }
        if (!d->IsType(4)) return;
        if (crt::StrICmp(n, "Hammer") == 0) {         // 0x0029D7A4
            wide = 128.0f;
        } else if (crt::StrICmp(n, "Pistol") == 0) {  // 0x0029CC8C
            px = F(double(bx) + 32.0f);               // [0x002A5368]
            wide = 64.0f;
        } else if (crt::StrICmp(n, "Shotgun") == 0 || crt::StrICmp(n, "Railgun") == 0 ||
                   crt::StrICmp(n, "Machinegun") == 0) {   // 0x0029CC84 / 0x0029CC7C / 0x0029CC70
            wide = 128.0f;
        } else {
            active = 0;                               // +0x15 = 0 (not through vf05)
            selected.reset();
            return;
        }
        const f32 py = F(double(by) - 32.0f);         // FSUB [0x002A5368] -> esp+0x1C
        if (coolEffect) {
            if (!flash) {
                const f32 pos[2] = {F(double(rx) + 78.0f), F(double(ry) + 29.0f)};   // 0x0009ED50
                const f32 size[2] = {128.0f, 128.0f};
                flash = std::make_unique<HudFlash>(hud, pos, size, 1.0f, 0.75f, std::bit_cast<f32>(0x41490FDBu));
            }
            coolEffect = 0;
            flash->time = 0;                          // (+0x70)+0 = 0
        }
        const auto* pd = dynamic_cast<const PickupDef*>(d);
        ni::NiObject* tex = pd ? pd->icon.Get() : nullptr;   // D+0x28
        if (!tex) return;
        if (selected) {
            previous = std::move(selected);           // the old previous deleted (vf00(1))
            fade = 0.0f;
        } else {
            SetPolygonAlpha(backdrop.Get(), 1.0f);
        }
        selected = std::make_unique<HudScreenObject>(   // new(0x1C) 0x000E5680("selected", polygon, this)
            hud, "selected", hud.MakePolygon(tex, wide, 64.0f, px, py, {1, 1, 1}, true, nullptr, nullptr));
        if (camera) {
            if (previous) camera->DetachScreenPolygon(previous->poly.Get());   // 0x00100FC0
            camera->DetachScreenPolygon(selected->poly.Get());
            if (flash) flash->Detach(camera.Get());   // 0x000EB950
            if (previous) camera->AttachScreenPolygon(previous->poly.Get());   // 0x00019460
            camera->AttachScreenPolygon(selected->poly.Get());
            if (flash) flash->Attach(camera.Get());   // 0x000EC130
        }
        if (!active) SetPolygonAlpha(selected->poly.Get(), 0.0f);
    }
    // orig: vf04 0x000ECF50 ("ammo" (strncmp 5, 0x00297214), v): +0x64 = _ftol(v), the text emptied; for >= 0 "%d"
    // (0x00296D4C) widened (0x000D2CB0, 0x1F) at the static position: AddString(.., -1).
    void Command(const char* cmd, f32 v) override {
        if (std::strncmp(cmd, "ammo", 5) != 0) return;
        ammo = s32(s64(v));                           // _ftol 0x00167110
        if (text) text->RemoveAll();
        if (ammo < 0) return;
        char buf[0x20] = {};
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(ammo));
        ShowText(buf);
    }
    // orig: vf05 0x000ED0F0: a change of +0x15. Showing: the fade 1; with an icon the backdrop opaque and the icon
    // shown (0x000E5AB0(1)); the ammo text added (" " (0x002A55B0) for +0x64 < 0, else "%d"; the text is not emptied
    // first). Hiding: the text emptied, the icon hidden, the fade 1, the previous icon deleted, the backdrop
    // transparent.
    void SetActive(bool on) override {
        if (u8(on) == active) return;
        active = u8(on);
        if (on) {
            fade = 1.0f;
            if (selected) {
                SetPolygonAlpha(backdrop.Get(), 1.0f);
                selected->Show(true);
            }
            char buf[0x20] = {};
            if (ammo >= 0) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(ammo));
            else std::snprintf(buf, sizeof buf, " ");
            ShowText(buf);
            return;
        }
        if (text) text->RemoveAll();
        if (selected) selected->Show(false);
        fade = 1.0f;
        previous.reset();
        SetPolygonAlpha(backdrop.Get(), 0.0f);        // +0x48
    }
    // 0x000D2CB0 (the 8-bit string widened (MOVSX), at most 0x1F characters), at (0x002EFB3C, 0x002EFB0C):
    // AddString(.., -1)
    void ShowText(const char* s) {
        if (!text) return;
        std::u16string wtext;
        for (const char* c = s; *c && wtext.size() < 0x1F; ++c)
            wtext.push_back(char16_t(u16(s16(static_cast<signed char>(*c)))));
        text->x = kAmmoTextX, text->y = kAmmoTextY;   // +0x4C / +0x50
        (void)text->AddString(wtext, -1.0f);          // 0x0009A070
    }
};

// orig: HUDCharge (0x7C bytes, ctor 0x000E6750, vtable 0x002A5158: vf01 0x000E60D0, vf02 0x000B0420 (empty), vf03
// 0x00011280, vf04 0x000E6220, vf05 0x000E6280; dtor 0x000E6050): four 16 x 16 pips on the scene camera, the first
// `level` of them with chrg_on's texture, the others chrg_off's.
struct HudCharge : HudElement {
    f32 pos[8] = {87.0f, 155.0f, 114.0f, 144.0f, 135.0f, 123.0f, 143.0f, 96.0f};   // +0x24..+0x40: the pips' (x, y)
    f32 w = 16.0f, h = 16.0f;                         // +0x44 / +0x48 (0x000E6330 uses its own constants)
    ni::NiPointer<ni::NiScreenPolygon> pip[4];        // +0x4C
    ni::NiMaterialProperty* material[4] = {};         // +0x5C
    ni::NiPointer<ni::NiTexturingProperty> off, on;   // +0x6C / +0x70
    s32 level = 0;                                    // +0x74
    u8 dirty = 0;                                     // +0x78
    explicit HudCharge(Hud& hud) : HudElement(hud, "charge") {   // 0x0029A698
        active = 1;                                   // +0x15 = 1
        Setup();                                      // 0x000E6330
    }
    static ni::NiTexturingProperty* MakeTexturing(ni::NiObject* texture) {
        auto* t = new ni::NiTexturingProperty;        // new(0x3C) 0x00109DC0; +0x18 = 2
        t->applyMode = 2;
        if (!t->HasMap(0)) t->SetBaseTexture(nullptr);   // SetBaseFilterMode(1) 0x0001D6B0 (makes the base map)
        t->maps[0].filterMode = 1;
        t->SetBaseTexture(texture);                   // 0x0001D610
        return t;
    }
    // orig: 0x000E6330: with the scene camera (S+0x8C +0x30): the textures chrg_on (0x002A5188), then chrg_off
    // (0x002A5174); +0x6C a texturing property of chrg_off, +0x70 of chrg_on (apply mode 2, filter 1); per pip a
    // polygon (x / 640, 1 - y / 480; the size [0x00293AC0] 0.025 x [0x00295A90] 16 / 480; the default uvs), +0x6C in
    // its slot, a material (all four colours 1, shininess 0, alpha 1: six revisions), an alpha property ((0xEC &
    // 0xFEED) | 0xED), attached to the camera.
    void Setup() {
        ni::NiCamera* camera = hud.SceneCamera();
        if (!camera) return;
        ni::NiObject* texOn = hud.Texture("HUD\\chrg_on.nif");
        ni::NiObject* texOff = hud.Texture("HUD\\chrg_off.nif");
        off = MakeTexturing(texOff);
        on = MakeTexturing(texOn);
        static const ni::NiPoint2 kUV[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const f32 W = std::bit_cast<f32>(0x3CCCCCCDu), H = std::bit_cast<f32>(0x3D088889u);
        for (int i = 0; i < 4; ++i) {
            const double L = double(pos[2 * i]) * kInv640;   // FMUL [0x0029BC18] (kept)
            const double T = 1.0 - double(pos[2 * i + 1]) * kInv480;   // FMUL [0x0029BC14]; FSUBR [0x00293A98]
            const double B = T - H;                   // FLD [0x00295A90]; FSUBR ST(1) (kept)
            const f32 R = F(L + W);                   // FADD [0x00293AC0]; FST
            const ni::NiPoint3 v[4] = {{F(L), F(B), 0}, {R, F(B), 0}, {R, F(T), 0}, {F(L), F(T), 0}};
            pip[i] = new ni::NiScreenPolygon(4, v, kUV, nullptr);   // new(0x24) 0x00106DE0
            pip[i]->AttachProperty(off.Get());
            auto* m = new ni::NiMaterialProperty;     // new(0x5C) 0x00114B10
            material[i] = m;
            m->ambient = m->diffuse = m->specular = m->emissive = {1.0f, 1.0f, 1.0f};
            m->shininess = 0.0f, m->alpha = 1.0f;
            m->revision += 6;
            pip[i]->AttachProperty(m);
            auto* a = new ni::NiAlphaProperty;        // new(0x1C): flags 0xEC
            a->flags = u16((a->flags & 0xFEED) | 0xED);
            pip[i]->AttachProperty(a);
            camera->AttachScreenPolygon(pip[i].Get());   // 0x00019460
        }
    }
    // orig: vf01 0x000E60D0 (no activity test): when +0x78 is set, by +0x74 (jump table 0x000E6200; above 4 nothing)
    // the first `level` pips get +0x70 (on), the others +0x6C (off) in their slots (0x00013CC0); +0x78 = 0.
    void Update() override {
        if (!dirty) return;
        if (u32(level) <= 4)
            for (int i = 0; i < 4; ++i)
                if (pip[i]) pip[i]->AttachProperty(i < level ? on.Get() : off.Get());
        dirty = 0;
    }
    // orig: vf04 0x000E6220 ("charge" (_stricmp, 0x0029A698), v): _ftol(v) in 0..4 (unsigned) and new: +0x74, +0x78 = 1.
    void Command(const char* cmd, f32 v) override {
        if (!cmd || crt::StrICmp(cmd, "charge") != 0) return;
        const s32 n = s32(s64(v));                    // _ftol 0x00167110 (truncation; the low dword)
        if (u32(n) > 4 || n == level) return;
        level = n;
        dirty = 1;
    }
    // orig: vf05 0x000E6280: showing re-attaches the pips to the scene camera (0x00019460), hiding detaches them
    // (0x00100FC0); nothing when +0x15 already has the value.
    void SetActive(bool show) override {
        if (show == bool(active)) return;
        active = u8(show);
        ni::NiCamera* camera = hud.SceneCamera();
        if (!camera) return;
        for (auto& p : pip) {
            if (show) camera->AttachScreenPolygon(p.Get());
            else camera->DetachScreenPolygon(p.Get());
        }
    }
};

// orig: HUDItems (0x44 bytes, ctor 0x000EA800, vtable 0x002A5508)
struct HudItems : HudElement {
    std::unique_ptr<HudFlash> flash;                  // +0x24
    f32 x = kItemsX, y = F(208.0f), w = 128.0f, h = F(128.0f);   // +0x28 / +0x2C / +0x30 / +0x34
    ni::NiPointer<ni::NiScreenPolygon> icon;          // +0x38
    f32 time = 0;                                     // +0x3C
    ni::NiPointer<ni::NiCamera> camera;               // +0x40 (the scene camera at construction)
    explicit HudItems(Hud& hud) : HudElement(hud, "items") {   // 0x0029A418
        camera = hud.SceneCamera();
    }
    // orig: 0x000EA790 (name): the item table DAT_002D7030 (strcmp; null-terminated: LightArtefactDef, Book, Drill,
    // Cup, Bucket, Cog, Calculator, Hairdryer, FireExtinguisher, Pogostick, PairOfPliers, propeller1, KeyCard,
    // KeyCard_mine and the thirteen keycard_<colour>, Jack).
    static bool IsItem(const char* n) {
        static const char* const kItems[] = {
            "LightArtefactDef", "Book",          "Drill",         "Cup",            "Bucket",        "Cog",
            "Calculator",       "Hairdryer",     "FireExtinguisher", "Pogostick",   "PairOfPliers",  "propeller1",
            "KeyCard",          "KeyCard_mine",  "keycard_black", "keycard_blue",   "keycard_brown", "keycard_cyan",
            "keycard_green",    "keycard_orance", "keycard_pink1", "keycard_pink2", "keycard_red",   "keycard_white",
            "keycard_yellow",   "Jack"};
        for (const char* k : kItems)
            if (std::strcmp(k, n) == 0) return true;
        return false;
    }
    // orig: vf03 0x000EAC10 ("show" (5 bytes, 0x0029A420), name): an item of the table whose pickup family
    // definition (Session::FindDef 0x000C3F10, vf0C(4)) has an icon texture (+0x28): the old icon off the camera +0x40
    // and released; the flash (made once: 0x000EBF40 at the icon's place and size, 1 s, split 0.75, spin
    // DAT_002EF294) restarted; the icon (0x000EA9B0: 0x000E5C00 w x h at (x, y), alpha, the scene camera) opaque; the
    // flash off the camera, the icon off and back on, the flash back on; the time 0.
    void Command(const char* cmd, const char* n) override {
        if (std::memcmp(cmd, "show", 5) != 0) return;
        const Def* d = hud.level.defs ? hud.level.defs->Find(n) : nullptr;
        if (!d || !d->IsType(4) || !IsItem(n)) return;
        const auto* pd = dynamic_cast<const PickupDef*>(d);
        ni::NiObject* tex = pd ? pd->icon.Get() : nullptr;   // D+0x28
        if (!tex) return;
        if (icon) {
            if (camera) camera->DetachScreenPolygon(icon.Get());   // 0x00100FC0
            icon = nullptr;
        }
        if (!flash) {
            const f32 pos[2] = {x, y}, size[2] = {w, h};
            flash = std::make_unique<HudFlash>(hud, pos, size, 1.0f, 0.75f, kItemsFlashSpin);
        }
        flash->time = 0;                              // (+0x24)+0 = 0
        icon = hud.MakePolygon(tex, w, h, x, y, {1, 1, 1}, true, nullptr, nullptr);   // 0x000EA9B0
        SetPolygonAlpha(icon.Get(), 1.0f);
        flash->Detach(camera.Get());                  // 0x000EB950
        if (camera && icon) {
            camera->DetachScreenPolygon(icon.Get());
            camera->AttachScreenPolygon(icon.Get());  // 0x00019460
        }
        flash->Attach(camera.Get());                  // 0x000EC130
        time = 0;                                     // +0x3C
    }
    // orig: vf01 0x000EAA50 (+0x15, an icon): r = F(time / 3) ([0x002A54E8]); the flash (0x000EBFC0); the icon's
    // alpha 0 below 0.25, rising to 1 at 0.5, 1 to 0.875, falling to 0 at 1 ([0x002A54EC..F8]), then the icon off the
    // camera and released; time += dt.
    void Update() override {
        if (!active || !icon) return;
        const f32 r = F(double(time) / 3.0f);
        if (flash) (void)flash->Update();             // (the original has no null test: a shown icon made it)
        const double q = 0.25f, hf = 0.5f, s7 = 0.875f, one = 1.0f;
        if (r < q) {                                  // FCOMP; TEST AH,5; JP (a NaN falls through to the end)
            SetPolygonAlpha(icon.Get(), 0.0f);
        } else if (r < hf) {
            SetPolygonAlpha(icon.Get(), F((double(r) - q) / (hf - q)));   // FSUB; FLD; FSUB; FDIVP ST(1)
        } else if (r < s7) {
            SetPolygonAlpha(icon.Get(), 1.0f);
        } else if (r < one) {
            SetPolygonAlpha(icon.Get(), F(1.0 - (double(r) - s7) / (one - s7)));   // FSUBR [0x00293A98]
        } else {
            if (camera) camera->DetachScreenPolygon(icon.Get());   // 0x00100FC0
            icon = nullptr;
        }
        time = F(double(g_frameStep) + time);         // FLD dt; FADD +0x3C; FSTP
    }
};

// orig: HUDNibblerCnt (0x48, ctor 0x000EB3C0, vtable 0x002A557C) and HUDCollectableCnt (0x48, ctor 0x000E8300, vtable
// 0x002A52E4): a counter's icon (a level texture / the player's collectable's icon), pulsing when the count changes.
struct HudCounter : HudElement {
    s32 shown = -1;                                   // +0x28
    ni::NiPointer<ni::NiScreenPolygon> icon;          // +0x2C
    f32 x, y, w = 64.0f, h = F(64.0f);                // +0x30 / +0x34 / +0x38 / +0x3C
    u8 pulsing = 0;                                   // +0x40
    f32 half = 0;                                     // +0x44
    const bool nibbler;
    std::unique_ptr<text::TextObject> text;           // +0x24
    HudCounter(Hud& hud, bool nib)
        : HudElement(hud, nib ? "Nibbler_cnt" : "collectable_cnt"),   // 0x002A5598 / 0x002A5300
          x(nib ? kNibblerX : kCollectX), y(F(nib ? kNibblerY : kCollectY)), nibbler(nib), text(MakeCounterText(hud)) {}
    void Draw() override { DrawText(*this, text.get()); }   // orig: vf02 0x000EB3A0 (both)
    // orig: vf05 0x000EB830 / 0x000E8760: hiding empties the text and makes the icon transparent, showing rewrites
    // the text (0x000EB520 / 0x000E8460) and makes it opaque.
    void SetActive(bool on) override {
        if (active && !on) {
            active = 0;
            if (text) text->RemoveAll();
            SetPolygonAlpha(icon.Get(), 0.0f);
        } else if (!active && on) {
            active = 1;
            UpdateText();
            SetPolygonAlpha(icon.Get(), 1.0f);
        }
    }
    void UpdateText() {                               // orig: 0x000EB520 / 0x000E8460
        char buf[0x40] = {};
        if (nibbler) {
            const s32 total = hud.level.nibblerCount; // S+0x190
            std::snprintf(buf, sizeof buf, "% 3d/%d%s", static_cast<int>(shown), static_cast<int>(total),
                          total < 10 ? "  " : total < 100 ? " " : "");   // 0x002A55A4 / 0x002A55B4 / 0x002A55B0
            SetCounterText(text.get(), buf, kNibblerTextX, kNibblerTextY);
        } else {
            std::snprintf(buf, sizeof buf, "%d", static_cast<int>(shown));   // 0x00296D4C
            SetCounterText(text.get(), buf, kCollectTextX, kCollectTextY);
        }
    }
    void MakeIcon() {                                 // orig: 0x000EB5D0 / 0x000E84E0
        ni::NiObject* tex = nullptr;
        if (nibbler) {
            tex = hud.level.FindTexture("nibbler.tga");   // 0x002A55B8
        } else {
            const Player* p = hud.level.player;       // S+0x94 +0x774
            if (!p) return;
            const char* n = p->type == 1 ? "FryCash" : p->type == 3 ? "BenderRuby" : p->type == 2 ? "LeelaGoldbar"
                          : p->type == 4 ? "Fish_skeleton_pickup" : nullptr;
            const Def* d = n && hud.level.defs ? hud.level.defs->Find(n) : nullptr;   // Session::FindDef 0x000C3F10
            if (const auto* pd = dynamic_cast<const PickupDef*>(d)) tex = pd->icon.Get();   // +0x28
            if (!tex) return;
        }
        if (!tex) return;
        icon = hud.MakePolygon(tex, w, h, x, y, {1, 1, 1}, true, nullptr, nullptr);
    }
    void Update() override {                          // orig: vf01 0x000EB680 / 0x000E85B0
        if (!IsActive()) return;
        const s32 n = nibbler ? hud.level.nibblersPickedUp : hud.level.pickupValue;   // S+0x18C / S+0x188
        if (n != shown) {
            shown = n;
            UpdateText();
            half = F(double(w) * 0.5f * 1.5f);        // FMUL [0x00293AA0]; FMUL [0x00293BEC]
            pulsing = 1;
        }
        if (!icon) MakeIcon();
        if (icon && pulsing && shown > 0) Pulse(*icon, w, half, pulsing);
    }
};

// ---- HUDChickenWalker (level1-6) ---------------------------------------------------------------------------------
// The layout globals of the walker's gauges (static initialisers 0x002159D0..0x00215A50; computed from their
// constants: FPATAN of the doubles 0x002A52B8 / 0x002A52C0 over 1, [0x002A5210] 230 [0x002A52C8] / [0x002A52CC],
// [0x002A5214] 269 - [0x002A52D0] 117; L: not read from a snapshot).
const f32 kCwUpper = std::bit_cast<f32>(0x43039790u);   // 0x002EE9C4 = 230 * 0.5721393 (the gauge above y 269)
const f32 kCwLower = std::bit_cast<f32>(0x42A4E7D9u);   // 0x002EEA24 = 230 * 0.3584906 (below)
constexpr f32 kCwTop = 152.0f;                          // 0x002EE968 = 269 - 117 (the backdrops' y)
const f32 kCwAngleUp = std::bit_cast<f32>(0x3F0509DDu); // 0x002EE9FC = atan(0.5721393)
const f32 kCwAngleDown = std::bit_cast<f32>(0x3EB03D6Eu);   // 0x002EE990 = atan(0.3584906)
constexpr f32 kCwIndicatorTime = 1.0f;                  // DAT_002D7028 (data, never written)

// orig: 0x000E73D0 (vertices, file, uvs; 3 vertices) / 0x000E7680 (4 vertices): a screen polygon on the scene camera
// (S+0x8C +0x30, no null test) with a material (all four colours `rgb`: six revisions, shininess 0, alpha 1), a vertex
// colour property (source 0, lighting 0), an alpha property ((0xEC & 0xFEED) | 0xED) and a texturing property (apply
// mode 2, the base map's filter 1 and clamp 0 (0x000506A0), the HUD's texture of the file), in that order.
ni::NiPointer<ni::NiScreenPolygon> MakeWalkerPolygon(Hud& hud, u16 n, const ni::NiPoint3* v, const char* file,
                                                     const ni::NiPoint2* uv, const ni::NiColor& rgb) {
    ni::NiCamera* camera = hud.SceneCamera();
    ni::NiPointer<ni::NiScreenPolygon> p(new ni::NiScreenPolygon(n, v, uv, nullptr));   // new(0x24) 0x00106DE0
    auto* m = new ni::NiMaterialProperty;             // new(0x5C) 0x00114B10
    m->ambient = m->diffuse = m->specular = m->emissive = rgb;
    m->revision += 6;
    m->shininess = 0.0f, m->alpha = 1.0f;
    p->AttachProperty(m);
    auto* vc = new ni::NiVertexColorProperty;         // new(0x20) (vtable 0x00293AD0): +0x18 = +0x1C = 0
    vc->sourceVertexMode = 0, vc->lightingMode = 0;
    p->AttachProperty(vc);
    auto* a = new ni::NiAlphaProperty;                // new(0x1C): flags 0xEC
    a->flags = u16((a->flags & 0xFEED) | 0xED);
    p->AttachProperty(a);
    ni::NiObject* tex = hud.Texture(file);            // 0x000E59E0
    auto* t = new ni::NiTexturingProperty;            // new(0x3C) 0x00109DC0
    t->applyMode = 2;                                 // +0x18
    if (!t->HasMap(0)) t->SetBaseTexture(nullptr);    // SetBaseFilterMode(1) 0x0001D6B0 (makes the base map)
    t->maps[0].filterMode = 1;
    t->maps[0].clampMode = 0;                         // 0x000506A0(0)
    if (tex) t->SetBaseTexture(tex);                  // 0x0001D610
    p->AttachProperty(t);
    if (camera) camera->AttachScreenPolygon(p.Get());   // 0x00019460
    return p;
}

// orig: HUDChickenWalker (0x58 bytes, ctor 0x000E8220, vtable 0x002A5248, dtor 0x000E6E80): the cockpit HUD of the
// level1-6 walker: two gauges (+0x24 the health, +0x28 the laser energy: a triangle over a backdrop whose third
// vertex follows the value along an arc), four damage indicators (+0x2C..+0x38: red bars at the screen's edges,
// faded in by "show_indicator"), the rocket icon (+0x44, made by the first update with a walker) and the rocket
// count text (+0x3C; the count shown +0x40, -1 at first).
struct HudChickenWalker : HudElement {
    // orig: a gauge (0x10 bytes, ctor 0x000E7930(kind, polygon, backdrop), dtor 0x000E6B00)
    struct Gauge {
        ni::NiPointer<ni::NiScreenPolygon> poly;      // +0x00
        ni::NiPointer<ni::NiScreenPolygon> back;      // +0x04
        ni::NiPointer<ni::NiMaterialProperty> material;   // +0x08 (the polygon's)
        s32 kind = 0;                                 // +0x0C (0 health: coloured by the value; 1 energy)
        Gauge(s32 k, ni::NiPointer<ni::NiScreenPolygon> p, ni::NiPointer<ni::NiScreenPolygon> b)
            : poly(std::move(p)), back(std::move(b)), kind(k) {
            material = MaterialOf(poly.Get());        // (+0)->+8 slot 3
            SetLevel(1.0f);                           // 0x000E70E0
            if (kind == 1 && material) {
                material->emissive = {0.0f, 1.0f, 1.0f};   // +0x40..+0x48
                ++material->revision;
            }
        }
        // orig: 0x000E70E0 (v): the third vertex at the angle (up + down) v - down from the gauge's centre (y 269,
        // 230 pixels): its y and its uv's v; the polygon's flags |= 5; a health gauge's colour.
        void SetLevel(f32 v) {
            const double a = (double(kCwAngleDown) + kCwAngleUp) * v - kCwAngleDown;   // FADD; FMUL; FSUB
            const double t = std::sin(a) / std::cos(a);   // FSIN / FCOS; FDIVP (#35: double here)
            const double y = 269.0f - t * 230.0f;     // FMUL [0x002A5210]; FSUBR [0x002A5214]
            if (poly && poly->vertices.size() > 2) poly->vertices[2].y = F(1.0 - double(kInv480) * y);
            if (poly && poly->uvs.size() > 2)
                poly->uvs[2].v = F((y - (269.0f - double(kCwUpper))) / 256.0f);   // FSUBR ST(1); FDIV [0x002A5218]
            if (poly) poly->flags |= 5;
            if (kind == 0) SetColour(v);              // 0x000E6C70
        }
        // orig: 0x000E6C70 (v): the emissive green above 0.75, yellow to green above 0.5, red to yellow below.
        void SetColour(f32 v) {
            f32 r, g;
            if (v > 0.75f) {                          // [0x00293A94]; TEST AH,0x41; JNE
                r = 0.0f, g = 1.0f;
            } else if (v > 0.5f) {                    // [0x00293AA0]
                const double d = double(v) - 0.5f;
                r = F(1.0 - (d + d));                 // FSUB; FADD ST(0),ST(0); FSUBR 1.0
                g = 1.0f;
            } else {
                r = 1.0f;
                g = F(double(v) + v);                 // FADD ST(0),ST(0)
            }
            if (!material) return;
            material->emissive = {r, g, 0.0f};
            ++material->revision;
        }
        void Show() { SetPolygonAlpha(poly.Get(), 1.0f), SetPolygonAlpha(back.Get(), 1.0f); }   // 0x000E6CF0
        void Hide() { SetPolygonAlpha(poly.Get(), 0.0f), SetPolygonAlpha(back.Get(), 0.0f); }   // 0x000E6D30
    };
    // orig: a damage indicator (0xC bytes, ctor 0x000E6D70(polygon), dtor 0x000E6BD0): its material's alpha 0 at first
    struct Indicator {
        ni::NiPointer<ni::NiScreenPolygon> poly;      // +0x00
        ni::NiPointer<ni::NiMaterialProperty> material;   // +0x04
        f32 timer = -1.0f;                            // +0x08
        explicit Indicator(ni::NiPointer<ni::NiScreenPolygon> p) : poly(std::move(p)) {
            material = MaterialOf(poly.Get());
            SetPolygonAlpha(poly.Get(), 0.0f);
        }
        // orig: 0x000E6E30: while the timer is not below 0: alpha = timer / DAT_002D7028 0.8, the timer - dt.
        void Tick() {
            if (timer < 0.0f) return;                 // FCOMP [0.0]; TEST AH,5; JNP
            if (ni::NiMaterialProperty* m = MaterialOf(poly.Get())) {
                ++m->revision;
                m->alpha = F(double(timer) / kCwIndicatorTime * 0.8f);   // FDIV; FMUL [0x00293C18]
            }
            timer = F(double(timer) - g_frameStep);   // FSUB [0x002D67E8]
        }
    };

    std::unique_ptr<Gauge> gauge[2];                  // +0x24 / +0x28
    std::unique_ptr<Indicator> indicator[4];          // +0x2C / +0x30 / +0x34 / +0x38
    std::unique_ptr<text::TextObject> text;           // +0x3C
    s32 rockets = -1;                                 // +0x40
    ni::NiPointer<ni::NiScreenPolygon> rocketIcon;    // +0x44
    f32 iconX = 0, iconY = 0, iconW = 0, iconH = 0;   // +0x48 / +0x4C / +0x50 / +0x54

    explicit HudChickenWalker(Hud& hud) : HudElement(hud, "cw_hud") {   // 0x00296FB4
        MakeGauges();                                 // 0x000E7A10
        MakeIndicators();                             // 0x000E7E00
        // the text: TextObject::Create(0), App->vf14() (0x00098F80), +0x60 = 0, +0x68 = 0, SetScale(2.0, 2.2)
        text.reset(text::TextObject::Create(0, hud.readFile, hud.level.renderer));
        if (text) {
            text->style.hAlign = 0;
            text->style.wordWrap = false;
            text->SetScale(2.0f, std::bit_cast<f32>(0x400CCCCDu));
        }
        rockets = -1;
        iconH = 85.0f, iconW = 85.0f;                 // [0x002A5224]
        iconX = std::bit_cast<f32>(0x43F99999u);      // [0x002A5228] 499.2
        iconY = std::bit_cast<f32>(0x41E66667u);      // [0x002A522C] 28.8
    }
    ~HudChickenWalker() override {                    // orig: 0x000E6E80
        if (text) text->RemoveAll();
    }

    // orig: 0x000E7A10: each gauge's backdrop (0x000E5F60 "HUD\cw_bar_bg.nif" 64 x 256, alpha; the right one's uvs
    // mirrored) and its triangle (0x000E73D0 "HUD\cw_bar.nif"), the left one at x 60, the right one at 640 - 60 - 64 +
    // 3, both at y 152; the triangles: the centre (61 / 640 or 582 / 640, 1 - 269 / 480) side and the arc's ends
    // 269 + 82.45 / 269 - 131.59 pixels down, x 0.0953125 / 0.909375.
    void MakeGauges() {
        const f32 yLow = F(1.0 - (double(kCwLower) + 269.0f) * kInv480);   // FADD; FMUL; FSUBR 1.0
        const f32 yMid = F(1.0 - double(269.0f) * kInv480);
        const f32 yHigh = F(1.0 - (269.0 - double(kCwUpper)) * kInv480);
        const double inv256 = 1.0 / 256.0f;           // FLD 1.0; FDIV [0x002A5218]
        const f32 uvLow = F((double(kCwUpper) + kCwLower) * inv256);
        const f32 uvHigh = F(inv256 * kCwUpper);
        const f32 uvU = F(230.0 / 64.0);              // [0x002A5210] / [0x002A521C]
        const f32 l0 = std::bit_cast<f32>(0x3DC33333u), l1 = std::bit_cast<f32>(0x3EEB3333u);   // 61 / 640, 294 / 640
        const f32 r0 = std::bit_cast<f32>(0x3F0B999Au), r1 = std::bit_cast<f32>(0x3F68CCCDu);   // 349 / 640, 582 / 640
        {
            static const ni::NiPoint2 kBackUV[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            ni::NiPointer<ni::NiScreenPolygon> back =
                hud.MakeIcon("HUD\\cw_bar_bg.nif", 64.0f, 256.0f, 60.0f, kCwTop, true, kBackUV, nullptr);   // 0x002A5290
            const ni::NiPoint3 v[3] = {{l0, yLow, 0}, {l1, yMid, 0}, {l0, yHigh, 0}};
            const ni::NiPoint2 uv[3] = {{0, uvLow}, {uvU, uvHigh}, {0, 0}};
            ni::NiPointer<ni::NiScreenPolygon> p = MakeWalkerPolygon(hud, 3, v, "HUD\\cw_bar.nif", uv, {1, 1, 1});
            gauge[0] = std::make_unique<Gauge>(0, p, back);   // new(0x10)
        }
        {
            static const ni::NiPoint2 kBackUV[4] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}};
            const f32 x = F(((640.0 - 60.0f) - 64.0f) + 3.0f);   // [0x002A527C] - [0x002A5220] - [0x0029560C] + [0x0029563C]
            ni::NiPointer<ni::NiScreenPolygon> back =
                hud.MakeIcon("HUD\\cw_bar_bg.nif", 64.0f, 256.0f, x, kCwTop, true, kBackUV, nullptr);
            const ni::NiPoint3 v[3] = {{r0, yMid, 0}, {r1, yLow, 0}, {r1, yHigh, 0}};
            const ni::NiPoint2 uv[3] = {{uvU, uvHigh}, {0, uvLow}, {0, 0}};
            ni::NiPointer<ni::NiScreenPolygon> p = MakeWalkerPolygon(hud, 3, v, "HUD\\cw_bar.nif", uv, {1, 1, 1});
            gauge[1] = std::make_unique<Gauge>(1, p, back);
        }
    }
    // orig: 0x000E7E00: the four indicators (0x000E7680 "HUD\cw_cursor.nif", red): top, bottom, left, right.
    void MakeIndicators() {
        const f32 a = std::bit_cast<f32>(0x3E99999Au), b = std::bit_cast<f32>(0x3F333333u);   // 0.3 / 0.7
        const f32 c = std::bit_cast<f32>(0x3F777777u), d = std::bit_cast<f32>(0x3F555555u);   // 29 / 30, 25 / 30
        const f32 e = std::bit_cast<f32>(0x3E2AAAABu);                                         // 1 / 6
        const f32 f = std::bit_cast<f32>(0x3F444444u), g = std::bit_cast<f32>(0x3E6EEEEFu);   // 23 / 30, 7 / 30
        const f32 h = std::bit_cast<f32>(0x3CCCCCCDu), i = 0.125f;                             // 0.025 / 0.125
        const f32 j = 0.875f, k = std::bit_cast<f32>(0x3F79999Au);                            // 0.875 / 0.975
        const ni::NiColor red{1.0f, 0.0f, 0.0f};
        const ni::NiPoint3 v0[4] = {{a, c, 0}, {a, d, 0}, {b, d, 0}, {b, c, 0}};
        const ni::NiPoint2 uv0[4] = {{1, 0}, {1, 1}, {0, 1}, {0, 0}};
        indicator[0] = std::make_unique<Indicator>(MakeWalkerPolygon(hud, 4, v0, "HUD\\cw_cursor.nif", uv0, red));
        const ni::NiPoint3 v1[4] = {{a, e, 0}, {a, 0, 0}, {b, 0, 0}, {b, e, 0}};
        const ni::NiPoint2 uv1[4] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}};
        indicator[1] = std::make_unique<Indicator>(MakeWalkerPolygon(hud, 4, v1, "HUD\\cw_cursor.nif", uv1, red));
        const ni::NiPoint3 v2[4] = {{h, f, 0}, {h, g, 0}, {i, g, 0}, {i, f, 0}};
        const ni::NiPoint2 uv2[4] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
        indicator[2] = std::make_unique<Indicator>(MakeWalkerPolygon(hud, 4, v2, "HUD\\cw_cursor.nif", uv2, red));
        const ni::NiPoint3 v3[4] = {{j, f, 0}, {j, g, 0}, {k, g, 0}, {k, f, 0}};
        const ni::NiPoint2 uv3[4] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}};
        indicator[3] = std::make_unique<Indicator>(MakeWalkerPolygon(hud, 4, v3, "HUD\\cw_cursor.nif", uv3, red));
    }
    // orig: 0x000E6A10: the text emptied, then "%d" (0x00296D4C) of +0x40 at ([0x002A5230] 0.84, [0x002A5234] 0.8).
    void UpdateText() {
        char buf[0x40] = {};
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(rockets));
        SetCounterText(text.get(), buf, std::bit_cast<f32>(0x3F570A3Du), std::bit_cast<f32>(0x3F4CCCCDu));
    }
    // orig: vf01 0x000E6FA0: active (vf18) and a player (S+0x94): the indicators' fade (+0x2C..+0x38 in order); with the
    // player's walker (+0x7B0): the rocket icon made (0x000E6A90: "HUD\rocket.nif" 85 x 85 at (499.2, 28.8), alpha),
    // and a changed rocket count (0x00042730) shown: 0 empties the text and hides the icon (alpha 0), else the icon's
    // alpha 1 and the text.
    void Update() override {
        if (!IsActive()) return;
        const Player* p = hud.level.player;
        if (!p) return;
        ChickenWalker* w = p->chicken.get();
        for (auto& ind : indicator)
            if (ind) ind->Tick();
        if (!w) return;
        if (!rocketIcon)
            rocketIcon = hud.MakeIcon("HUD\\rocket.nif", iconW, iconH, iconX, iconY, true, nullptr, nullptr);   // 0x002A5238
        const s32 n = static_cast<s32>(w->rockets);
        if (n == rockets) return;
        rockets = n;
        if (n == 0) {
            if (text) text->RemoveAll();
            SetPolygonAlpha(rocketIcon.Get(), 0.0f);
            return;
        }
        SetPolygonAlpha(rocketIcon.Get(), 1.0f);
        UpdateText();                                 // 0x000E6A10
    }
    void Draw() override { DrawText(*this, text.get()); }   // orig: vf02 0x000E69F0
    // orig: vf04 0x000E7180 (strcmp): "health" / "ammo" (0x00297208 / 0x00297214): the gauge +0x24 / +0x28 set to v
    // clamped to 0..1; "add_rockets" / "set_rockets" (0x002A5270 / 0x002A5264): the player's walker's rockets
    // (0x00042700 / 0x000426E0 with _ftol(v)); "show_indicator" (0x00296FBC): v 0 / 1 / 2 / 3 starts the indicator
    // +0x34 / +0x38 / +0x2C / +0x30 (timer = DAT_002D7028), any other value all four.
    void Command(const char* cmd, f32 v) override {
        if (!cmd) return;
        auto clamped = [](f32 x) {
            if (x > 1.0f) return 1.0f;                // FCOMP [1.0]; TEST AH,0x41; JNE (a NaN stays)
            if (x < 0.0f) return 0.0f;                // FCOMP [0.0]; TEST AH,5; JP
            return x;
        };
        if (std::strcmp(cmd, "health") == 0) {
            if (gauge[0]) gauge[0]->SetLevel(clamped(v));
        } else if (std::strcmp(cmd, "ammo") == 0) {
            if (gauge[1]) gauge[1]->SetLevel(clamped(v));
        } else if (std::strcmp(cmd, "add_rockets") == 0) {
            if (const Player* p = hud.level.player; p && p->chicken)
                p->chicken->AddRockets(static_cast<u32>(static_cast<s32>(static_cast<s64>(v))));   // _ftol
        } else if (std::strcmp(cmd, "set_rockets") == 0) {
            if (const Player* p = hud.level.player; p && p->chicken)
                p->chicken->SetRockets(static_cast<u32>(static_cast<s32>(static_cast<s64>(v))));
        } else if (std::strcmp(cmd, "show_indicator") == 0) {
            auto start = [&](int i) {
                if (indicator[i]) indicator[i]->timer = kCwIndicatorTime;
            };
            if (v == 0.0f) start(2);                  // TEST AH,0x44; JP
            else if (v == 1.0f) start(3);
            else if (v == 2.0f) start(0);             // [0x00293B20]
            else if (v == 3.0f) start(1);             // [0x0029563C]
            else
                for (int i = 0; i < 4; ++i) start(i);
        }
    }
    // orig: vf05 0x000E7070: a change of +0x15: showing sets both gauges' alpha 1 (0x000E6CF0); hiding sets them 0
    // (0x000E6D30), empties the text and hides the rocket icon.
    void SetActive(bool on) override {
        if (u8(on) == active) return;
        active = u8(on);
        if (on) {
            for (auto& g : gauge)
                if (g) g->Show();
            return;
        }
        for (auto& g : gauge)
            if (g) g->Hide();
        if (text) text->RemoveAll();
        SetPolygonAlpha(rocketIcon.Get(), 0.0f);
    }
};

// orig: HUDLives (0x4C bytes, ctor 0x000EAF00, vtable 0x002A5534)
struct HudLives : HudElement {
    std::unique_ptr<HudFlash> flash;                  // +0x24
    s32 shown = -1;                                   // +0x2C
    ni::NiPointer<ni::NiScreenPolygon> icon;          // +0x30
    f32 w = 48.0f, h = F(48.0f), x = kLivesX, y = F(kLivesY);   // +0x34 / +0x38 / +0x3C / +0x40
    u8 pulsing = 0;                                   // +0x44
    f32 half = 0;                                     // +0x48
    std::unique_ptr<text::TextObject> text;           // +0x28
    void Draw() override { DrawText(*this, text.get()); }   // orig: vf02 0x000EADB0
    explicit HudLives(Hud& hud) : HudElement(hud, "lives"), text(MakeCounterText(hud)) {   // 0x002A5560
        icon = hud.MakeIcon("HUD\\reanim.nif", w, h, x, y, true, nullptr, nullptr);
        if (ni::NiMaterialProperty* m = MaterialOf(icon.Get())) m->alpha = 1.0f, ++m->revision;
    }
    void Update() override {                          // orig: vf01 0x000EB070
        if (!IsActive()) return;
        if (flash) flash->Update();                   // 0x000EBFC0
        if (hud.level.lives != shown) {               // 0x000C12B0 (DAT_002D687C)
            if (!flash) {
                const f32 pos[2] = {kLivesFlashX, kLivesFlashY}, size[2] = {0.2f, 0.2f};
                flash = std::make_unique<HudFlash>(hud, pos, size, 1.0f, 0.75f, kLivesFlashSpin);
                flash->Attach(hud.SceneCamera());     // 0x000EC130 (S+0x8C +0x30)
            }
            flash->time = 0;
            shown = hud.level.lives;
            UpdateText();                             // 0x000EAE80
            pulsing = 1;
            half = F(double(48.0f) * 0.5f * 1.5f);
        }
        if (icon && pulsing && shown > 0) Pulse(*icon, 48.0f, half, pulsing);
    }
    void UpdateText() {                               // orig: 0x000EAE80
        if (!text) return;
        text->RemoveAll();
        if (shown < 0) return;
        char buf[0x40] = {};
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(shown));   // 0x00296D4C
        SetCounterText(text.get(), buf, kLivesFlashX, kLivesFlashY);
    }
    // orig: vf05 0x000EB2E0: hiding: the icon's alpha 0, the text emptied, the flash's polygons off the scene camera
    // (0x000EB950); showing: the icon's alpha 1, the text (0x000EAE80), the flash back on the camera (0x000EC130).
    void SetActive(bool on) override {
        if (active && !on) {
            active = 0;
            SetPolygonAlpha(icon.Get(), 0.0f);
            if (text) text->RemoveAll();
            if (flash) flash->Detach(hud.SceneCamera());
        } else if (!active && on) {
            active = 1;
            SetPolygonAlpha(icon.Get(), 1.0f);
            UpdateText();
            if (flash) flash->Attach(hud.SceneCamera());
        }
    }
};

// orig: HUDTimer (0x54 bytes, ctor 0x000ECC00, vtable 0x002A5698): its state (+0x28..+0x2F) is LevelData::hudTimer
// (the natives'); the text +0x24 ("%d:%02d") and, built with swamp = 1, the swamp bar: the backdrop, the two end
// icons and the progress bar +0x50 (its width +0x4C) filled by the player's progress along the stage's AI net.
struct HudTimerElement : HudElement {
    std::shared_ptr<HudTimer> state;
    std::unique_ptr<text::TextObject> text;           // +0x24
    f32 barWidth = 0;                                 // +0x4C
    ni::NiPointer<ni::NiScreenPolygon> bar;           // +0x50
    HudTimerElement(Hud& hud, std::shared_ptr<HudTimer> s) : HudElement(hud, "timer"), state(std::move(s)) {
        // the text: TextObject::Create(0), App->vf14() (0x00098F80), +0x60 = 0, +0x68 = 0, SetScale(2.0, 2.2)
        text.reset(text::TextObject::Create(0, hud.readFile, hud.level.renderer));
        if (text) {
            text->style.hAlign = 0;
            text->style.wordWrap = false;
            text->SetScale(2.0f, std::bit_cast<f32>(0x400CCCCDu));
        }
        if (!state->swamp) {                          // +0x15 = 0 and RemoveAll when it was set
            if (active) {
                active = 0;
                if (text) text->RemoveAll();
            }
            return;
        }
        // 0x000ECC96..: the polygons of 0x000E5F60 (all with an alpha property; the scene camera)
        ni::NiPointer<ni::NiScreenPolygon> back =
            hud.MakeIcon("HUD\\swamp_bar.nif", 256.0f, 32.0f, 192.0f, 400.0f, true, nullptr, nullptr);   // 0x002A56E4
        hud.MakeIcon("HUD\\swamp_cl.nif", 32.0f, 32.0f, 156.0f, 400.0f, true, nullptr, nullptr);       // 0x002A56D0
        hud.MakeIcon("HUD\\swamp_fl.nif", 32.0f, 64.0f, 452.0f, 384.0f, true, nullptr, nullptr);      // 0x002A56BC
        if (ni::NiMaterialProperty* m = MaterialOf(back.Get())) {   // emissive (0, 0.3, 0.4), alpha 1
            m->emissive = {0.0f, std::bit_cast<f32>(0x3E99999Au), std::bit_cast<f32>(0x3ECCCCCDu)};
            m->revision += 2;
            m->alpha = 1.0f;
        }
        bar = hud.MakeIcon("HUD\\swamp_bar.nif", 256.0f, 32.0f, 192.0f, 400.0f, true, nullptr, nullptr);
        if (bar && bar->vertices.size() >= 2) barWidth = F(double(bar->vertices[1].x) - bar->vertices[0].x);
        if (ni::NiMaterialProperty* m = MaterialOf(bar.Get())) {   // emissive (0, 0.75, 1), alpha 1
            m->emissive = {0.0f, 0.75f, 1.0f};
            m->revision += 2;
            m->alpha = 1.0f;
        }
    }
    ~HudTimerElement() override {                     // orig: 0x000EC870 (RemoveAll, the text deleted)
        if (text) text->RemoveAll();
    }
    void Update() override { state->Update(); }       // vf01 0x000EC760 (no activity test)
    void Command(const char* cmd, f32 v) override { state->Command(cmd, v); }   // vf04 0x000EC7B0
    // orig: 0x000EC900: the text emptied, then "%d:%02d" (0x002A56B4) of _ftol(+0x28) / 60 and % 60 (IDIV), widened
    // (0x000D2CB0, 0x40), at ([0x002A5674] 0.5, [0x002A5678] 0.23): AddString(.., -1).
    void UpdateText() {
        if (!text) return;
        text->RemoveAll();
        const s32 n = s32(s64(state->time));          // _ftol 0x00167110
        char buf[0x40] = {};
        std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(n / 60), static_cast<int>(n % 60));
        std::u16string w;
        for (const char* c = buf; *c && w.size() < 0x3F; ++c) w.push_back(char16_t(u8(*c)));
        text->x = 0.5f, text->y = std::bit_cast<f32>(0x3E6B851Fu);
        text->AddString(w, -1.0f);
    }
    // orig: vf02 0x000EC9B0: active (vf18) and running (+0x2C): the text rebuilt, its camera's Update and Click; with
    // the swamp bar, a player (S+0x94) and +0x50: the current stage's (S+0x5C) index i in the table 0x002D70A0 (15
    // names "Swamp0A".."Swamp2H", _stricmp; none: nothing), the AI net of the stage's name (S+0x114 0x000313E0), the
    // walk from the node nearest to the stage's "Start" waypoint (0x00031D80 of its position) along the chain (a node
    // of one link continues through it, others through the link that is not the previous node; up to the next node of
    // one link): the node nearest to the player at step k of n; v = F(i [0x0029A53C] 1/15) + k / n (0 for n = 0)
    // 1/15, at most 1; the bar's uvs 1 / 2 u = v, its vertices 1 / 2 x = v +0x4C + vertex 0 x; flags |= 5.
    void Draw() override {
        if (!IsActive() || !state->running) return;
        UpdateText();                                 // 0x000EC900
        if (text) {
            text->UpdateCamera();                     // 0x00098D70
            text->Click();                            // 0x00098DB0
        }
        if (!state->swamp) return;
        LevelData& L = hud.level;
        if (!L.player || !bar) return;
        const ni::NiPoint3 pos = L.player->GetWorldPosition();   // vf34
        Stage* stage = L.currentStage;
        if (!stage || stage->name.empty()) return;    // (+0xC: the name pointer)
        static const char* const kStages[15] = {"Swamp0A", "Swamp1A", "Swamp1B", "Swamp1C", "Swamp1D",
                                                "Swamp1E", "Swamp1G", "Swamp2A", "Swamp2B", "Swamp2C",
                                                "Swamp2D", "Swamp2E", "Swamp2F", "Swamp2G", "Swamp2H"};
        u32 i = 0;
        while (i < 15 && crt::StrICmp(kStages[i], stage->name.c_str()) != 0) ++i;
        if (i == 15) return;
        const f32 kFifteenth = std::bit_cast<f32>(0x3D888889u);   // [0x0029A53C] 1 / 15
        const f32 base = F(double(i) * kFifteenth);   // FILD (unsigned fix-up); FMUL; FSTP
        AiNet* net = L.FindAINet(stage->name.c_str());
        if (!net) return;
        AiNetNode* nearest = net->Nearest(pos);
        const StageWaypoint* start = stage->FindWaypoint("Start");   // 0x0029B9C4
        // (a missing waypoint or an empty net: the original reads through null; nothing here)
        AiNetNode* cur = start ? net->Nearest(start->position) : nullptr;
        if (!cur) return;
        AiNetNode* prev = nullptr;
        s32 at = 0, steps = 0;
        do {
            if (cur == nearest) at = steps;
            AiNetNode* next = NodeAt(*net, LinkNode(*cur, 0));   // 0x00031C00 / 0x00031C20
            if (cur->count != 1 && next == prev) next = NodeAt(*net, LinkNode(*cur, 1));
            prev = cur;
            cur = next;
            ++steps;
        } while (cur && cur->count != 1);
        if (cur == nearest) at = steps;
        double v = steps > 0 ? double(at) / double(steps) : 0.0;   // FILD; FIDIV (or FLD [0x00293AB8] 0)
        v = v * kFifteenth + base;
        if (v > 1.0) v = 1.0;                         // FCOM [0x00293A98]; TEST AH,0x41; JNE (NaN kept)
        if (bar->uvs.size() >= 3) bar->uvs[1].u = F(v), bar->uvs[2].u = F(v);   // FST +8 / +0x10
        if (bar->vertices.size() >= 3) {
            bar->vertices[1].x = F(v * barWidth + bar->vertices[0].x);
            bar->vertices[2].x = F(v * barWidth + bar->vertices[0].x);
        }
        bar->flags |= 5;
    }
    // orig: vf05 0x000ECBC0: hiding empties the text; showing, with the timer running, rebuilds it (0x000EC900).
    void SetActive(bool on) override {
        if (active && !on) {
            active = 0;
            if (text) text->RemoveAll();
        } else if (!active && on) {
            active = 1;
            if (state->running) UpdateText();
        }
    }
};

}  // namespace

ni::NiMaterialProperty* MaterialOf(ni::NiScreenPolygon* p) {
    return p ? p->FindProperty<ni::NiMaterialProperty>() : nullptr;
}

// ---- HudElement / Hud ---------------------------------------------------------------------------------------------
HudElement::HudElement(Hud& h, const char* n) : hud(h) {   // orig: 0x000E57F0
    std::strncpy(name, n, 16);
    name[15] = 0;                                     // +0x13 = 0 (L: the original clears +0x13, name[15])
    hud.elements.insert(hud.elements.begin(), this);  // the head of +4
}

bool HudElement::IsActive() const { return active && hud.enabled; }   // orig: 0x000E58B0

Hud::Hud(LevelData& l, std::function<bool(const std::string&, std::vector<u8>&)> rf)
    : level(l), readFile(std::move(rf)) {}            // orig: 0x000E5470 (+0 = 1, +4 = +8 = 0)

Hud::~Hud() = default;

void Hud::Build(const std::string& n) {               // orig: 0x000C12F0
    auto add = [this](HudElement* e) { owned.emplace_back(e); };
    add(new HudInfo(*this));                          // HUDInfo 0x000EA100 (0x3C)
    add(new HudCheckpoint(*this));                    // HUDCheckpoint 0x000E6950 (0x44)
    add(new HudChargeText(*this));                    // HUDChargeText 0x000E6890 (0x44)
    add(new Hud1Up(*this));                           // HUD1Up 0x000E5FF0 (0x44)
    auto timer = [&](bool swamp) {
        if (!level.hudTimer) level.hudTimer = std::make_shared<HudTimer>(swamp);
        add(new HudTimerElement(*this, level.hudTimer));
    };
    const char* s = n.c_str();
    if (crt::StrICmp(s, "Level1-6") == 0) {           // 0x0029BE94: HUDHead, HUDChickenWalker, HUDNibblerCnt, HUDLives
        add(new HudHead(*this, n));
        add(new HudChickenWalker(*this));
        add(new HudCounter(*this, true));
        add(new HudLives(*this));
    } else if (crt::StrICmp(s, "Level4-1") == 0) {    // 0x002A32F0: HUDHead, HUDNibblerCnt, HUDTimer(1)
        add(new HudHead(*this, n));
        add(new HudCounter(*this, true));
        timer(true);
    } else if (crt::StrNICmp(s, "Level1", 6) == 0 || crt::StrICmp(s, "Level5-3") == 0) {   // 0x002A32DC / 0x00297B94
        add(new HudHead(*this, n));                   // HUDHead, HUDHealth, HUDWeapons, HUDItems, HUDNibblerCnt,
        add(new HudHealth(*this));                    // HUDTimer(0), HUDLives
        add(new HudWeapons(*this));
        add(new HudItems(*this));
        add(new HudCounter(*this, true));
        timer(false);
        add(new HudLives(*this));
    } else {                                          // HUDHead, HUDHealth, HUDCharge, HUDItems, HUDNibblerCnt,
        add(new HudHead(*this, n));                   // HUDTimer(0), HUDLives
        add(new HudHealth(*this));
        add(new HudCharge(*this));
        add(new HudItems(*this));
        add(new HudCounter(*this, true));
        timer(false);
        add(new HudLives(*this));
    }
    if (crt::StrNICmp(s, "Level5", 6) != 0) add(new HudCounter(*this, false));   // HUDCollectableCnt (0x002A32A4)
    SetActiveAll(true);                               // 0x000E5A60(1)
}

void Hud::Update() {                                  // orig: 0x000E58E0
    if (!enabled) return;
    for (size_t i = 0; i < elements.size(); ++i) elements[i]->Update();
}

void Hud::Draw() {                                    // orig: 0x000E5900
    if (!enabled) return;
    for (size_t i = 0; i < elements.size(); ++i) elements[i]->Draw();
}

HudElement* Hud::Find(const char* n) const {          // orig: 0x000E5920
    if (!n) return nullptr;
    for (HudElement* e : elements)
        if (std::strncmp(n, e->name, 15) == 0) return e;
    return nullptr;
}

void Hud::SetActiveAll(bool on) {                     // orig: 0x000E5A60
    for (size_t i = 0; i < elements.size(); ++i) elements[i]->SetActive(on);
}

void Hud::SetActive(const char* n, bool on) {         // orig: 0x000E5B60
    if (HudElement* e = Find(n)) e->SetActive(on);
}

void Hud::Command(const char* n, const char* cmd, f32 v) {   // orig: 0x000E5B90
    if (!n || !cmd) return;
    if (HudElement* e = Find(n)) e->Command(cmd, v);
}

void Hud::Command(const char* n, const char* cmd, const char* arg) {   // orig: 0x000E5BC0
    if (!n || !cmd || !arg) return;
    if (HudElement* e = Find(n)) e->Command(cmd, arg);
}

ni::NiCamera* Hud::SceneCamera() const {
    return level.camera ? level.camera->sceneCamera.Get() : nullptr;   // S+0x8C +0x30
}

ni::NiObject* Hud::Texture(const char* file) {        // orig: 0x000E59E0
    if (!file) return nullptr;
    for (const Tex& t : textures)                     // 0x000E5960 (strcmp)
        if (t.name == file) return t.texture.Get();
    Tex t;                                            // 0x000E5480: NiStream::Load, the first object's NiTexture
    t.name = file;
    std::vector<u8> bytes;
    if (readFile && readFile(file, bytes)) {
        ni::NiStream s;
        if (s.Load(bytes) && !s.TopLevelLinks().empty())
            t.texture = dynamic_cast<ni::NiTexture*>(s.ResolveLink(s.TopLevelLinks()[0]));
    }
    if (!t.texture) return nullptr;                   // (the entry is unlinked and deleted, 0x000E55E0)
    textures.insert(textures.begin(), t);
    return textures.front().texture.Get();
}

ni::NiPointer<ni::NiScreenPolygon> Hud::MakePolygon(ni::NiObject* texture, f32 w, f32 h, f32 x, f32 y,
                                                    const ni::NiColor& rgb, bool alpha, const ni::NiPoint2* uvs,
                                                    ni::NiCamera* camera) {   // orig: 0x000E5C00
    if (!camera) camera = SceneCamera();
    if (!camera) return {};
    static const ni::NiPoint2 kUV[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    const f32 W = F(double(w) * kInv640);
    const f32 H = F(double(h) * kInv480);
    const double L = double(x) * kInv640;             // (kept)
    const double T = 1.0 - double(y) * kInv480;       // FSUBR [1.0] (kept)
    const ni::NiPoint3 v[4] = {{F(L), F(T - H), 0}, {F(L + W), F(T - H), 0}, {F(L + W), F(T), 0}, {F(L), F(T), 0}};
    ni::NiPointer<ni::NiScreenPolygon> p(new ni::NiScreenPolygon(4, v, uvs ? uvs : kUV, nullptr));
    auto* t = new ni::NiTexturingProperty;            // apply mode 2, the base map's filter 1 (0x0001D6B0)
    t->applyMode = 2;
    if (!t->HasMap(0)) t->SetBaseTexture(nullptr);    // (0x0001D6B0 makes the base map: Map::Map 0x0001D570)
    t->maps[0].filterMode = 1;
    if (texture) t->SetBaseTexture(texture);
    p->AttachProperty(t);
    auto* m = new ni::NiMaterialProperty;             // all four colours, shininess 0, alpha 1
    m->ambient = m->diffuse = m->specular = m->emissive = rgb;
    m->shininess = 0.0f, m->alpha = 1.0f;
    m->revision += 6;
    p->AttachProperty(m);
    if (alpha) {
        auto* a = new ni::NiAlphaProperty;            // (0xEC & 0xFEED) | 0xED
        a->flags = 0x00ED;
        p->AttachProperty(a);
    }
    camera->AttachScreenPolygon(p.Get());             // 0x00019460
    return p;
}

ni::NiPointer<ni::NiScreenPolygon> Hud::MakeIcon(const char* file, f32 w, f32 h, f32 x, f32 y, bool alpha,
                                                 const ni::NiPoint2* uvs, ni::NiCamera* camera) {   // 0x000E5F60
    return MakePolygon(Texture(file), w, h, x, y, {1, 1, 1}, alpha, uvs, camera);
}

void PrepareLitModel(ni::NiAVObject* object) { PrepareHeadModel(object); }   // orig: 0x000966A0

}  // namespace fut::game
