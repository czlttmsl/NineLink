#pragma once

namespace nl::ui {

// Reroutes Core.Game.OnGUI so the mod can draw with Unity's IMGUI every frame.
bool install();

} // namespace nl::ui
