#pragma once
// Immediate-mode widgets drawn with Unity IMGUI's DrawTexture + GUI.color (the
// game build strips IMGUI's widgets and skin) and a mouse read from Windows.
#include "text.h"
#include "unity.h"

#include <string>

namespace nl::gui {

using unity::Rect;
struct Color {
    float r, g, b, a;
};

// palette
constexpr Color kPanel{0.10f, 0.08f, 0.07f, 0.94f};
constexpr Color kPanelEdge{0.85f, 0.62f, 0.25f, 1.0f};
constexpr Color kRow{0.18f, 0.15f, 0.12f, 0.95f};
constexpr Color kButton{0.30f, 0.22f, 0.14f, 1.0f};
constexpr Color kButtonHot{0.55f, 0.40f, 0.18f, 1.0f};
constexpr Color kButtonOn{0.80f, 0.58f, 0.22f, 1.0f};
constexpr Color kButtonOff{0.20f, 0.18f, 0.17f, 0.9f};
constexpr Color kText{0.95f, 0.91f, 0.84f, 1.0f};
constexpr Color kTextDim{0.65f, 0.60f, 0.54f, 1.0f};
constexpr Color kGold{0.98f, 0.78f, 0.35f, 1.0f};
constexpr Color kGreen{0.55f, 0.85f, 0.45f, 1.0f};
constexpr Color kWhite{1, 1, 1, 1};

enum Align { Left, Center, Right };

bool init();          // resolve Unity methods (init thread)
void begin_frame();   // once per OnGUI call: screen size, mouse, click edges
void end_frame();

extern float U;       // UI scale relative to a 1080p screen
extern float W, H;    // screen size in pixels

void SetColor(Color c);
void Fill(Rect r, Color c);
void Frame(Rect r, Color fill, Color edge, float thickness);
void Image(Rect r, void* texture, Color tint = kWhite);
void Text(float x, float y, float lineH, const std::string& s, float size, Color c, Align a = Left, bool bold = false);
// Word-wrapped text inside a box (simple CJK-friendly wrap by character).
void TextBox(Rect r, const std::string& s, float size, Color c, Align a = Center);
bool Button(Rect r, const std::string& label, bool enabled = true, bool on = false, float size = 22);

// mouse
bool Inside(Rect r);
float MouseX();
float MouseY();
bool MouseDown();         // left button held
bool Pressed();           // left button went down this frame (consumed by TakePress)
bool TakePress(Rect r);   // a press inside r: consumes it
bool Released();          // left button went up this frame
bool TakeClick(Rect r);   // a release inside r: consumes it
bool RightClick(Rect r);  // right button released inside r

} // namespace nl::gui
