// PluginInfoWindow.h — an informational plugin window (a counter, a list, a timer) with the
// host's gear menu: Lock window, Game overlay mode (off = the window leaves the game and can
// be placed anywhere on the screens), Reset position, Close. The host remembers its place for
// each mode, so the plugin keeps no position for it. Header-only.
//
//     void DrawUI() override {
//         ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
//         if (!m_showList) return;
//         PluginSDK::InfoWindowOptions o;
//         o.id = "list";                          // stable, 1-48 of [A-Za-z0-9_.-]
//         o.title = "Skill DPS";                  // nullptr: give it ImGuiWindowFlags_NoTitleBar
//         o.flags = ImGuiWindowFlags_AlwaysAutoResize;
//         o.defaultPos = ImVec2(40, 200);
//         o.legacyPos = ImVec2(m_oldX, m_oldY);   // a position the plugin saved before (seeds once)
//         ImGui::SetNextWindowBgAlpha(0.8f);      // window-level style: before, popped after
//         PluginSDK::InfoWindow window(ctx()->Overlay, o, &m_showList);   // Close clears m_showList
//         if (!window) return;                    // collapsed, closed or not shown this frame
//         ImGui::Text("...");
//     }                                           // the window ends here
//
// Rules: DrawUI only. Never SetNextWindowPos / Collapsed for it, and SetNextWindowSize only for
// an AlwaysAutoResize / NoResize window (the host owns the rest).
// Push window-level style and call SetNextWindowBgAlpha / SetNextWindowSizeConstraints before
// it and pop after it ends, never in between (out of game the window is drawn in another ImGui
// context: the host binds the plugin to it while the window is open). Inside it draw only into
// the window (no GetForegroundDrawList). Detached() tells whether the window is out of game.
// After Close the plugin saves its show flag as it saves any setting.
// On a host older than 366 it is a plain ImGui window at legacyPos / defaultPos (no gear, the
// place kept for the session only): publish a plugin that uses it with min_host_version 366.
#pragma once

#include "PluginSDK.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"   // NextWindowData: dropped when nothing begins

#include <string>

namespace PluginSDK {

struct InfoWindowOptions {
    const char* id = nullptr;
    const char* title = nullptr;
    ImGuiWindowFlags flags = 0;
    ImVec2 defaultPos{ 40.0f, 40.0f };
    ImVec2 defaultSize{ 0.0f, 0.0f };    // resizable windows
    ImVec2 legacyPos{ -1.0f, -1.0f };    // exactly (-1, -1): none (a negative coordinate is a real place)
    int legacyCollapsed = -1;            // -1 none, 0 / 1
};

class InfoWindow {
public:
    InfoWindow(const OverlayService& overlay, const InfoWindowOptions& o, bool* open = nullptr)
        : m_overlay(overlay), m_open(open) {
        if (overlay.HasInfoWindows()) {
            // Also when closed: the host then begins nothing and drops the caller's
            // SetNextWindow* calls, so they never reach the next window.
            InfoWindowDescAbi desc{};
            desc.size_bytes = static_cast<uint32_t>(sizeof(desc));
            desc.window_id = o.id;
            desc.title = o.title;
            desc.imgui_flags = static_cast<int32_t>(o.flags);
            desc.default_x = o.defaultPos.x;
            desc.default_y = o.defaultPos.y;
            desc.default_w = o.defaultSize.x;
            desc.default_h = o.defaultSize.y;
            desc.legacy_x = o.legacyPos.x;
            desc.legacy_y = o.legacyPos.y;
            desc.legacy_collapsed = o.legacyCollapsed;
            InfoWindowFrameAbi frame{};
            frame.size_bytes = static_cast<uint32_t>(sizeof(frame));
            m_open8 = (open && !*open) ? 0 : 1;   // closed: the host begins nothing
            m_hostBegan = overlay.BeginInfoWindow(desc, open ? &m_open8 : nullptr, &frame) != 0;
            if (!m_hostBegan) return;
            m_visible = frame.visible != 0;
            m_detached = frame.detached != 0;
            m_interactive = frame.interactive != 0;
            return;
        }
        if (!o.id || (open && !*open)) {
            if (ImGuiContext* g = ImGui::GetCurrentContext()) g->NextWindowData.ClearFlags();
            return;
        }
        // An older host: a plain window (no gear, no out-of-game mode; ImGui keeps the
        // place for the session). ImGui window ids are shared by every plugin: the plugin's
        // own id keeps two plugins' "main" windows apart.
        const PluginInfoAbi* info = PluginSDK_GetInfo();
        const std::string name = std::string(o.title ? o.title : "") + "###" +
                                 ((info && info->id) ? info->id : "plugin") + "." + o.id;
        const ImVec2 start = (o.legacyPos.x == -1.0f && o.legacyPos.y == -1.0f) ? o.defaultPos : o.legacyPos;
        ImGui::SetNextWindowPos(start, ImGuiCond_FirstUseEver);
        if (o.defaultSize.x > 0.0f && o.defaultSize.y > 0.0f) ImGui::SetNextWindowSize(o.defaultSize, ImGuiCond_FirstUseEver);
        if (o.legacyCollapsed >= 0) ImGui::SetNextWindowCollapsed(o.legacyCollapsed == 1, ImGuiCond_FirstUseEver);
        m_visible = ImGui::Begin(name.c_str(), open, o.flags);
        m_plainBegan = true;
        m_interactive = !(o.flags & ImGuiWindowFlags_NoMouseInputs);
    }

    ~InfoWindow() {
        if (m_hostBegan) {
            m_overlay.EndInfoWindow();
            if (m_open && m_open8 == 0) *m_open = false;
        } else if (m_plainBegan) {
            ImGui::End();
        }
    }

    InfoWindow(const InfoWindow&) = delete;
    InfoWindow& operator=(const InfoWindow&) = delete;

    explicit operator bool() const { return m_visible; }
    bool Began() const { return m_hostBegan || m_plainBegan; }   // the window exists this frame (also collapsed)
    bool Visible() const { return m_visible; }
    bool Detached() const { return m_detached; }       // out of game (Game overlay mode off)
    bool Interactive() const { return m_interactive; } // takes the mouse this frame

private:
    const OverlayService& m_overlay;
    bool* m_open = nullptr;
    uint8_t m_open8 = 1;
    bool m_hostBegan = false;
    bool m_plainBegan = false;
    bool m_visible = false;
    bool m_detached = false;
    bool m_interactive = false;
};

} // namespace PluginSDK
