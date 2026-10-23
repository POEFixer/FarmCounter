// PluginUi.h — the plugin UI kit: the look of the host's own settings tabs
// (Health Bars, Atlas) for plugin settings, plus a few in-game overlay helpers.
//
// Header-only. Depends only on Dear ImGui and the C/C++ standard library. All text
// is passed in by the caller (plugins are not localized). Colours are taken from
// the LIVE ImGui style, which the host fills from the user's theme, so a plugin
// follows every theme without naming one. Set ImGui's context from
// ctx()->ImGuiContext at every entry point that draws (OnEnable, DrawUI,
// DrawSettings): with "Separate menu window" DrawUI and DrawSettings run in two
// different contexts. The host (366+) also binds it before each call.
//
// ---------------------------------------------------------------------------
// How a DrawSettings() should be laid out
// ---------------------------------------------------------------------------
//   * One card per section (BeginCard / EndCard): the card pads its content, so
//     nothing touches the tab's edges. Leave ImGui::Spacing() between cards.
//   * Each card starts with Heading(icon, "Title", "longer help"): the help
//     becomes a "?" right after the title. Beside a control, a HelpMarker
//     carries it.
//   * Diagnostics (dry runs, scan state, what the plugin sees) go last, in
//     BeginDiagnostics(tip) ... EndDiagnostics(): one closed card per plugin,
//     the same everywhere.
//   * Label-left rows share ONE measured column per block: build a LabelColumn
//     with every label of the block, then call Row("Label") before each control.
//     Never hard-code SameLine(120) or PushItemWidth(80).
//   * Every non-obvious control gets a Tip(...) right after it (multi-line,
//     wrapped). Put details behind "?", not in the label.
//   * Dependent controls are dimmed (ImGui::BeginDisabled), never hidden.
//   * Checkbox ids do not change with their text: Check("Label", "id", ...).
//   * Two columns only when WideEnoughForTwoColumns(); otherwise stack. Test at
//     narrow widths and at font scales 0.8-1.5 (tools/plugin_ui_preview renders
//     your settings to PNG at several widths and scales without the game).
//   * No ImGui::BeginPopupModal (the overlay cannot hit-test a modal's
//     backdrop): use ConfirmPopup.
//
//     void DrawSettings() override {
//         using namespace PluginUi;
//         BeginCard("##display");
//         Heading(ICON_FA_EYE, "Display");                 // icon may be nullptr
//         Check("Show window", "showWindow", &m_show, "Opens the plugin window.\nIt remembers its position.");
//         const LabelColumn col({ "Opacity", "Hotkey" });
//         col.Row("Opacity");
//         ImGui::SliderFloat("##opacity", &m_alpha, 0.3f, 1.0f, "%.1f");
//         Tip("How see-through the window is.");
//         col.Row("Hotkey");
//         HotkeyCapture("##hotkey", &m_hotkeyVk, "Click, then press a key or mouse button.");
//         EndCard();
//     }
//
// Every helper is allocation-free per frame except InputString (it grows the
// string it edits) and LabelColumn's vector constructor.
#pragma once

