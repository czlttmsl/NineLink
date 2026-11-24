#pragma once
// The perk page, laid out like the official multiplayer one (MultiplayerHubPerksUI):
// title + reset on top, back / done at the corners, the 3x3 perk grid in the middle,
// save slots 1-3 on the right, the hovered perk's name and description under the grid,
// and the hand of perks (with category paging) along the bottom. Drag perks from the
// hand onto the grid; drag them off (or right-click) to take them back.

namespace nl::perkpage {

void open(int lobbyKing);
bool is_open();
void draw();  // full-screen; call from OnGUI while is_open()

} // namespace nl::perkpage
