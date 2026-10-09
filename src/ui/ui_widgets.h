// The menu's controls, in the game's style. All work with mouse, keyboard
// and gamepad navigation, since each is built on an ImGui item.
#pragma once

#include "ui/ui_glyphs.h"

#include <imgui.h>

namespace dq8::ui {

// Begin() plus the Dragon Quest window edge and shadow.
bool beginWindow(const char *name, bool *open, ImGuiWindowFlags flags);

// An on/off switch.
bool toggle(const char *id, bool *value);
// A row of mutually exclusive choices, filling the width it is given.
// `enabled` may be null; a disabled choice is shown but cannot be picked.
bool segmented(const char *id, int *value, const char *const *labels, int count,
               const bool *enabled = nullptr);

void sectionHeader(const char *title);
// A two-column table of settings: the label (with an optional chip such as
// "Experimental" beside it, and help underneath) on the left, the control on
// the right.
bool beginSettings(const char *id);
void settingRow(const char *label, const char *help = nullptr, const char *chipText = nullptr);
void endSettings();
// A small rounded label after the item before it, or on the next line when
// it does not fit beside it. Call after SameLine().
void chip(const char *text, ImU32 color);

// The settings window's page list: the selected entry carries the cursor.
bool navItem(const char *label, Icon icon, bool selected);
// A menu entry that shows the triangle cursor when hovered or focused.
bool menuItem(const char *label, const char *shortcut = nullptr, bool selected = false, bool enabled = true);
// The same for an entry that opens a submenu; EndMenu() when it returns true.
bool beginSubmenu(const char *label);
// A button with an icon before its label.
bool iconButton(const char *label, Icon icon, bool primary = false);

// A small keycap, inline, for shortcut hints.
void keycap(const char *text);
void mutedText(const char *format, ...) IM_FMTARGS(1);

} // namespace dq8::ui