// (std::min)/(std::max) are parenthesized: Windows.h may define min/max macros.
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"   // CurrentItemFlags: a swatch dims inside BeginDisabled

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace PluginUi {

// ---------------------------------------------------------------------------
// Colours, from the live style (the host writes the theme into it)
// ---------------------------------------------------------------------------
struct Palette {
    ImVec4 Text;            // primary text
    ImVec4 TextSecondary;   // explanations, captions
    ImVec4 TextDisabled;
    ImVec4 Accent;          // headings
    ImVec4 Surface;         // card background
    ImVec4 SurfaceVariant;  // quiet button / field background
    ImVec4 BorderSubtle;    // card border
    ImVec4 Hover;
    ImVec4 Pressed;
    ImVec4 Success;
    ImVec4 Warning;
    ImVec4 Error;
};

namespace detail {
inline ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
inline ImVec4 WithAlpha(ImVec4 c, float a) { c.w = a; return c; }
inline float Luminance(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
} // namespace detail

// The host maps its theme tokens onto these style slots (gui/theme/ThemeManager):
// Surface -> ChildBg, SurfaceVariant -> FrameBg, Hover / Pressed ->
// FrameBgHovered / Active, BorderSubtle -> TableBorderLight, Accent ->
// DragDropTarget, Success -> CheckMark. The secondary text sits between text and
// disabled text, as the themes define it. Warning and Error are fixed semantic
// colours readable on every host theme.
inline Palette Colors() {
    const ImGuiStyle& s = ImGui::GetStyle();
    Palette p;
    p.Text = s.Colors[ImGuiCol_Text];
    p.TextDisabled = s.Colors[ImGuiCol_TextDisabled];
    p.TextSecondary = detail::Lerp(p.Text, p.TextDisabled, 0.45f);
    p.Accent = detail::WithAlpha(s.Colors[ImGuiCol_DragDropTarget], 1.0f);
    p.Surface = s.Colors[ImGuiCol_ChildBg];
    if (p.Surface.w <= 0.0f)   // plain ImGui (no host theme): a step above the window
        p.Surface = detail::Lerp(s.Colors[ImGuiCol_WindowBg], s.Colors[ImGuiCol_FrameBg], 0.35f);
    p.SurfaceVariant = s.Colors[ImGuiCol_FrameBg];
    p.BorderSubtle = s.Colors[ImGuiCol_TableBorderLight];
    p.Hover = s.Colors[ImGuiCol_FrameBgHovered];
    p.Pressed = s.Colors[ImGuiCol_FrameBgActive];
    p.Success = detail::WithAlpha(s.Colors[ImGuiCol_CheckMark], 1.0f);
    // Light themes (bright window background) get darker variants: the bright amber
    // and red are too faint as text on white.
    const bool light = detail::Luminance(s.Colors[ImGuiCol_WindowBg]) > 0.5f;
    p.Warning = light ? ImVec4(0.66f, 0.42f, 0.00f, 1.0f) : ImVec4(0.93f, 0.68f, 0.16f, 1.0f);
    p.Error = light ? ImVec4(0.76f, 0.16f, 0.16f, 1.0f) : ImVec4(0.92f, 0.33f, 0.33f, 1.0f);
    return p;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// Wrapped, multi-line tooltip for the last item (also when it is disabled, so a
// dimmed control still explains itself). Use '\n' for explicit line breaks.
inline void Tip(const char* text) {
    if (!text || !text[0]) return;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// A "?" beside the previous item that shows the longer explanation. For a
// heading pass the text as Heading's `help` instead: a HelpMarker after a
// heading lands on its separator line.
inline void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("?");
    Tip(text);
}

// Heading of one block: an optional icon and a title over a separator, in the
// accent colour. `help` puts a "?" right after the title, in a gap the separator
// leaves for it, with the longer explanation as its tooltip.
inline void Heading(const char* icon, const char* title, const char* help = nullptr) {
    char buf[256];
    if (icon && icon[0]) std::snprintf(buf, sizeof(buf), "%s  %s", icon, title ? title : "");
    else std::snprintf(buf, sizeof(buf), "%s", title ? title : "");
    const bool hasHelp = help && help[0];
    const float gap = hasHelp ? ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("?").x : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, Colors().Accent);
    ImGui::SeparatorTextEx(0, buf, ImGui::FindRenderedTextEnd(buf), gap);
    ImGui::PopStyleColor();
    if (!hasHelp) return;
    ImGui::SameLine();   // SeparatorTextEx leaves the cursor at the end of the title
    ImGui::TextDisabled("?");
    Tip(help);
}

// The plugin's Diagnostics block: a card of its own whose single row (bug icon,
// "Diagnostics", accent colour, hover highlight) opens it; `tip` explains what is
// inside. Returns true while open: draw the content, then EndDiagnostics().
// Returns false with the card already closed (no EndDiagnostics then).
inline bool BeginDiagnostics(const char* tip = nullptr, const char* id = "diag");
inline void EndDiagnostics();

// A secondary line of explanation: wrapped to the region, in the secondary colour.
inline void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, Colors().TextSecondary);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text ? text : "");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Checkbox whose ImGui id ("###id") does not change with its text, with a tip.
inline bool Check(const char* label, const char* id, bool* v, const char* tip = nullptr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s###%s", label ? label : "", id ? id : "");
    const bool changed = ImGui::Checkbox(buf, v);
    Tip(tip);
    return changed;
}

