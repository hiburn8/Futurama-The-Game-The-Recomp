// The App and its context machine; see App.h.
#include "game/app/App.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiRenderer.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "game/app/Breakdown.h"
#include "game/app/Contexts.h"
#include "game/app/ErrorOverlay.h"
#include "game/app/Extras.h"
#include "game/app/Frontend.h"
#include "game/app/MemoryCards.h"
#include "game/app/PauseMenu.h"
#include "game/defs/IngameEvent.h"
#include "game/level/Dialogue.h"
#include "game/level/Session.h"
#include "game/level/LevelLoader.h"
#include "game/text/LanguageDb.h"

#include <cstdio>
#include <cstring>

namespace fut::game::app {

using ni::detail::F;

// ---- contexts ------------------------------------------------------------------------------------------------------

Context::Context(App& app, const char* name) : name(name), app_(app) {}   // orig: 0x0002C320

bool Context::Enter() {
    return OnEnter();
}

void Context::Exit() {
    OnExit();
}

// ---- level records -------------------------------------------------------------------------------------------------

int LevelRecords::Index(const char* n) {              // orig: 0x000F39D0
    if (!n) return -1;
    if (crt::StrICmp(n, "always") == 0) return 0x17;
    if (std::strlen(n) != 8 || crt::StrNICmp(n, "level", 5) != 0) return -1;
    const int a = n[5] - '0', b = n[7] - '0';         // (n[6] is not tested)
    if (a <= 0 || a >= 6 || b <= 0 || b >= 8) return -1;
    switch (a) {
    case 1: return b - 1;
    case 2: return b < 7 ? b + 6 : -1;
    case 3: return b < 7 ? b + 12 : -1;
    case 4: return b < 2 ? 19 : -1;
    default: return b < 4 ? b + 19 : -1;
    }
}

bool LevelRecords::Flag(const char* name) const {     // orig: 0x000F3B10
    if (name && crt::StrICmp(name, "always") == 0) return true;
    const int i = Index(name);
    return i >= 0 && records[size_t(i)].allNibblers != 0;
}

void LevelRecords::SetFlag(const char* name, bool on) {   // orig: 0x000F3B40
    const int i = Index(name);
    if (i >= 0) records[size_t(i)].allNibblers = on ? 1 : 0;
}

s32 LevelRecords::BestTime(const char* name) const {  // orig: 0x000F3B60
    const int i = Index(name);
    return i >= 0 ? records[size_t(i)].bestTime : -1;
}

void LevelRecords::SetBestTime(const char* name, s32 t) {   // orig: 0x000F3B90
    const int i = Index(name);
    if (i >= 0) records[size_t(i)].bestTime = t;
}

// ---- the App -------------------------------------------------------------------------------------------------------

App::App(AppServices s) : services(std::move(s)), extras(std::make_unique<ExtrasData>()) {   // orig: 0x000A22D0 / 0x000D2A40
    // (the process-wide front-end globals start as in the image: one App per process)
    g_menuInputMode = 0;
    g_feOptionsChain = g_feCheckChain = g_feExtrasChain = 0;
    qpf_ = services.qpf ? services.qpf : 1000000;
    // (the sound system exists before any context: the port makes it here, App::Init 0x000D2730 news it after the
    // archives; nothing between the two uses it)
    sound_ = services.sound;
    invQpf_ = F(1.0 / double(qpf_));                  // 0x000D2C20's first call: FILD QPF; FDIVR [0x00293A98] 1.0
    if (!services.readPad) {
        static const input::GamepadState neutral{};
        services.readPad = [](u32 port) { return port == 0 ? &neutral : nullptr; };
    }
    streams.readFile = services.readFile;
    // the App's file keeping: the front end's files go to it, a level's to the level it hosts
    streams.keepFile = [this](stream::Request& r, std::unique_ptr<ni::NiStream> f) {
        if (r.owner == "Frontend") return;            // (M.ClearObjects: the scene and the menus hold what they use)
        if (ingame && ingame->Level()) KeepLevelFile(*ingame->Level(), r, std::move(f));
    };
}

App::~App() {
    movie.reset();                                    // (its close draws with the renderer and the sound system)
    // (the contexts first: the Ingame context's level holds requests of the stream manager)
    current_ = next_ = nullptr;
    intro = nullptr, frontend = nullptr, attract = nullptr, ingame = nullptr, death = nullptr;
    contexts_.clear();
    pause.reset();                                    // (its load panel releases sounds: before the sound system)
}

const LevelData* App::CurrentLevel() const {
    const LoadedLevel* l = ingame ? ingame->Level() : nullptr;
    return l ? l->level.get() : nullptr;
}

void App::Srand(u32 seed, [[maybe_unused]] u32 site) {   // orig: 0x0016694F (site: the trace)
    crt::SeedRand(seed);
}

bool App::Init(const char* directLevel) {            // orig: 0x000D2730 + 0x000A2730
    // the renderer 0x0014C200 (App+0x104: services.renderer); Input_Init 0x0009C330: four pads (each joins the list),
    // XInputOpen for the connected port 0, Reset, SetDisconnected(0)
    for (u32 port = 0; port < 4; ++port) {
        pads[port] = std::make_unique<input::Pad>(input, port);
        input.pads[port] = pads[port].get();
    }
    pads[0]->SetDeviceOpen(true);
    pads[0]->Reset();
    pads[0]->SetDisconnected(false);
    // (no renderer: Init fails in the original; the port's harnesses run without one) the clear colour black: vf6C
    if (services.renderer) services.renderer->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
    text = std::make_unique<text::TextManager>(services.readFile);
    text->SetLanguage(services.language);
    // (port) the texture / pixel data sharing are off until 0x000A2730 turns them on (DAT_002F9AA0 / DAT_002F973C are
    // 0 in the image): App::Init's own files (the error overlay's, the memory cards') share nothing. The port's
    // default is on (the harnesses without an App); the lists are empty at this point (a new process).
    ni::SetTextureSharing(false);
    ni::SetPixelDataSharing(false);
    // the font manager, sysNTSC.img (App+0xE8 = 1), the error overlay 0x000F3470
    errorOverlay = std::make_unique<ErrorOverlay>(*this);
    memoryCards = std::make_unique<MemoryCards>(*this);   // new(0xA4) 0x000E4B30 -> +0xF4
    // FutNTSC%02d.img, the sound system new(0x7C) -> +0xF0, the hint text new(0xC) 0x000255E0 -> +0x100
    memoryCards->Init();                              // 0x000E5250
    // 0x000A2730: dt = 1/60 (NTSC), g_fixedFrameStep = 1, the clears / presents, the profiler, App+0xA8, App+0xAC =
    // "Level1-1", the contexts:
    g_frameStep = F(1.0 / 60.0);                      // 0x3C888889
    ni::SetTextureSharing(true);                      // 0x00118930(1) (0x000A283F)
    ni::SetPixelDataSharing(true);                    // 0x001119D0(1)
    // App+0xA8 = new(0x1FC) 0x000A2290: the records' vector ctor, the defaults 0x000A1B70 (the options globals and the
    // records), DAT_002E79A4 = 0, the dirty flag DAT_002E79A8 = 0, DAT_002E79BC = 0 (no other access), +0 = 0
    levelTargets = LevelTargets{};
    RestoreDefaults(*this);
    options.dirty = false;
    SetLevelName("Level1-1");
    auto add = [this](std::unique_ptr<Context> c) {
        Context* p = c.get();
        contexts_.push_back(std::move(c));
        if (!current_) current_ = p;                  // (the first registered becomes current)
        return p;
    };
    intro = static_cast<IntroContext*>(add(std::make_unique<IntroContext>(*this)));         // new(0x40) 0x0002B440
    frontend = static_cast<FrontendContext*>(add(std::make_unique<FrontendContext>(*this)));   // new(0x1D0)
    attract = static_cast<AttractContext*>(add(std::make_unique<AttractContext>(*this)));   // new(0x18)
    ingame = static_cast<IngameContext*>(add(std::make_unique<IngameContext>(*this)));      // new(0x1C)
    death = static_cast<DeathContext*>(add(std::make_unique<DeathContext>(*this)));         // new(0x48)
    if (!dialogue) {                                  // DAT_002EBDAC: new(0xC) 0x000D7760 unless made (0x000A2B21)
        dialogue = std::make_shared<DialogueDisplay>();
        dialogue->CreateText(services.readFile, services.renderer);   // (TextObject::Create(App+0xFC, 0), App->vf14())
    }
    // the Intermission 0x000D4E50
    intermission.input = &input;
    intermission.renderer = services.renderer;        // (App->vf14(): the movie camera's)
    intermission.playMovie = [this](const char* name) { return PlayMovie(name, name ? 0x000D5C09 : 0x000D57AC); };
    intermission.isMoviePlaying = [this] { return IsMoviePlaying(); };
    intermission.queryCounter = [this] { return qpc; };
    intermission.counterFrequency = qpf_;
    if (directLevel) {                                // (the oracle runner's direct start)
        SetLevelName(directLevel);
        current_ = ingame;
    }
    if (!current_ || !current_->Enter()) return false;
    stepPosition_ = qpc;                              // App+0xC8 = QueryCounter
    fpsStart_ = qpc;                                  // App+0xD8
    // BUILD.TXT into DAT_002E7B08 (the version text: render)
    return true;
}

void App::Shutdown() {                                // orig: 0x000D29B0
    if (current_) current_->Exit();
    current_ = nullptr;
}

Context* App::FindContext(const char* name) const {   // orig: 0x000A3120
    for (const auto& c : contexts_)
        if (crt::StrICmp(c->name.c_str(), name) == 0) return c.get();
    return nullptr;
}

bool App::RequestContext(const char* name, [[maybe_unused]] u32 site) {   // orig: 0x000A3160
    Context* c = FindContext(name);
    if (!c) return false;
    next_ = c;
    return true;
}

void App::SetLevelName(const char* name) {            // orig: 0x000A23D0
    if (!name) return;                                // (NULL: the getter form)
    std::memset(levelName_, 0, sizeof levelName_);
    std::strncpy(levelName_, name, 8);
}

void App::EndLevel(const LevelData& level, const char* next, s32 seconds) {   // orig: 0x000B05C6..0x000B06A7
    LevelRecords& rec = records;                      // App+0xA4 (0x000A25B0)
    const char* name = level.name.c_str();            // S+0xC
    const s32 best = rec.BestTime(name);              // 0x000F3B60 (-1: none)
    if (level.nibblerCount > 0 && level.nibblersPickedUp == level.nibblerCount)   // S+0x190 > 0 (JLE), == S+0x18C
        rec.SetFlag(name, true);                      // 0x000F3B40(S+0xC, 1)
    if (best == -1 || best > seconds)                 // CMP ESI,-1; JE set; CMP ESI,EDI; JLE skip (signed)
        rec.SetBestTime(name, seconds);               // 0x000F3B90
    SetLevelName(next);                               // 0x000A23D0
    if (!breakdown) breakdown = std::make_unique<Breakdown>(*this);   // DAT_002E8EEC = new(0x94) 0x000A4E70
    breakdown->Open();                                // 0x000A6340
    breakdown->open = true;                           // +1
}

bool App::PlayMovie(const char* name, [[maybe_unused]] u32 site) {   // orig: 0x000A2410
    if (movie) {                                      // 0x000EE220, delete, Mem_CoalesceAll (no game heap in the port)
        movie.reset();
    }
    if (!name) {
        Sound().ResumeAll();                          // Sound_ResumeAll 0x000A2468
        return false;
    }
    MovieService* movies = services.movies;
    movie = std::make_unique<MoviePlayer>(*this, *movies);   // new(0x48)
    if (movie->AcquireDevice()) {                     // 0x000EDED0 (none under the oracle's emulator)
        char full[0x100];
        std::snprintf(full, sizeof full, "%s-%s", name, ntsc ? "NTSC" : "PAL");   // "%s-%s" with DAT_002E7C10
        Sound().PauseAll();                           // Sound_PauseAll 0x000CAFD0
        if (movie->Open(full)) return true;           // 0x000EE2B0
    }
    movie.reset();                                    // (no Mem_CoalesceAll, and no Sound_ResumeAll after PauseAll)
    return false;
}

bool App::IsMoviePlaying() {                          // orig: 0x000A2550
    if (!movie) return false;
    if (!movie->Finished()) return true;              // 0x000EDC60
    movie.reset();                                    // 0x000EE220 (the close's black present), delete
    // Mem_CoalesceAll (no game heap in the port)
    Sound().ResumeAll();                              // Sound_ResumeAll
    return false;
}

void App::MoviePump() {                               // orig: vf20 0x000A2530
    if (IsMoviePlaying()) movie->Pump();              // 0x000EDF20
}

void App::FlushRenderer() {                           // orig: 0x0002A660
    // 0x000F2830: DAT_002F0680 deleted (0x000F2150; L: not identified, NOT PORTED)
    ni::NiRenderer* r = services.renderer;            // App->vf14()
    if (!r) return;
    r->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});   // vf6C(0x0030E940: black)
    auto clearAndShow = [r](int n) {
        for (int i = 0; i < n; ++i) {
            r->ClearBuffer(nullptr, 1);               // vf44(0, 1)
            r->DisplayFrame();                        // vf40
        }
    };
    clearAndShow(2);
    // 0x0013D020: the two static arrays 0x00304608..0x0030E608 / 0x002FA520..0x00304520 of references released and
    // DAT_002FA4E0 = 0 (L: the renderers' state caches; NOT PORTED: the port's renderer has no such arrays)
    clearAndShow(3);
    // vf11C 0x000B0420: nothing
    clearAndShow(3);
    // renderer+8 (the sorter) released: the port's renderer keeps its own (render)
    // (port, UB-avoiding) the property / effect state of the last object displayed (renderer +0xC / +0x10: the
    // deleted level's) cleared; the original leaves them dangling (its next screen texture AddRefs the stale +0xC)
    r->propertyState = nullptr;
    r->effectState = nullptr;
}

