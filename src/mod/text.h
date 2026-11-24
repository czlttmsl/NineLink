#pragma once
// Text for the overlay. The game build strips IMGUI's default font, so text
// is rasterised with GDI (Microsoft YaHei, handles Chinese) into a Texture2D
// and drawn with GUI.DrawTexture. Results are cached per (text, size, bold).
#include <string>

namespace nl::text {

struct Tex {
    void* texture = nullptr;  // UnityEngine.Texture2D
    int w = 0, h = 0;
};

bool init();  // resolve Texture2D methods; call once from the init thread
const Tex& get(const std::string& utf8, int pixelHeight, bool bold = false);  // main thread only

} // namespace nl::text