// One block of label-left rows sharing ONE measured column: the widest label of
// the block plus two item spacings, so a font scale or a long label never pushes
// a control under its label. Build it where the block starts (it records the
// block's left edge), every frame.
class LabelColumn {
public:
    LabelColumn(std::initializer_list<const char*> labels) { Measure(labels.begin(), labels.end()); }
    explicit LabelColumn(const std::vector<const char*>& labels) { Measure(labels.data(), labels.data() + labels.size()); }

    // Draws the label and leaves the cursor at the column, with the next item's
    // width set to the rest of the row (capped at 15 em so wide panes keep
    // sliders readable, at least 4 em).
    void Row(const char* label) const {
        RowNoWidth(label);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float em = ImGui::GetFontSize();
        ImGui::SetNextItemWidth((std::max)(em * 4.0f, (std::min)(avail, em * 15.0f)));
    }
    // Same, for a widget that sets no item width (a swatch, a pill, a button).
    void RowNoWidth(const char* label) const {
        ImGui::SetCursorPosX(m_x0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label ? label : "");
        ImGui::SameLine();
        // A plain cursor position: SameLine(x) would add a table column's offset twice.
        ImGui::SetCursorPosX(m_colX);
    }
    // Width of the label column (left edge to the controls).
    float Width() const { return m_colX - m_x0; }

private:
    void Measure(const char* const* first, const char* const* last) {
        m_x0 = ImGui::GetCursorPosX();
        float w = 0.0f;
        for (auto it = first; it != last; ++it)
            if (*it) w = (std::max)(w, ImGui::CalcTextSize(*it).x);
        m_colX = m_x0 + w + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    }
    float m_x0 = 0.0f;
    float m_colX = 0.0f;
};

// Slider rows on a LabelColumn: label, slider, tip. Return true on change.
inline bool SliderIntRow(const LabelColumn& col, const char* label, const char* id, int* v, int min, int max,
                         const char* format = "%d", const char* tip = nullptr) {
    col.Row(label);
    const bool changed = ImGui::SliderInt(id, v, min, max, format, ImGuiSliderFlags_AlwaysClamp);
    Tip(tip);
    return changed;
}
inline bool SliderFloatRow(const LabelColumn& col, const char* label, const char* id, float* v, float min, float max,
                           const char* format = "%.2f", const char* tip = nullptr) {
    col.Row(label);
    const bool changed = ImGui::SliderFloat(id, v, min, max, format, ImGuiSliderFlags_AlwaysClamp);
    Tip(tip);
    return changed;
}

// Width a button with this label takes ("##"/"###" suffix excluded).
inline float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// A quiet button for secondary actions (reset, clear): surface-coloured instead
// of the theme's filled primary, so it does not compete with the content.
inline bool SubtleButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
    const Palette p = Colors();
    ImGui::PushStyleColor(ImGuiCol_Button, p.SurfaceVariant);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.Hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.Pressed);
    ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Puts the cursor so that an item `width` wide ends at the right edge of the
// region (for a footer button or a right-aligned value).
inline void AlignRight(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

// One section: a bordered surface card with the window padding inside, so no
// text, field or button touches an edge. Height follows the content. Always pair
// with EndCard (also on an early return).
inline void BeginCard(const char* id) {
    const Palette p = Colors();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.WindowPadding);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.Surface);
    ImGui::PushStyleColor(ImGuiCol_Border, p.BorderSubtle);
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
inline void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

inline bool BeginDiagnostics(const char* tip, const char* id) {
    char buf[96];
    // "\xef\x86\x88" is ICON_FA_BUG (U+F188) of the host's Font Awesome 6 font;
    // spelled out so the kit needs no icon header.
    std::snprintf(buf, sizeof(buf), "\xef\x86\x88  Diagnostics###%s", id && id[0] ? id : "diag");
    char cardId[64];
    std::snprintf(cardId, sizeof(cardId), "##%sCard", id && id[0] ? id : "diag");
    BeginCard(cardId);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors().Accent);
    const bool open = ImGui::TreeNodeEx(buf, ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen);
    ImGui::PopStyleColor();
    Tip(tip);
    if (!open) EndCard();
    return open;
}
inline void EndDiagnostics() { EndCard(); }

