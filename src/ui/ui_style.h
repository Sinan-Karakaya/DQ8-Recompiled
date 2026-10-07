// The menu's look: Dragon Quest VIII's windows are near-black navy, edged in
// white with rounded corners, with a white triangle as the cursor. Gold marks
// what is selected or switched on.
#pragma once

#include <imgui.h>

namespace dq8::ui {

namespace palette {
inline constexpr ImU32 kWindow = IM_COL32(9, 13, 36, 232);
inline constexpr ImU32 kWindowSolid = IM_COL32(12, 17, 46, 248);
inline constexpr ImU32 kBorder = IM_COL32(242, 243, 252, 240);
inline constexpr ImU32 kBorderInner = IM_COL32(255, 255, 255, 46);
inline constexpr ImU32 kText = IM_COL32(246, 247, 255, 255);
inline constexpr ImU32 kTextMuted = IM_COL32(158, 168, 204, 255);
inline constexpr ImU32 kGold = IM_COL32(244, 203, 82, 255);
inline constexpr ImU32 kGoldSoft = IM_COL32(244, 203, 82, 60);
inline constexpr ImU32 kSky = IM_COL32(118, 160, 255, 255);
inline constexpr ImU32 kHover = IM_COL32(110, 140, 255, 52);
inline constexpr ImU32 kFill = IM_COL32(255, 255, 255, 16);
inline constexpr ImU32 kFillStrong = IM_COL32(255, 255, 255, 30);
inline constexpr ImU32 kGood = IM_COL32(112, 222, 142, 255);
inline constexpr ImU32 kWarn = IM_COL32(246, 182, 64, 255);
inline constexpr ImU32 kBad = IM_COL32(238, 96, 96, 255);
} // namespace palette

inline ImU32 withAlpha(ImU32 color, float alpha) {
    const auto a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFFu) * alpha);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// Loads the embedded font into the current context's atlas.
ImFont *loadFonts();
// Rebuilds the style at a scale: the display's own times the player's choice.
void applyStyle(float scale);

// The white double edge of a Dragon Quest window, drawn over a window's own
// background. Call right after Begin().
void decorateWindow(float alpha = 1.0f);
void drawFrame(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding, float alpha = 1.0f);
// A soft shadow under a floating window, so it reads over a busy picture.
void drawShadow(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding, float alpha = 1.0f);
// The cursor: a white triangle pointing right with its tip at `tip`, nudged
// back and forth the way the game's own cursor bobs.
void drawCursor(ImDrawList *list, ImVec2 tip, float size, float alpha = 1.0f);

// Heights scale with the font: one "unit" is the current font size.
inline float em(float units = 1.0f) { return ImGui::GetFontSize() * units; }

} // namespace dq8::ui