void App::StepServices() {                            // orig: vf28 0x000D2660
    input.HandleDeviceChanges(services.deviceChanges);   // Input_HandleDeviceChanges 0x0009C1D0
    input.Update(g_frameStep, services.readPad);      // Input_Update 0x0009BEA0
    UpdateSound(Sound(), CurrentLevel());             // the sound system's vt+4 Update 0x000CBD40 (vt+8: a nop)
    memoryCards->Step();                              // 0x000E5380
}

bool App::OnIdle() {                                  // orig: 0x000A2CB0
    if (!current_) return false;
    streams.Step();                                   // 0x000D2400
    {                                                 // SoundRequestLoader::Get() 0x000F20E0 -> 0x000F2710
        const LoadedLevel* l = ingame ? ingame->Level() : nullptr;   // (DAT_002DAC64: the Session)
        StepSoundRequests(l && l->session ? l->level.get() : nullptr, l && l->session ? s32(l->session->state) : 0,
                          services.streamFile);
    }
    const u64 now = qpc;                              // 0x000D2C20
    s64 acc = static_cast<s64>(now - stepPosition_);
    {                                                 // the fps counter (+0xD0 / +0xD8 / +0xE0)
        const double since = Seconds(static_cast<s64>(now - fpsStart_));
        ++fpsCount_;
        if (since > 1.0f) {                           // FCOM [0x00293A98]; TEST AH,0x41; JNE
            fpsStart_ = now;
            fps_ = F(double(fpsCount_) / since);
            fpsCount_ = 0;
        }
    }
    const f32 cap = F(double(g_frameStep) * 6.0f);    // [0x0029C094]
    if (Seconds(acc) > double(cap)) acc = Ticks(cap); // FCOMP; TEST AH,0x41; JNE; 0x000D2BA0
    // (g_fixedFrameStep DAT_002D67EC = 1: the variable-step branch is not taken)
    lastSteps = 0;
    for (int n = 100; !(Seconds(acc) < double(g_frameStep)) && n >= 1; --n) {   // FCOMP dt; TEST AH,1; JNE out
        StepServices();                               // vf28
        (void)cheats.Update(*this);                   // 0x0002C890(DAT_002E7C1C): the pad-combo recorder
        ++lastSteps;
        if (!current_->Update()) {                    // vf08
            current_->Exit();                         // vf10
            FlushRenderer();                          // 0x0002A660
            // 0x001084A0(App) (the stream's objects), the sorter released (render)
            current_ = next_;
            if (!current_) return false;              // (the App quits)
            next_ = nullptr;
            // Mem_CoalesceAll, the new NiAlphaAccumulator sorter (render)
            if (!current_->Enter()) return false;     // vf04
            input.Update(g_frameStep, services.readPad);   // Input_Update 0x0009BEA0
            (void)current_->Update();                 // the new context's first update, in this step
        }
        acc -= Ticks(g_frameStep);                    // 0x000D2BE0
    }
    stepPosition_ = now - static_cast<u64>(acc);      // App+0xC8
    // Mem_FrameTick; the render clock DAT_002E7DF0 (the first call sets it)
    if (!renderClockSet_) {
        renderClockSet_ = true;
        lastRender_ = qpc;
    }
    lastRenderDt = F(Seconds(static_cast<s64>(qpc - lastRender_)));   // DAT_002E7C28
    lastRender_ = qpc;
    current_->Render();                               // vf0C
    // the memory-card manager's draw hook (Mem_ReturnTrue), the profiler 0x0002C9C0 (the profiler's: U)
    if (dialogue) dialogue->Render();                 // 0x000A30F7: DAT_002EBDAC->0x000D76C0
    return true;
}

bool App::Frame() {                                   // orig: vf0C 0x000D26C0
    if (services.renderer) services.renderer->ClearBuffer(nullptr, g_clearFlags);   // vf44(0, DAT_002D67E0)
    MoviePump();                                      // vf20 0x000A2530
    stream::Manager* previous = stream::Manager::Current();
    stream::Manager::Current() = &streams;
    const bool ok = OnIdle();
    stream::Manager::Current() = previous;
    if (!ok) return false;
    if (memoryCards && memoryCards->running) memoryCards->Draw();   // (App+0xF4)+0xA: 0x000E5360
    if (errorOverlay) errorOverlay->Draw();           // 0x000F2D80
    if (services.renderer) services.renderer->DisplayFrame();   // vf40
    return true;
}

}  // namespace fut::game::app
