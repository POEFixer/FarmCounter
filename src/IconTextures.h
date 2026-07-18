#pragma once
// IconTextures.h — D3D11 texture cache for currency icons (Resources) and item icons (core iconPath).
#include <imgui.h>
#include <d3d11.h>
#include <string>
#include <unordered_map>

struct IconTex { ImTextureID srv = 0; int w = 0, h = 0; bool valid = false; };

class IconTextures {
public:
    ~IconTextures() { Release(); }
    void SetDevice(ID3D11Device* dev) { m_device = dev; }
    void LoadCurrencyIcons();                       // ex/div/chaos from Resources/currency/poe2
    const IconTex& Currency(int overlayCurrencyIdx) const;  // 0=ex,1=div,2=chaos
    IconTex Item(const std::string& localPngPath);  // path -> SRV (cached; "" path => invalid)
    void Release();

private:
    static IconTex LoadPngFile(ID3D11Device*, const std::wstring& widePath);
    static IconTex LoadPngBytes(ID3D11Device*, const unsigned char* bytes, int len);

    ID3D11Device* m_device = nullptr;
    IconTex m_ex, m_div, m_chaos;       // currency
    IconTex m_empty;                    // returned when nothing valid
    std::unordered_map<std::string, IconTex> m_items;  // localPath -> tex (caches misses too)
};
