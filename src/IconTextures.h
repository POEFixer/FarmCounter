#pragma once
// IconTextures.h — D3D11 texture cache for currency icons (Resources), item
// icons (core iconPath) and the radar sprite-sheet monster icons.
#include <imgui.h>
#include <d3d11.h>
#include <string>
#include <unordered_map>

struct IconTex { ImTextureID srv = 0; int w = 0, h = 0; bool valid = false; };

// A sub-rect of the radar atlas (Resources/radar/icons.png, 64px grid) — the
// same sprite sheet the built-in radar and the KillCount plugin draw from.
struct AtlasIcon { ImTextureID tex = 0; ImVec2 uv0{0, 0}, uv1{0, 0}; bool valid = false; };

class IconTextures {
public:
    ~IconTextures() { Release(); }
    void SetDevice(ID3D11Device* dev) { m_device = dev; }
    void LoadCurrencyIcons();                       // ex/div/chaos from Resources/currency/poe2
    void LoadRadarAtlas();                          // monster rarity icons from Resources/radar
    const IconTex& Currency(int overlayCurrencyIdx) const;  // 0=ex,1=div,2=chaos
    // Radar monster icon by rarity (0=Normal 1=Magic 2=Rare 3=Unique); .valid
    // false when the atlas is missing — callers fall back to colored dots.
    const AtlasIcon& Monster(int rarity) const;
    IconTex Item(const std::string& localPngPath);  // path -> SRV (cached; "" path => invalid)
    void Release();

private:
    static IconTex LoadPngFile(ID3D11Device*, const std::wstring& widePath);
    static IconTex LoadPngBytes(ID3D11Device*, const unsigned char* bytes, int len);

    ID3D11Device* m_device = nullptr;
    IconTex m_ex, m_div, m_chaos;       // currency
    IconTex m_atlas;                    // radar sprite sheet (owns the SRV)
    AtlasIcon m_monster[4];             // per-rarity sub-rects of m_atlas
    AtlasIcon m_atlasEmpty;
    IconTex m_empty;                    // returned when nothing valid
    std::unordered_map<std::string, IconTex> m_items;  // localPath -> tex (caches misses too)
};
