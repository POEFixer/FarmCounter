// Text.h — how FarmCounter's texts follow the program language (PluginI18n.h, through PluginSDK.h).
//
//   * Every text a user reads is a key of lang/en.json (lang/ru.json, ... next to the DLL):
//     I18n::Tr("key") for a text, I18n::TrF("key", { { "name", value } }) for one with {name}
//     placeholders, I18n::TrId("key") ("<text>###<key>") for every widget whose label is its
//     ImGui id (Checkbox, Button, Selectable, BeginTabItem, Slider*, InputText*, TableSetupColumn).
//   * A text is never a printf format: Num formats numbers in code into named TrF arguments, and
//     the result is drawn with TextUnformatted (never as a format).
//   * Nothing translated lives in a static: tables hold keys written I18n::Key("..."), looked up
//     when drawn; a worker or a model stores keys, never texts.
//   * Layout is code: no leading, trailing or doubled spaces in a text; two values on one line are
//     two keys and ImGui::SameLine; widths come from the translated text or the font size.
//   * English on purpose: the game's own names (zones, items, map modifiers), log lines.
//   * After editing lang/en.json run python tools/plugin_i18n/gen.py Plugins/FarmCounter
//     (it writes src/lang_en.generated.h, included once by FarmCounter.cpp).
#pragma once
#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include <imgui.h>

#include <sal.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace FcText {

namespace I18n = PluginSDK::I18n;

// printf for a TrF argument: Num("%.1f", value). The format is code, never a translation.
inline std::string Num(_In_z_ _Printf_format_string_ const char* format, ...) {
    char buf[128];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// One line of a text TrF built (TextUnformatted: a translation is no format).
inline void Show(const std::string& text) { ImGui::TextUnformatted(text.c_str()); }

// The width a combo needs for its widest item text, at least `minimum`.
inline float ComboWidthFor(float minimum, const char* const* texts, int count) {
    float width = minimum;
    for (int i = 0; i < count; ++i)
        width = (std::max)(width, std::ceil(ImGui::CalcTextSize(texts[i]).x + ImGui::GetFrameHeight() +
                                             2 * ImGui::GetStyle().FramePadding.x) + 1);
    return width;
}

// A combo over keys: the preview is the selected key's text, every item a Selectable labelled
// TrId(key), so its ImGui id ("###<key>") is the same in every language. `value` is clamped.
inline bool KeyCombo(const char* id, int& value, const char* const* keys, int count) {
    if (!keys || count <= 0) return false;
    const int current = std::clamp(value, 0, count - 1);
    bool changed = false;
    if (ImGui::BeginCombo(id, I18n::Tr(keys[current]))) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(I18n::TrId(keys[i]), i == current)) {
                value = i;
                changed = true;
            }
            if (i == current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// The overlay currency names, in OverlaySettings::overlayCurrency order (0 exalted, 1 divine,
// 2 chaos): the Settings and Statistics currency combos share them.
inline constexpr const char* kCurrencyKeys[] = {
    I18n::Key("currency_exalted"), I18n::Key("currency_divine"), I18n::Key("currency_chaos"),
};

} // namespace FcText
