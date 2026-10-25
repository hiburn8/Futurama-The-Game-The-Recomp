// The front end's extras (docs/analysis/frontend_panels_port.md 5): menu/extras/unlock.txt's sections and items
// (orig: the parse 0x0001C900 with its release 0x0001C3B0, the section 0x0001BA90 / 0x0001C270, AddItem 0x0001C580,
// the item 0x0001C4A0 / 0x0001C060 / 0x0001B9C0), the thumbnails of menu\extras\thumbs.nif (0x0001CF00, the lookup
// 0x0001BA30), and the two extras menus' scenes: the menu base (vtable 0x00293DD8: ctor 0x0001D020(stream), dtor
// 0x0001C740), the thumbnail grid DAT_002D9A70 (vtable 0x00293F44: 0x0001E130 / 0x0001D740, Extra_A.nif) and the
// music list DAT_002D9B98 (ExtraMenuB, vtable 0x00294028: 0x0001F360 / 0x0001EAF0, Extra_B.nif). The Frontend's load
// makes the menus (its stream states 4 / 5); the extras panel's destructor (0x00020A70) deletes them, so their scenes
// and lights live until Frontend::Exit (the effect ids' free-list order).
//
// The menus' Open (0x0001C890, the items 0x0001E960 / 0x0001FAD0), Update (the base 0x0001BB30, the grid 0x0001D880,
// the list 0x0001EC30), Close (0x0001C310), Hide (0x0001B670, 0x0001DF90 / 0x0001F2C0) and Draw (0x0001BBB0, the grid
// 0x0001D530, the list's render target 0x0001F1A0). NOT PORTED (reported when reached): the picture splash's text
// 0x0001BCF0 in the grid's draw.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiStream.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fut::text {
class TextObject;
class LanguageDb;
}  // namespace fut::text

