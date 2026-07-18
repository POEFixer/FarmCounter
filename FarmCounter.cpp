// FarmCounter.cpp — thin plugin shell.
//
// All real work lives in the src/ modules; this file only owns the module
// instances, wires them together, and dispatches the SDK lifecycle hooks.
// Pricing flows through the host price service (ctx()->Prices via PriceProvider),
// so the plugin makes zero HTTP calls and owns no price database.
#include "sdk/PluginSDK.h"
#include <imgui.h>
#include "version.h"

#include "src/FarmTypes.h"
#include "src/PriceProvider.h"
#include "src/IconTextures.h"
#include "src/ResourceReaders.h"
#include "src/KillCounter.h"
#include "src/ZoneNames.h"
#include "src/Persistence.h"
#include "src/FarmTracker.h"
#include "src/ui/Overlay.h"
#include "src/ui/Settings.h"

#include <d3d11.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <filesystem>
#include <vector>
#include <cmath>
#include <cstring>

// Generate a sine-wave WAV in memory and play it via PlaySound.
// freq = Hz, durationMs = milliseconds, volume = 0.0-1.0.
static void PlayTone(int freq, int durationMs, float volume) {
    const int sampleRate = 44100;
    const int numSamples = sampleRate * durationMs / 1000;

    // WAV header: RIFF/fmt/data chunks
    struct WavHeader {
        char     riff[4]      = {'R','I','F','F'};
        uint32_t chunkSize;
        char     wave[4]      = {'W','A','V','E'};
        char     fmt[4]       = {'f','m','t',' '};
        uint32_t fmtSize      = 16;
        uint16_t audioFormat  = 1;   // PCM
        uint16_t numChannels  = 1;
        uint32_t sampleRateF;
        uint32_t byteRate;
        uint16_t blockAlign   = 2;
        uint16_t bitsPerSample= 16;
        char     data[4]      = {'d','a','t','a'};
        uint32_t dataSize;
    };

    const uint32_t dataSize = (uint32_t)(numSamples * 2);

    // SND_ASYNC|SND_MEMORY: PlaySound streams from the buffer AFTER this function
    // returns, so the data must outlive the call. Two persistent buffers alternate
    // so a new tone never rewrites the block a still-playing tone is read from.
    // (Only the render thread calls this — no synchronization needed.)
    static std::vector<uint8_t> s_toneBuf[2];
    static int s_toneWhich = 0;
    std::vector<uint8_t>& buf = s_toneBuf[s_toneWhich ^= 1];
    buf.resize(sizeof(WavHeader) + dataSize);

    WavHeader hdr;
    hdr.chunkSize    = 36 + dataSize;
    hdr.sampleRateF  = sampleRate;
    hdr.byteRate     = sampleRate * 2;
    hdr.dataSize     = dataSize;
    std::memcpy(buf.data(), &hdr, sizeof(WavHeader));

    int16_t* samples = reinterpret_cast<int16_t*>(buf.data() + sizeof(WavHeader));
    float amp = std::clamp(volume, 0.f, 1.f) * 32767.f;
    const int fadeLen = (std::max)(1, numSamples / 10);  // >= 1: no div-by-zero for tiny durations; (std::max) dodges the windows.h macro
    for (int i = 0; i < numSamples; i++) {
        // Short fade-out in last 10% to avoid click
        float env = (i > numSamples - fadeLen)
            ? (float)(numSamples - i) / (float)fadeLen
            : 1.f;
        samples[i] = (int16_t)(amp * env * std::sin(2.f * 3.14159265f * freq * i / sampleRate));
    }

    PlaySoundA(reinterpret_cast<LPCSTR>(buf.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

class FarmCounterPlugin : public PluginSDK::Plugin {
public:
    const char* GetName() const override { return "FarmCounter"; }

    bool WantsOverlay() const override {
        return m_settings.wantsOverlay || m_settings.itShow || m_settings.hbShow;
    }

    void OnEnable(bool /*isGameAttached*/) override {
        m_dir = DirectoryPath();                       // plugin ROOT (modules append subpaths)
        ::LoadSettings(m_dir, m_settings);
        ::LoadCustomPrices(m_dir, m_prices);
        m_zones.Load(m_dir);
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        m_prices.SetContext(ctx());
        m_icons.SetDevice(static_cast<ID3D11Device*>(ctx()->D3DDevice));
        m_icons.LoadCurrencyIcons();
        m_tracker.Init(ctx(), &m_prices, &m_zones, m_dir);  // Init BEFORE OnEnable
        m_tracker.OnEnable();                               // OnEnable loads the map history
        ctx()->Log.Info("FarmCounter " ZONETIMER_VERSION " enabled");
    }

    void OnDisable() override {
        m_tracker.OnDisable();
        m_icons.Release();
        ctx()->Log.Info("FarmCounter disabled");
    }

    void DrawUI() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        auto snap = ctx()->Game.GetSnapshot();
        if (!snap.IsAttached) return;
        // Esc menu = real pause in solo play: freeze the map/session timers and
        // skip the frame (the overlay is hidden while the menu covers the game).
        if (snap.State == PluginSDK::GameState::Escape) { m_tracker.SetPaused(true); return; }
        if (snap.State != PluginSDK::GameState::InGame) return;
        m_tracker.SetPaused(false);

        m_kills.Update(snap);
        // Beacon-element BFS re-find only runs outside town/hideout — idle areas
        // never pay for UI-tree sweeps (a cached element still refreshes there).
        m_resources.Tick(ctx(), !snap.IsTown && !snap.IsHideout);
        m_tracker.OnFrame(snap, m_resources.Hiveblood(), m_resources.Incursion(), &m_kills);

        // Atziri beacon gain chime (the one overlay feature deferred to the shell).
        const ResourceReaders::ItState it = m_resources.Incursion();
        if (m_lastTokens >= 0 && it.ok && it.cur > m_lastTokens && m_settings.itSound)
            PlayTone(880, 120, m_settings.itVolume);
        m_lastTokens = it.ok ? it.cur : -1;

        // The overlay reads the model/services and clears m_settingsOpen via the
        // outSettingsConsumed handshake, so the shell does not reset it here.
        OverlayDeps od{
            &m_tracker, &m_prices, &m_icons, &m_kills, &m_resources, &m_zones,
            &m_settings, m_settingsOpen, &m_settingsOpen
        };
        RenderOverlay(od);
    }

    void DrawSettings() override {
        m_settingsOpen = true;                          // consumed + reset by RenderOverlay
        SettingsDeps sd{
            &m_tracker, &m_prices, &m_settings, &m_zones, &m_kills, &m_resources, &m_icons, m_dir,
            [](float vol) { PlayTone(880, 200, vol); }
        };
        RenderSettings(sd);
    }

    void SaveSettings() override { ::SaveSettings(m_dir, m_settings); }

private:
    std::filesystem::path m_dir;
    OverlaySettings       m_settings;
    PriceProvider         m_prices;
    IconTextures          m_icons;
    ResourceReaders       m_resources;
    KillCounter           m_kills;
    ZoneNames             m_zones;
    FarmTracker           m_tracker;
    bool                  m_settingsOpen = false;
    int                   m_lastTokens   = -1;   // last seen beacon count (chime edge-detect)
};

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() {
    return new FarmCounterPlugin();
}

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) {
    delete p;
}