// Two columns only when each still has room for a label and a usable control.
inline bool WideEnoughForTwoColumns() {
    return ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 46.0f;
}

namespace detail {
// True inside ImGui::BeginDisabled(true).
inline bool ItemDisabled() {
    return ImGui::GetCurrentContext() && (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}
// Dear ImGui 1.93 draws colour swatches at full strength inside BeginDisabled; this
// lays the card surface over the last item so it looks dimmed like its neighbours.
inline void DimLastItem() {
    const ImGuiStyle& s = ImGui::GetStyle();
    ImVec4 bg = s.Colors[ImGuiCol_ChildBg];
    if (bg.w <= 0.0f) bg = s.Colors[ImGuiCol_WindowBg];
    bg.w = 1.0f - s.DisabledAlpha;
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::ColorConvertFloat4ToU32(bg), s.FrameRounding);
}
} // namespace detail

// Colour swatch: click to edit (picker with alpha), no inputs, no label. Dimmed
// inside BeginDisabled like every other control.
inline bool Swatch(const char* id, float color[4]) {
    const bool disabled = detail::ItemDisabled();
    const bool changed = ImGui::ColorEdit4(id, color,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    if (disabled) detail::DimLastItem();
    return changed;
}
inline bool Swatch(const char* id, ImVec4& color) { return Swatch(id, &color.x); }
inline bool Swatch(const char* id, ImU32& color) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    if (!Swatch(id, c)) return false;
    color = ImGui::ColorConvertFloat4ToU32(c);
    return true;
}