namespace fut::game::app {

class App;
struct ExtrasSection;
struct Splash;

// orig: an unlock.txt item (new(0x18) 0x0001C4A0(section, key, value)).
struct ExtrasItem {
    std::string key;                                  // orig: +0x00 ("EXTRA_FMV_001")
    std::string file;                                 // orig: +0x04 (the value: "Credits.avi")
    std::string base;                                 // orig: +0x08 (the value before its '.': 0x0001C060)
    s32 kind = 0;                                     // orig: +0x0C (1 avi, 2 wav, 3 tga, 0 else)
    ni::NiPointer<ni::NiSourceTexture> icon;          // orig: +0x10
    ExtrasSection* section = nullptr;                 // orig: +0x14
    bool Locked() const;                              // orig: 0x0001B620
};

// orig: an unlock.txt "[name]" section (new(0x24) 0x0001BA90(name), vtable 0x00293D80).
struct ExtrasSection {
    bool locked = false;                              // orig: +0x04
    std::string name;                                 // orig: +0x08
    std::vector<std::unique_ptr<ExtrasItem>> items;   // orig: +0x0C (data +0x10, count +0x18)
    // orig: 0x0001C580 (key, value): the value's "|Language" / "|Europe" / "|America" filters, then the item.
    void AddItem(App& app, const char* key, const char* value);
    void Refresh(const App& app);                     // orig: 0x0001B630
};

// The process-wide extras data (one App per process: the App owns it).
struct ExtrasData {
    std::vector<ni::NiPointer<ni::NiSourceTexture>> thumbs;   // orig: DAT_002D9A50 (data +4, count +0xC)
    std::vector<std::unique_ptr<ExtrasSection>> sections;     // orig: DAT_002D5C40 (data 0x2D5C44, count 0x2D5C4C)
    ni::NiPointer<ni::NiSourceTexture> aviIcon;       // orig: DAT_002D99F8 (the current "@AVI_ICON")
    ni::NiPointer<ni::NiSourceTexture> wavIcon;       // orig: DAT_002D994C (the current "@WAV_ICON")
    s32 users = 0, users2 = 0;                        // orig: DAT_002D9928 / DAT_002D9924 (0x0001C900 / 0x0001C3B0)
    u8 allUnlocked = 0;                               // orig: DAT_002D9920 (the cheat codes 26 / 27, CheatCodes.h)
    // orig: 0x0001BA30 (name): the first thumbnail whose name equals `name` (_stricmp), else null.
    ni::NiSourceTexture* FindThumb(const char* name) const;
};

// orig: 0x0001CF00 (stream): every named NiSourceTexture among menu\extras\thumbs.nif's top-level objects appended
// to the thumbnails.
void LoadExtrasThumbs(App& app, ni::NiStream& s);
// orig: 0x0001C900: the first user parses menu/extras/unlock.txt (the sections, their items, the icons); every user
// then loads Language/EXTRAS.ldb.
void AcquireExtras(App& app);
// orig: 0x0001C3B0: the last user releases the thumbnails and the sections; the icons are released every time.
void ReleaseExtras(App& app);

// orig: the extras menu base (vtable 0x00293DD8, ctor 0x0001D020(stream), dtor 0x0001C740).
class ExtrasMenu {
public:
    ExtrasMenu(App& app, ni::NiStream& s);
    virtual ~ExtrasMenu();
    virtual void Update() = 0;                        // vf04
    virtual void Draw() { BaseDraw(); }               // vf08 (0x0001BBB0)
    virtual void Open(s32 m) { BaseOpen(m); }         // vf0C (0x0001C890)
    virtual void Hide() { shown = false; }            // vf10 (0x0001B670)
    bool Shown() const { return shown; }              // vf14 (0x0001B680)
    App& app;
    s32 mode = 0;                                     // orig: +0x04 (1 movies, 2 music, 3 art)
    bool closeRequested = false;                      // orig: +0x08
    bool shown = false;                               // orig: +0x09
    s32 cursor = 0;                                   // orig: +0x0C
    s32 prev = -1;                                    // orig: +0x10
    ni::NiSequence* sequence = nullptr;               // orig: +0x14 (the running enter / leave sequence)
    ni::NiPointer<ni::NiKeyframeManager> manager;     // orig: +0x18 (on the scene root)
    ni::NiPointer<ni::NiNode> cameraNode;             // orig: +0x1C ("CAMERA")
    ni::NiPointer<ni::NiNode> root;                   // orig: +0x20 (the file's first NiNode)
    ni::NiPointer<ni::NiObject> field24;              // orig: +0x24 (released by the dtor; no writer found)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x28 ("CAMERA1"; +0x15C = the root, +0x1A8 the renderer)
    u32 scrollSound = 0, selectSound = 0, scrollLimitSound = 0;   // orig: +0x2C / +0x30 / +0x34
    u32 foldUpSound = 0, foldDownSound = 0;           // orig: +0x38 / +0x3C
    f32 sequenceTime = 0;                             // orig: +0x40
    f32 cooldown = 0;                                 // orig: +0x44
    f32 repeatTimer = 0;                              // orig: +0x48
    s32 repeatDir = 0;                                // orig: +0x4C
    std::unique_ptr<text::TextObject> title;          // orig: +0x50 (scene mode)
    std::unique_ptr<text::TextObject> text;           // orig: +0x54
    text::LanguageDb* hints = nullptr;                // orig: +0x58 (Language/HINTS_XBOX.ldb)

protected:
    // orig: 0x0001BB30: the running enter / leave sequence ended ((hi - lo) / frequency - its offset not above the
    // menu's time): deactivated; with the close requested: the title's strings removed and vf10; else time += dt.
    void BaseUpdate();
    void BaseOpen(s32 mode);                          // orig: 0x0001C890
    void BaseDraw();                                  // orig: 0x0001BBB0
    // orig: 0x0001BCF0 (id): the hint text TextManager::Get(id, +0x58)'s string drawn by the text object +0x54 at
    // (0.5, 0.12) (RemoveAll, UpdateCamera, AddString(alpha -1), UpdateCamera, Click).
    void DrawHint(const char* id);
    void Close(const char* sequence);                 // orig: 0x0001C310 (the sequence from -time, activated)
    void StartSequence(const char* sequence);         // orig: 0x0001C350 (likewise, not kept in +0x14)
    void LoopSequence(const char* sequence, bool loop);   // orig: 0x0001C380 (0x0001BC10: the cycle type)
    // orig: the cells' texturing property: every geometry `object`'s texturing property (GetProperty(8) as an
    // NiTexturingProperty) removed from its list (0x0001E020) and `property` put at the head (AddHead 0x00013A50).
    static void ReplaceTexturing(ni::NiAVObject* object, ni::NiTexturingProperty* property);
    // orig: new(0x3C) 0x00109DC0, +0x18 = 2, SetBaseFilterMode(2) 0x0001D6B0.
    static ni::NiTexturingProperty* NewCellTexturing();
};

// orig: the thumbnail grid (new(0x110) 0x0001E130(stream), vtable 0x00293F44, dtor 0x0001D740): DAT_002D9A70.
class ThumbGrid : public ExtrasMenu {
public:
    ThumbGrid(App& app, ni::NiStream& s);
    ~ThumbGrid() override;
    void Update() override;                           // orig: 0x0001D880
    void Draw() override;                             // orig: 0x0001D530
    void Open(s32 mode) override;                     // orig: 0x0001E960
    void Hide() override;                             // orig: 0x0001DF90
    std::array<ni::NiPointer<ni::NiSwitchNode>, 15> frames;   // orig: +0x5C ("frame_<c>_<r>", else "frame<c>_<r>")
    std::array<ni::NiPointer<ni::NiTexturingProperty>, 15> cells;   // orig: +0x98
    ni::NiPointer<ni::NiSourceTexture> defaultTexture;   // orig: +0xD4 (menu/extras/extra_default.nif)
    ni::NiPointer<ni::NiSwitchNode> arrowUp, arrowDown;  // orig: +0xD8 / +0xDC
    ni::NiPointer<ni::NiNode> headline, textNode;     // orig: +0xE0 ("text_headline_movie") / +0xE4 ("text")
    u16 headlineId = 0;                               // orig: +0xE8 (the cursor item's name string of +0x50)
    std::unique_ptr<Splash> splash;                   // orig: +0xEC (a picture: new(0x3C) 0x000EFBC0)
    s32 top = 0;                                      // orig: +0xF0
    bool playing = false;                             // orig: +0xF4
    std::vector<ExtrasItem*> items;                   // orig: +0xF8 (data +0xFC, count +0x104, non-null +0x108)
};

// orig: the music list (ExtraMenuB, new(0xB8) 0x0001F360(stream), vtable 0x00294028, dtor 0x0001EAF0): DAT_002D9B98.
class MusicList : public ExtrasMenu {
public:
    MusicList(App& app, ni::NiStream& s);
    ~MusicList() override;
    void Update() override;                           // orig: 0x0001EC30
    void Draw() override;                             // orig: 0x0001F1A0 (the front end into the render target)
    void Open(s32 mode) override;                     // orig: 0x0001FAD0
    void Hide() override;                             // orig: 0x0001F2C0
    std::array<ni::NiPointer<ni::NiNode>, 4> texts;   // orig: +0x5C ("text_<i>", else "text<i>")
    std::array<ni::NiPointer<ni::NiSwitchNode>, 4> boxes;   // orig: +0x6C ("box_<i>", else "box<i>")
    std::vector<ExtrasItem*> items;                   // orig: +0x7C (data +0x80, count +0x88)
    ni::NiPointer<ni::NiSwitchNode> playStop, upArrow, downArrow;   // orig: +0x94 / +0x98 / +0x9C
    ni::NiPointer<ni::NiNode> headline;               // orig: +0xA0 ("text_headline_audio")
    ni::NiPointer<ni::NiTexturingProperty> thumbnail; // orig: +0xA4 (the "thumbnail" geometry's)
    ni::NiPointer<ni::NiRenderedTexture> target;      // orig: +0xA8 (NiRenderedTexture::Create(256, 256))
    s32 top = 0;                                      // orig: +0xAC
    u32 sample = 0;                                   // orig: +0xB0
    s32 voice = 0;                                    // orig: +0xB4
};

}  // namespace fut::game::app
