#include "IconTextures.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#include <stb_image.h>

namespace fs = std::filesystem;

IconTex IconTextures::LoadPngBytes(ID3D11Device* device, const unsigned char* bytes, int len) {
    IconTex tex{};
    if (!device || !bytes || len <= 0) return tex;
    int w = 0, h = 0;
    unsigned char* data = stbi_load_from_memory(bytes, len, &w, &h, nullptr, 4);
    if (!data) return tex;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)w; desc.Height = (UINT)h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {}; init.pSysMem = data; init.SysMemPitch = (UINT)(w * 4);
    ID3D11Texture2D* t = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &init, &t);
    stbi_image_free(data);
    if (FAILED(hr) || !t) return tex;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = desc.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(t, &sd, &srv);
    t->Release();
    if (FAILED(hr) || !srv) return tex;
    tex.srv = (ImTextureID)reinterpret_cast<uintptr_t>(srv);
    tex.w = w; tex.h = h; tex.valid = true;
    return tex;
}

IconTex IconTextures::LoadPngFile(ID3D11Device* device, const std::wstring& widePath) {
    std::ifstream f(fs::path(widePath), std::ios::binary);
    if (!f.is_open()) return IconTex{};
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return IconTex{};
    return LoadPngBytes(device, bytes.data(), (int)bytes.size());
}

void IconTextures::LoadCurrencyIcons() {
    if (!m_device) return;
    // Watchdog re-enable does not call OnDisable first. Replace each owning
    // reference explicitly, preserving reloads when assets or the device change.
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    fs::path base = fs::path(exe).parent_path() / L"Resources" / L"currency" / L"poe2";
    auto load = [&](IconTex& tex, const wchar_t* name) {
        const IconTex replacement = LoadPngFile(m_device, (base / name).wstring());
        if (tex.srv) reinterpret_cast<ID3D11ShaderResourceView*>(tex.srv)->Release();
        tex = replacement;
    };
    load(m_ex, L"exalted.png");
    load(m_div, L"divine.png");
    load(m_chaos, L"chaos.png");
}

const IconTex& IconTextures::Currency(int idx) const {
    if (idx == 1) return m_div;
    if (idx == 2) return m_chaos;
    return m_ex;  // 0 or out-of-range
}

void IconTextures::LoadRadarAtlas() {
    if (!m_device || m_atlas.valid) return;
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    fs::path base = fs::path(exe).parent_path() / L"Resources" / L"radar";
    m_atlas = LoadPngFile(m_device, (base / L"icons.png").wstring());
    if (!m_atlas.valid || m_atlas.w <= 0 || m_atlas.h <= 0) return;

    // Grid cells of the per-rarity monster icons — the radar's icons.json
    // positions, with the KillCount plugin's hardcoded values as fallback.
    struct Slot { const char* name; int gx, gy; };
    Slot slots[5] = {
        { "Normal Monster", 0, 14 },
        { "Magic Monster",  6,  3 },
        { "Rare Monster",   4, 57 },
        { "Unique Monster", 6, 57 },
        { "RogueExile",     0, 61 },   // dedicated Rogue Exile icon (radar IconRegistry)
    };
    std::ifstream f(base / L"icons.json");
    if (f.is_open()) {
        nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.is_array()) {
            for (const auto& item : j) {
                if (!item.is_object()) continue;
                auto nit = item.find("name");
                if (nit == item.end() || !nit->is_string()) continue;
                const std::string name = nit->get<std::string>();
                for (auto& s : slots) {
                    if (name != s.name) continue;
                    auto gxIt = item.find("gridX"), gyIt = item.find("gridY");
                    if (gxIt != item.end() && gxIt->is_number()) s.gx = (int)gxIt->get<double>();
                    if (gyIt != item.end() && gyIt->is_number()) s.gy = (int)gyIt->get<double>();
                }
            }
        }
    }

    constexpr float kCell = 64.0f;   // radar atlas grid size
    const float cu = kCell / (float)m_atlas.w;
    const float cv = kCell / (float)m_atlas.h;
    auto makeIcon = [&](const Slot& s) -> AtlasIcon {
        AtlasIcon a;
        a.tex   = m_atlas.srv;
        a.uv0   = ImVec2(s.gx * cu, s.gy * cv);
        a.uv1   = ImVec2((s.gx + 1) * cu, (s.gy + 1) * cv);
        a.valid = true;
        return a;
    };
    for (int i = 0; i < 4; i++) m_monster[i] = makeIcon(slots[i]);
    m_rogue = makeIcon(slots[4]);
}

const AtlasIcon& IconTextures::Monster(int rarity) const {
    if (rarity < 0 || rarity > 3) return m_atlasEmpty;
    return m_monster[rarity];
}

IconTex IconTextures::Item(const std::string& localPngPath) {
    if (localPngPath.empty()) return IconTex{};
    auto it = m_items.find(localPngPath);
    if (it != m_items.end()) return it->second;       // cached (incl. cached failures)
    if (!m_device) return IconTex{};                  // don't cache; retry once device ready
    // UTF-8 -> wide for Unicode-safe path.
    int wn = MultiByteToWideChar(CP_UTF8, 0, localPngPath.c_str(), -1, nullptr, 0);
    std::wstring wp; if (wn > 0) { wp.resize(wn - 1); MultiByteToWideChar(CP_UTF8, 0, localPngPath.c_str(), -1, wp.data(), wn); }
    IconTex tex = LoadPngFile(m_device, wp);
    m_items[localPngPath] = tex;                      // cache even invalid to avoid re-reading missing files
    return tex;
}

void IconTextures::Release() {
    auto rel = [](IconTex& t){ if (t.srv) { reinterpret_cast<ID3D11ShaderResourceView*>(t.srv)->Release(); } t = IconTex{}; };
    rel(m_ex); rel(m_div); rel(m_chaos);
    rel(m_atlas);
    for (auto& m : m_monster) m = AtlasIcon{};
    m_rogue = AtlasIcon{};
    for (auto& kv : m_items) rel(kv.second);
    m_items.clear();
}
