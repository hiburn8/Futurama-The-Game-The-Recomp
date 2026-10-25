// The Intermission's AVI path; see Intermission.h and docs/analysis/intermission_port.md.
#include "game/level/Intermission.h"

#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "game/input/Pad.h"

#include <bit>
#include <cmath>

#include <cstdio>
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
}  // namespace

void Intermission::Prepare(const char* name) {        // orig: 0x000D5810
    nullName = name && crt::StrICmp(name, "NULL") == 0;   // NEG; SBB; INC (a NULL name crashes the original)
    for (Views& e : table) {                          // 0x002D6DB0, 2 entries of 12 bytes
        if (name && crt::StrICmp(e.name, name) == 0) {
            e.current = 1;
            ++e.views;
        } else {
            e.current = 0;
        }
    }
    // orig: 0x000D5050(S+0x18, name): ".avi" = the last four characters
    const size_t n = name ? std::strlen(name) : 0;
    if (n >= 4 && crt::StrICmp(name + n - 4, ".avi") == 0) {
        camera = MakeCamera();                        // +0x3C = 0x000D3DE0() (+0x11A = 0 there)
        movieName.assign(name, static_cast<size_t>(std::strchr(name, '.') - name));   // strcpy, cut at the first '.'
        movie = true;                                 // +0x140
    } else {
        movie = false;
        NotPorted("Intermission: an RTM cutscene (0x000D4AD0)");
    }
}

ni::NiCamera* Intermission::MakeCamera() const {      // orig: 0x000D3DE0
    auto* c = new ni::NiCamera;                       // new(0x250) 0x001009D0
    // FLD [0x002F9274] (pi, set at start-up: 0x40490FDB); FMUL [0x00293B50] (0.25); FPTAN; FSTP ST(0); then
    // FLD [0x00293A94] (0.75) FMUL ST(1): the frustum (-t, t, 0.75 t, -0.75 t, 1, 1000) with t = tan(pi / 4)
    const double t = std::tan(double(std::bit_cast<f32>(0x40490FDBu)) * 0.25f);
    const double v = 0.75f * t;
    const f32 frustum[6] = {F(-t), F(t), F(v), F(-v), 1.0f, 1000.0f};   // (+0x20 .. +0x34; 0x447A0000)
    c->renderer = renderer;                           // +0x1A8 = App->vf14() (AddRef'd)
    c->port[0] = 0.0f, c->port[1] = 1.0f, c->port[2] = 1.0f, c->port[3] = 0.0f;   // +0x14C (0, 1, 1, 0)
    c->SetFrustum(frustum);                           // 0x00011960
    return c;
}

void Intermission::Render() {                         // orig: vf0C 0x000D6370
    // (+0x118: 0x000D5E90, +0x119 / +0x11A: the fade quads detached from the camera: mode 0 only, never set)
    // 0x000D4F80("SYSTEM", "PRERENDER", 3, ...): no handler
    if (!movie) {                                     // +0x140
        if (renderer) renderer->ClearBuffer(nullptr, 5);   // App->vf14()->vf44(0, 5)
        ni::BeginOutlineClick();                      // 0x0013C7E0
    }
    // DAT_002F90D4 = 1 around the Click (written only here: no reader in the image)
    if (camera) ni::VisitForCulling(*camera, nullptr);   // 0x00100CE0 (no scene: +0x15C)
    // +0x40's Click: never set (the ctor's 0 and 0x000D3D9E's 0 are its only writes)
    // 0x000D4F80("SYSTEM", "POSTRENDER", 4, ...): no handler
}

void Intermission::Start() {                          // orig: 0x000D4950
    skippable = false;                                // +0x2D
    startTicks = queryCounter ? queryCounter() : 0;   // +0x138 = QueryPerformanceCounter
    if (movie) {
        fadeTargetBits = 0x3E000000u;                 // +0x120 = 0.125f (overwritten by 0x000D3560)
        fadePhase = true;                             // +0xE0
    } else {
        NotPorted("Intermission: FsStartCutscene without a movie (fade quads, cutscene nodes)");
    }
    BeginActive();
}

void Intermission::BeginActive() {                    // orig: 0x000D3560
    if (!nullName && !movie) NotPorted("Intermission: the RTM objects (0x000D6450)");
    if (input) input->SetRumbleEnabled(false, 0.0f);  // Input_SetRumbleEnabled(0, 0)
    fade = 0;                                         // +0x11C (+0x20 = +0x10D = +0x2C = 0: RTM state)
    active = true;                                    // +0x34
    fadeTargetBits = 0x3DCCCD38u;                     // +0x120
}

bool Intermission::Update() {                         // orig: vf08 0x000D61B0
    if (!active) return false;
    if (movie) {
        UpdateMovie();
        return true;
    }
    NotPorted("Intermission: the RTM / NULL update (0x000D5C40, 0x000D3F10)");
    return true;
}

void Intermission::UpdateMovie() {                    // orig: 0x000D5BB0
    if (fadePhase) {
        if (FadeDone()) {                             // 0x000D3710
            fadePhase = false;
            if (movieStarted) {                       // the fade after the movie: finished
                movieStarted = false;
                movie = false;
                active = false;
            } else {
                movieStarted = true;
                if (playMovie) (void)playMovie(movieName.c_str());   // App::PlayMovie (result ignored)
            }
        }
        return;
    }
    if (movieStarted && !(isMoviePlaying && isMoviePlaying())) {
        fadeTargetBits = 0;                           // +0x120 = 0
        fadePhase = true;
        End();
    }
}

void Intermission::End() {                            // orig: 0x000D5790
    if (movie && playMovie) (void)playMovie(nullptr);   // App::PlayMovie(NULL): Sound_ResumeAll 0x000A2468
    movieStarted = false;
    movie = false;
    active = false;
    camera = nullptr;                                 // +0x3C released
    fade = 0;
    fadeTargetBits = 0x3DCCCD38u;
    if (input) input->SetRumbleEnabled(true, 1.0f);
    fadePhase = true;
    // orig: 0x000D4F80("SYSTEM", "ENDOFCUTSCENE", 5, 0, 0): no "SYSTEM" handler is ever registered
}

bool Intermission::Skippable() const {                // orig: 0x000D31B0
    if (skipAll) return true;
    for (const Views& e : table)
        if (e.current && e.views <= 1) return false;  // CMP [..], 1; JBE
    if (skippable) return true;
    if (!queryCounter) return false;
    const double s = double(static_cast<s64>(queryCounter() - startTicks)) * (1.0 / double(counterFrequency));
    return s > 2.5f;                                  // FCOMP [0x00293B10]; TEST AH,0x41; JNE -> false
}

}  // namespace fut::game