// Combo over string items; the items keep stable ids ("###item<i>") so their
// text may change. value is clamped into range for display.
inline bool Combo(const char* id, int& value, const char* const* items, int count) {
    if (!items || count <= 0) return false;
    const int current = std::clamp(value, 0, count - 1);
    bool changed = false;
    if (ImGui::BeginCombo(id, items[current] ? items[current] : "")) {
        char buf[256];
        for (int i = 0; i < count; ++i) {
            std::snprintf(buf, sizeof(buf), "%s###item%d", items[i] ? items[i] : "", i);
            if (ImGui::Selectable(buf, i == current)) {
                value = i;
                changed = true;
            }
            if (i == current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
inline bool Combo(const char* id, int& value, std::initializer_list<const char*> items) {
    return Combo(id, value, items.begin(), static_cast<int>(items.size()));
}

namespace detail {
inline int ResizeStringCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}
} // namespace detail

// Text field bound to a std::string of any length (no fixed buffer to outgrow).
inline bool InputString(const char* id, const char* hint, std::string& text, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextWithHint(id, hint ? hint : "", text.data(), text.capacity() + 1, flags,
                                    detail::ResizeStringCallback, &text);
}
inline bool InputStringMultiline(const char* id, std::string& text, const ImVec2& size = ImVec2(0.0f, 0.0f),
                                 ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(id, text.data(), text.capacity() + 1, size, flags,
                                     detail::ResizeStringCallback, &text);
}

// A quiet, unframed expandable row in the accent colour. Unlike a framed
// CollapsingHeader it stays inside a card's padding. Returns true while open; the
// caller draws the content and calls NO TreePop.
inline bool Disclosure(const char* label, const char* id, bool defaultOpen = false) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s###%s", label ? label : "", id ? id : "disclosure");
    ImGui::PushStyleColor(ImGuiCol_Text, Colors().Accent);
    const bool open = ImGui::TreeNodeEx(buf, ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    ImGui::PopStyleColor();
    return open;
}

// A small NON-modal confirmation, opened with ImGui::OpenPopup(id). Returns true
// on the frame the yes button is pressed.
inline bool ConfirmPopup(const char* id, const char* text, const char* yes = "Yes", const char* no = "Cancel") {
    bool confirmed = false;
    if (ImGui::BeginPopup(id)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text ? text : "");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s###yes", yes ? yes : "Yes");
        if (ImGui::Button(buf)) {
            confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%s###no", no ? no : "Cancel");
        if (SubtleButton(buf)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return confirmed;
}

// A small rounded chip ("Live", "Off", "3 new"): text in `color` on a tinted
// fill, vertically centred on a frame-height row so it lines up with
// checkboxes and buttons on the same line.
inline void StatusPill(const char* text, ImU32 color) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(text ? text : "");
    const float padX = style.FramePadding.x * 0.75f;
    const float pillH = (std::min)(ImGui::GetFrameHeight(), textSize.y + style.FramePadding.y);
    const ImVec2 size(textSize.x + padX * 2.0f, ImGui::GetFrameHeight());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    if (!ImGui::IsItemVisible()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Every corner on a whole pixel: with ImGui 1.93's default stroke (inside the
    // rectangle, no +0.5 offset) the 1 px border then covers exactly one pixel row.
    // pillH is fractional at most font scales, so the bottom edge is snapped too.
    const float top = std::floor(pos.y + (size.y - pillH) * 0.5f + 0.5f);
    const ImVec2 p0(std::floor(pos.x + 0.5f), top),
                 p1(std::floor(pos.x + size.x + 0.5f), std::floor(top + pillH + 0.5f));
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * 0.18f));
    const ImU32 border = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * 0.55f));
    dl->AddRectFilled(p0, p1, fill, pillH * 0.5f);
    dl->AddRect(p0, p1, border, pillH * 0.5f);
    dl->AddText(ImVec2(std::floor(p0.x + padX + 0.5f), std::floor(top + (p1.y - top - textSize.y) * 0.5f + 0.5f)), color,
                text ? text : "");
}

// ---------------------------------------------------------------------------
// Hotkeys: Windows virtual-key codes (what GetAsyncKeyState / SendInput use)
// ---------------------------------------------------------------------------
namespace detail {
struct KeyRow { ImGuiKey key; int vk; const char* name; };
inline const KeyRow* KeyTable(int& count) {
    static const KeyRow rows[] = {
        { ImGuiKey_Tab, 0x09, "Tab" }, { ImGuiKey_LeftArrow, 0x25, "Left" }, { ImGuiKey_RightArrow, 0x27, "Right" },
        { ImGuiKey_UpArrow, 0x26, "Up" }, { ImGuiKey_DownArrow, 0x28, "Down" }, { ImGuiKey_PageUp, 0x21, "Page Up" },
        { ImGuiKey_PageDown, 0x22, "Page Down" }, { ImGuiKey_Home, 0x24, "Home" }, { ImGuiKey_End, 0x23, "End" },
        { ImGuiKey_Insert, 0x2D, "Insert" }, { ImGuiKey_Delete, 0x2E, "Delete" }, { ImGuiKey_Backspace, 0x08, "Backspace" },
        { ImGuiKey_Space, 0x20, "Space" }, { ImGuiKey_Enter, 0x0D, "Enter" },
        { ImGuiKey_LeftCtrl, 0x11, "Ctrl" }, { ImGuiKey_RightCtrl, 0x11, "Ctrl" },
        { ImGuiKey_LeftShift, 0x10, "Shift" }, { ImGuiKey_RightShift, 0x10, "Shift" },
        { ImGuiKey_LeftAlt, 0x12, "Alt" }, { ImGuiKey_RightAlt, 0x12, "Alt" },
        { ImGuiKey_LeftSuper, 0x5B, "Left Win" }, { ImGuiKey_RightSuper, 0x5C, "Right Win" },
        { ImGuiKey_Menu, 0x5D, "Menu" }, { ImGuiKey_Oem102, 0xE2, "OEM 102" },
        { ImGuiKey_AppBack, 0xA6, "Browser Back" }, { ImGuiKey_AppForward, 0xA7, "Browser Forward" },
        { ImGuiKey_KeypadEnter, 0x0D, "Enter" }, { ImGuiKey_KeypadEqual, 0x92, "Num =" },
        { ImGuiKey_0, 0x30, "0" }, { ImGuiKey_1, 0x31, "1" }, { ImGuiKey_2, 0x32, "2" }, { ImGuiKey_3, 0x33, "3" },
        { ImGuiKey_4, 0x34, "4" }, { ImGuiKey_5, 0x35, "5" }, { ImGuiKey_6, 0x36, "6" }, { ImGuiKey_7, 0x37, "7" },
        { ImGuiKey_8, 0x38, "8" }, { ImGuiKey_9, 0x39, "9" },
        { ImGuiKey_A, 0x41, "A" }, { ImGuiKey_B, 0x42, "B" }, { ImGuiKey_C, 0x43, "C" }, { ImGuiKey_D, 0x44, "D" },
        { ImGuiKey_E, 0x45, "E" }, { ImGuiKey_F, 0x46, "F" }, { ImGuiKey_G, 0x47, "G" }, { ImGuiKey_H, 0x48, "H" },
        { ImGuiKey_I, 0x49, "I" }, { ImGuiKey_J, 0x4A, "J" }, { ImGuiKey_K, 0x4B, "K" }, { ImGuiKey_L, 0x4C, "L" },
        { ImGuiKey_M, 0x4D, "M" }, { ImGuiKey_N, 0x4E, "N" }, { ImGuiKey_O, 0x4F, "O" }, { ImGuiKey_P, 0x50, "P" },
        { ImGuiKey_Q, 0x51, "Q" }, { ImGuiKey_R, 0x52, "R" }, { ImGuiKey_S, 0x53, "S" }, { ImGuiKey_T, 0x54, "T" },
        { ImGuiKey_U, 0x55, "U" }, { ImGuiKey_V, 0x56, "V" }, { ImGuiKey_W, 0x57, "W" }, { ImGuiKey_X, 0x58, "X" },
        { ImGuiKey_Y, 0x59, "Y" }, { ImGuiKey_Z, 0x5A, "Z" },
        { ImGuiKey_F1, 0x70, "F1" }, { ImGuiKey_F2, 0x71, "F2" }, { ImGuiKey_F3, 0x72, "F3" }, { ImGuiKey_F4, 0x73, "F4" },
        { ImGuiKey_F5, 0x74, "F5" }, { ImGuiKey_F6, 0x75, "F6" }, { ImGuiKey_F7, 0x76, "F7" }, { ImGuiKey_F8, 0x77, "F8" },
        { ImGuiKey_F9, 0x78, "F9" }, { ImGuiKey_F10, 0x79, "F10" }, { ImGuiKey_F11, 0x7A, "F11" }, { ImGuiKey_F12, 0x7B, "F12" },
        { ImGuiKey_F13, 0x7C, "F13" }, { ImGuiKey_F14, 0x7D, "F14" }, { ImGuiKey_F15, 0x7E, "F15" }, { ImGuiKey_F16, 0x7F, "F16" },
        { ImGuiKey_F17, 0x80, "F17" }, { ImGuiKey_F18, 0x81, "F18" }, { ImGuiKey_F19, 0x82, "F19" }, { ImGuiKey_F20, 0x83, "F20" },
        { ImGuiKey_F21, 0x84, "F21" }, { ImGuiKey_F22, 0x85, "F22" }, { ImGuiKey_F23, 0x86, "F23" }, { ImGuiKey_F24, 0x87, "F24" },
        { ImGuiKey_Apostrophe, 0xDE, "'" }, { ImGuiKey_Comma, 0xBC, "," }, { ImGuiKey_Minus, 0xBD, "-" },
        { ImGuiKey_Period, 0xBE, "." }, { ImGuiKey_Slash, 0xBF, "/" }, { ImGuiKey_Semicolon, 0xBA, ";" },
        { ImGuiKey_Equal, 0xBB, "=" }, { ImGuiKey_LeftBracket, 0xDB, "[" }, { ImGuiKey_Backslash, 0xDC, "\\" },
        { ImGuiKey_RightBracket, 0xDD, "]" }, { ImGuiKey_GraveAccent, 0xC0, "`" },
        { ImGuiKey_CapsLock, 0x14, "Caps Lock" }, { ImGuiKey_ScrollLock, 0x91, "Scroll Lock" },
        { ImGuiKey_NumLock, 0x90, "Num Lock" }, { ImGuiKey_PrintScreen, 0x2C, "Print Screen" },
        { ImGuiKey_Pause, 0x13, "Pause" },
        { ImGuiKey_Keypad0, 0x60, "Num 0" }, { ImGuiKey_Keypad1, 0x61, "Num 1" }, { ImGuiKey_Keypad2, 0x62, "Num 2" },
        { ImGuiKey_Keypad3, 0x63, "Num 3" }, { ImGuiKey_Keypad4, 0x64, "Num 4" }, { ImGuiKey_Keypad5, 0x65, "Num 5" },
        { ImGuiKey_Keypad6, 0x66, "Num 6" }, { ImGuiKey_Keypad7, 0x67, "Num 7" }, { ImGuiKey_Keypad8, 0x68, "Num 8" },
        { ImGuiKey_Keypad9, 0x69, "Num 9" }, { ImGuiKey_KeypadDecimal, 0x6E, "Num ." },
        { ImGuiKey_KeypadDivide, 0x6F, "Num /" }, { ImGuiKey_KeypadMultiply, 0x6A, "Num *" },
        { ImGuiKey_KeypadSubtract, 0x6D, "Num -" }, { ImGuiKey_KeypadAdd, 0x6B, "Num +" },
    };
    count = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
    return rows;
}
// The id of the HotkeyCapture currently waiting for a key (one per plugin DLL).
inline ImGuiID& CapturingId() {
    static ImGuiID id = 0;
    return id;
}
} // namespace detail

// True while any HotkeyCapture of this plugin waits for a key: pause your own
// hotkey handling meanwhile, so binding a key does not also trigger it.
inline bool IsCapturingHotkey() { return detail::CapturingId() != 0; }

// Name of a virtual-key code: "F5", "Mouse 4", "None" for 0, "Key 0xNN" otherwise.
// The returned pointer is static storage (the hex form: one buffer, valid until
// the next call).
inline const char* KeyName(int vk) {
    switch (vk) {
    case 0: return "None";
    case 0x01: return "Left Mouse";
    case 0x02: return "Right Mouse";
    case 0x04: return "Middle Mouse";
    case 0x05: return "Mouse 4";
    case 0x06: return "Mouse 5";
    // Side-specific codes saved by older GetAsyncKeyState-based captures.
    case 0xA0: return "Left Shift";
    case 0xA1: return "Right Shift";
    case 0xA2: return "Left Ctrl";
    case 0xA3: return "Right Ctrl";
    case 0xA4: return "Left Alt";
    case 0xA5: return "Right Alt";
    default: break;
    }
    int count = 0;
    const detail::KeyRow* rows = detail::KeyTable(count);
    for (int i = 0; i < count; ++i)
        if (rows[i].vk == vk) return rows[i].name;
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "Key 0x%02X", vk & 0xFF);
    return buf;
}

