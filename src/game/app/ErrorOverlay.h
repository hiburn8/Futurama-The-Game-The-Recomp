// The App's error overlay (orig: App::Init 0x000D2730 -> 0x000F3470; the globals DAT_002F0940 camera, DAT_002F094C
// text object, DAT_002F0950 the message, DAT_002F0954 the error database, DAT_002F0A5C / DAT_002F0ADC the shared
// alpha / material of the dark backdrop; the draw 0x000F2D80 from App::Frame, the message setters 0x000F2C80 /
// 0x000F2CF0, the read-error loop 0x000F3060). docs/analysis/frontend_port.md 1.2.
//
// Ported: the build (the camera, the text object with font 0 (the second texture of the run, after the clipper),
// the backdrop screen polygon with its vertex-colour / alpha / material properties, HUD\ErrorIcon.nif's icon polygon
// 0x000F3140, Language/XBOXERROR.ldb) so the renderer's creation order from App::Init is the original's; the draw's
// guard, the message setters (the pads' disconnection: ERRCTRLFRONTEND 0x000F2C80 from the front end's Render,
// ERRCTRLINGAME 0x000F2CF0 from the Ingame context's) and the draw with a message (0x000F2D80: the camera's Click,
// the message centred, the text's Click; the message is freed after the draw).
// NOT PORTED (reported when reached): the debug byte's level / position / fps lines (DAT_002F0944: no writer),
// NiUDSStream's read-error handler 0x002D711C and its loop 0x000F3060.
#pragma once

#include "engine/ni/NiObjects.h"

#include <memory>
#include <string>

namespace fut::text {
class TextObject;
class LanguageDb;
}  // namespace fut::text

namespace fut::game::app {

class App;

struct ErrorOverlay {
    explicit ErrorOverlay(App& app);                  // orig: 0x000F3470
    ~ErrorOverlay();
    void Draw();                                      // orig: 0x000F2D80
    // orig: 0x000F2C80 (id): the pending message replaced by a copy of the error database's text (none when the
    // database has no such text). Traced as ERRMSG <frame> <id>.
    void SetMessage(const char* id);
    // orig: 0x000F2CF0 (id, n): as 0x000F2C80, then the copy's first '#' (0x000D2D70) becomes the digit n ('0' + n).
    // Traced as ERRMSG <frame> <id> <n>.
    void SetMessage(const char* id, u32 n);
    // orig: 0x000F3140 (texture, w, h, x, y, rgb, alpha, out texturing, out material, uvs, camera): a textured
    // screen polygon in 640 x 480 pixels on the camera (the outputs and uvs are null at its only call).
    static ni::NiPointer<ni::NiScreenPolygon> MakeIcon(ni::NiObject* texture, f32 w, f32 h, f32 x, f32 y,
                                                       const ni::NiColor& rgb, bool alpha, ni::NiCamera* camera);

    ni::NiPointer<ni::NiCamera> camera;               // orig: DAT_002F0940 (new(0x250) 0x001009D0)
    std::unique_ptr<text::TextObject> text;           // orig: DAT_002F094C (TextObject::Create(0))
    ni::NiPointer<ni::NiScreenPolygon> backdrop;      // (the camera's first polygon: new(0x24) 0x00106DE0)
    ni::NiPointer<ni::NiScreenPolygon> icon;          // (0x000F3140's polygon; the original keeps it on the camera only)
    ni::NiPointer<ni::NiAlphaProperty> alpha;         // orig: DAT_002F0A5C
    ni::NiPointer<ni::NiMaterialProperty> material;   // orig: DAT_002F0ADC
    std::unique_ptr<std::u16string> message;          // orig: DAT_002F0950 (the pending message's copy, or none)
    bool debugText = false;                           // orig: DAT_002F0944 (0 in the image, no writer found: L)
    text::LanguageDb* errors = nullptr;               // orig: DAT_002F0954 (Language/XBOXERROR.ldb)

private:
    App& app_;
};

}  // namespace fut::game::app
