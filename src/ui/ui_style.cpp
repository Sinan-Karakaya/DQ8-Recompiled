#include "ui/ui_style.h"

#include <cmath>

// Roboto Medium, from Dear ImGui's misc/fonts, embedded at build time.
extern const unsigned char kUiFontData[];
extern const unsigned int kUiFontData_size;

namespace dq8::ui {
namespace {
constexpr float kBaseFontSize = 17.0f;

ImVec4 color(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }
ImVec4 rgba(int r, int g, int b, float a) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
} // namespace

ImFont *loadFonts() {
    ImFontConfig config;
    // The data is static; the atlas must not try to free it.
    config.FontDataOwnedByAtlas = false;
    ImGuiIO &io = ImGui::GetIO();
    return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(kUiFontData),
                                          static_cast<int>(kUiFontData_size), kBaseFontSize, &config);
}

void applyStyle(float scale) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 14.0f;
    style.WindowBorderSize = 2.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 2.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 9.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 7.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.DisabledAlpha = 0.45f;
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = color(palette::kText);
    c[ImGuiCol_TextDisabled] = color(palette::kTextMuted);
    c[ImGuiCol_WindowBg] = color(palette::kWindow);
    c[ImGuiCol_ChildBg] = rgba(0, 0, 0, 0.0f);
    c[ImGuiCol_PopupBg] = color(palette::kWindowSolid);
    c[ImGuiCol_Border] = color(palette::kBorder);
    c[ImGuiCol_BorderShadow] = rgba(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg] = rgba(255, 255, 255, 0.07f);
    c[ImGuiCol_FrameBgHovered] = rgba(255, 255, 255, 0.12f);
    c[ImGuiCol_FrameBgActive] = rgba(255, 255, 255, 0.17f);
    c[ImGuiCol_TitleBg] = color(palette::kWindowSolid);
    c[ImGuiCol_TitleBgActive] = color(palette::kWindowSolid);
    c[ImGuiCol_TitleBgCollapsed] = color(palette::kWindow);
    c[ImGuiCol_MenuBarBg] = rgba(8, 11, 32, 0.95f);
    c[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgba(255, 255, 255, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = rgba(255, 255, 255, 0.28f);
    c[ImGuiCol_ScrollbarGrabActive] = rgba(244, 203, 82, 0.70f);
    c[ImGuiCol_CheckMark] = color(palette::kGold);
    c[ImGuiCol_SliderGrab] = color(palette::kGold);
    c[ImGuiCol_SliderGrabActive] = rgba(255, 224, 130, 1.0f);
    c[ImGuiCol_Button] = rgba(255, 255, 255, 0.08f);
    c[ImGuiCol_ButtonHovered] = rgba(110, 140, 255, 0.30f);
    c[ImGuiCol_ButtonActive] = rgba(110, 140, 255, 0.45f);
    c[ImGuiCol_Header] = rgba(110, 140, 255, 0.22f);
    c[ImGuiCol_HeaderHovered] = rgba(110, 140, 255, 0.30f);
    c[ImGuiCol_HeaderActive] = rgba(110, 140, 255, 0.42f);
    c[ImGuiCol_Separator] = rgba(255, 255, 255, 0.16f);
    c[ImGuiCol_SeparatorHovered] = rgba(244, 203, 82, 0.60f);
    c[ImGuiCol_SeparatorActive] = color(palette::kGold);
    c[ImGuiCol_ResizeGrip] = rgba(255, 255, 255, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = rgba(244, 203, 82, 0.40f);
    c[ImGuiCol_ResizeGripActive] = rgba(244, 203, 82, 0.70f);
    c[ImGuiCol_InputTextCursor] = color(palette::kGold);
    c[ImGuiCol_Tab] = rgba(255, 255, 255, 0.05f);
    c[ImGuiCol_TabHovered] = rgba(110, 140, 255, 0.30f);
    c[ImGuiCol_TabSelected] = rgba(244, 203, 82, 0.20f);
    c[ImGuiCol_TabSelectedOverline] = color(palette::kGold);
    c[ImGuiCol_TabDimmed] = rgba(255, 255, 255, 0.04f);
    c[ImGuiCol_TabDimmedSelected] = rgba(244, 203, 82, 0.14f);
    c[ImGuiCol_PlotLines] = color(palette::kGold);
    c[ImGuiCol_PlotHistogram] = color(palette::kGold);
    c[ImGuiCol_TableHeaderBg] = rgba(255, 255, 255, 0.05f);
    c[ImGuiCol_TableBorderStrong] = rgba(255, 255, 255, 0.12f);
    c[ImGuiCol_TableBorderLight] = rgba(255, 255, 255, 0.07f);
    c[ImGuiCol_TableRowBg] = rgba(0, 0, 0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgba(255, 255, 255, 0.025f);
    c[ImGuiCol_TextLink] = color(palette::kSky);
    c[ImGuiCol_TextSelectedBg] = rgba(244, 203, 82, 0.30f);
    // Softer than the cursor, which already marks the focused entry in menus.
    c[ImGuiCol_NavCursor] = rgba(244, 203, 82, 0.55f);
    c[ImGuiCol_ModalWindowDimBg] = rgba(0, 0, 10, 0.55f);

    style.FontSizeBase = kBaseFontSize;
    style.FontScaleMain = scale;
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

void drawShadow(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    constexpr int kRings = 9;
    for (int ring = 1; ring <= kRings; ++ring) {
        const float spread = static_cast<float>(ring) * 1.6f;
        const float fade = 1.0f - static_cast<float>(ring) / (kRings + 1);
        list->AddRect(ImVec2(min.x - spread, min.y - spread + 2.0f),
                      ImVec2(max.x + spread, max.y + spread + 2.0f),
                      IM_COL32(0, 0, 8, static_cast<int>(70.0f * fade * fade * alpha)),
                      rounding + spread, 1.8f);
    }
}

void drawFrame(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    // A dark hairline outside keeps the white edge visible over bright scenes.
    list->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f),
                  IM_COL32(0, 0, 12, static_cast<int>(150 * alpha)), rounding + 1.0f, 1.0f);
    list->AddRect(min, max, withAlpha(palette::kBorder, alpha), rounding, 2.0f);
    const float inset = 4.0f;
    list->AddRect(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset),
                  withAlpha(palette::kBorderInner, alpha), rounding > inset ? rounding - inset : 0.0f, 1.0f);
}

void decorateWindow(float alpha) {
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImDrawList *list = ImGui::GetWindowDrawList();
    // Past the window's own clip rect: the edge and the shadow sit outside it.
    list->PushClipRect(ImVec2(min.x - 24.0f, min.y - 24.0f), ImVec2(max.x + 24.0f, max.y + 24.0f), false);
    const float rounding = ImGui::GetStyle().WindowRounding;
    drawShadow(list, min, max, rounding, alpha);
    const float inset = 4.0f;
    list->AddRect(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset),
                  withAlpha(palette::kBorderInner, alpha), rounding > inset ? rounding - inset : 0.0f, 1.0f);
    list->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f),
                  IM_COL32(0, 0, 12, static_cast<int>(150 * alpha)), rounding + 1.0f, 1.0f);
    list->PopClipRect();
}

void drawCursor(ImDrawList *list, ImVec2 tip, float size, float alpha) {
    const float bob = std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f) * size * 0.12f;
    tip.x += bob;
    const float half = size * 0.5f;
    const ImVec2 a(tip.x - size * 0.82f, tip.y - half);
    const ImVec2 b(tip.x, tip.y);
    const ImVec2 c(tip.x - size * 0.82f, tip.y + half);
    list->AddTriangleFilled(ImVec2(a.x - 1.0f, a.y - 1.5f), ImVec2(b.x + 1.5f, b.y),
                            ImVec2(c.x - 1.0f, c.y + 1.5f), IM_COL32(0, 0, 12, static_cast<int>(170 * alpha)));
    list->AddTriangleFilled(a, b, c, withAlpha(palette::kText, alpha));
}

} // namespace dq8::ui