// A key binding button: shows the bound key; click it, then press a key or a
// mouse button (right, middle, 4, 5) to bind it, Esc or a click elsewhere to
// cancel. With allowClear a quiet "Clear" button sets it to 0 (None). Stores a
// Windows virtual-key code. Returns true when *vk changed. `id` must be unique
// in the window ("##hotkey").
inline bool HotkeyCapture(const char* id, int* vk, const char* tip = nullptr, bool allowClear = true) {
    if (!vk) return false;
    bool changed = false;
    ImGui::PushID(id);
    const ImGuiID self = ImGui::GetID("##capture");
    ImGuiID& capturing = detail::CapturingId();
    const bool waiting = capturing == self;

    char label[96];
    if (waiting) std::snprintf(label, sizeof(label), "Press a key... (Esc cancels)###capture");
    else std::snprintf(label, sizeof(label), "%s###capture", KeyName(*vk));
    const float minW = ImGui::GetFontSize() * 7.0f;
    const float w = (std::max)(minW, ButtonWidth(label));
    // Idle it reads like a field (quiet surface); waiting it takes the theme's
    // filled button colour so the capture state is obvious.
    const bool clicked = waiting ? ImGui::Button(label, ImVec2(w, 0.0f)) : SubtleButton(label, ImVec2(w, 0.0f));
    const bool hovered = ImGui::IsItemHovered();
    Tip(tip);

    if (clicked && !waiting) {
        capturing = self;
    } else if (waiting) {
        int captured = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            capturing = 0;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered) {
            capturing = 0;
        } else {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) captured = 0x02;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) captured = 0x04;
            else if (ImGui::IsMouseClicked(3)) captured = 0x05;
            else if (ImGui::IsMouseClicked(4)) captured = 0x06;
            else {
                int count = 0;
                const detail::KeyRow* rows = detail::KeyTable(count);
                for (int i = 0; i < count && captured < 0; ++i)
                    if (ImGui::IsKeyPressed(rows[i].key, false)) captured = rows[i].vk;
            }
        }
        if (captured >= 0) {
            capturing = 0;
            if (*vk != captured) { *vk = captured; changed = true; }
        }
    }

    if (allowClear) {
        ImGui::SameLine();
        ImGui::BeginDisabled(*vk == 0 && !waiting);
        if (SubtleButton("Clear###clear")) {
            if (capturing == self) capturing = 0;
            if (*vk != 0) { *vk = 0; changed = true; }
        }
        ImGui::EndDisabled();
        Tip("Removes the binding.");
    }
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------
// In-game overlay drawing (ImDrawList: background / foreground lists or your
// own transparent window)
// ---------------------------------------------------------------------------

// Rounds to the nearest whole pixel: text and 1 px lines stay crisp. With ImGui
// 1.93 (stroke inside the rectangle by default) pass snapped corners to AddRect
// unchanged; never add the pre-1.93 +0.5 inset.
inline float SnapPx(float v) { return std::floor(v + 0.5f); }
inline ImVec2 SnapPx(const ImVec2& v) { return ImVec2(SnapPx(v.x), SnapPx(v.y)); }

// Text with a 1 px dark outline, pixel-snapped, readable on any ground. The
// outline follows the text's alpha. font / fontSize default to the current font.
inline void TextOutlined(ImDrawList* dl, ImFont* font, float fontSize, const ImVec2& pos, ImU32 col,
                         const char* text, const char* textEnd = nullptr) {
    if (!dl || !text) return;
    if (!font) font = ImGui::GetFont();
    if (fontSize <= 0.0f) fontSize = ImGui::GetFontSize();
    const ImVec2 p = SnapPx(pos);
    const unsigned alpha = (col >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(alpha * 0.85f));
    static const float offsets[5][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { 1, 1 } };
    for (const auto& o : offsets)
        dl->AddText(font, fontSize, ImVec2(p.x + o[0], p.y + o[1]), outline, text, textEnd);
    dl->AddText(font, fontSize, p, col, text, textEnd);
}
inline void TextOutlined(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text, const char* textEnd = nullptr) {
    TextOutlined(dl, nullptr, 0.0f, pos, col, text, textEnd);
}

// The consistent in-game panel: translucent dark fill, rounded, a faint light
// border. Draw it first, then the content inset by PanelPadding().
inline void Panel(ImDrawList* dl, const ImVec2& min, const ImVec2& max, float opacity = 0.78f) {
    if (!dl) return;
    const ImVec2 a = SnapPx(min), b = SnapPx(max);
    const float rounding = SnapPx(ImGui::GetFontSize() * 0.35f);
    const int fillA = static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
    dl->AddRectFilled(a, b, IM_COL32(12, 14, 18, fillA), rounding);
    // ImGui 1.93 strokes inside the rectangle by default and adds no +0.5 itself:
    // the snapped corners put the 1 px border exactly on the outer pixel ring (the
    // pre-1.93 half-pixel inset would smear it over two half-alpha pixels).
    dl->AddRect(a, b, IM_COL32(255, 255, 255, static_cast<int>(fillA * 0.14f)), rounding);
}
// Inner padding of a Panel, scaled with the font.
inline ImVec2 PanelPadding() {
    const float p = SnapPx(ImGui::GetFontSize() * 0.5f);
    return ImVec2(p, SnapPx(p * 0.75f));
}

} // namespace PluginUi
