// PluginI18n.h — plugin localization: the plugin's own texts follow the program's UI language.
//
// Header-only; included by PluginSDK.h (and PluginUi.h for the kit texts), so a plugin never
// includes it itself. Depends on the C++ standard library, PluginAbi.h and <Windows.h> only
// (no min/max macro use: it compiles next to a Windows.h included without NOMINMAX).
//
// A plugin ships its texts as flat JSON files next to its DLL:
//
//     Plugins/<Name>/lang/en.json      { "settings_show_monsters": "Show monsters", ... }
//     Plugins/<Name>/lang/ru.json      the same keys, Russian texts
//     ... one file per program language (en, pt_BR, ru, th, de, fr, es, ja, ko, zh_TW)
//
// and the English file compiled in: `python tools/plugin_i18n/gen.py Plugins/<Name>` writes
// src/lang_en.generated.h (lang_en.generated.h without src/), included ONCE, in one .cpp, after
// PluginSDK.h. Then:
//
//     using namespace PluginSDK;
//     ImGui::TextUnformatted(I18n::Tr("status_ready"));                         // a text
//     ImGui::Checkbox(I18n::TrId("settings_show_monsters"), &m_show);             // "<text>###<key>"
//     const std::string line = I18n::TrF("loot_count", { { "count", countText } });  // "{count} items"
//     static constexpr const char* kTabs[] = { I18n::Key("tab_loot"), I18n::Key("tab_map") };  // keys
//
// Rules (python tools/plugin_i18n/gen.py --gate checks the files and the sources):
//   * Every widget whose label is its ImGui id takes TrId (Checkbox, Button, TreeNode,
//     BeginTabItem, Selectable, CollapsingHeader, MenuItem, Combo, TableSetupColumn): the id
//     stays the key's in every language, so open/closed state and saved layouts survive a
//     language switch.
//   * Texts are never printf formats: no %d / %s in a value (pass them to TextUnformatted, not
//     Text); placeholders are named ({count}) and filled by TrF, whose argument names must be
//     exactly the text's placeholders.
//   * Nothing translated lives in a static: a table holds keys, written I18n::Key("..."), and
//     looks their texts up when it draws (or rebuilds its cache when Revision() moves).
//   * Every key of lang/en.json is used: a literal of Tr / TrF / TrId / Key, or a key a plugin
//     builds at run time under a prefix it lists in lang/dynamic_prefixes.txt.
//   * Layout is code: no leading, trailing or doubled spaces in a value (Indent / SameLine).
//
// Lookup order: the current language's file, then the embedded English, then the key itself.
// Every table loaded is kept for the DLL's lifetime (at most ten), so a pointer Tr / TrId /
// KitText returned stays valid after a language switch. On an older host (before desktop 371)
// or before the host told the language, everything is English. Any thread.
//
// The host tells the language through HostAbi::get_ui_language: the SDK asks it whenever the host
// binds the plugin's ImGui (PluginSDK_BindImGui) and, at most every 250 ms, from Tr / TrF / TrId /
// KitText / Language / Revision themselves (the host binds only when an ImGui context changes).
#pragma once
#ifndef POEFIXER_PLUGIN_I18N_H
#define POEFIXER_PLUGIN_I18N_H 1

#include "PluginAbi.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PluginSDK::I18n {

// ---------------------------------------------------------------------------------------------
// The lang/<code>.json reader: one flat JSON object whose values are all strings.
// ---------------------------------------------------------------------------------------------
enum class ParseError : uint8_t {
    None,
    TooLarge,        // more than 1 MiB
    NotObject,       // not one well-formed object of "key": "value" members
    BadString,       // an unterminated string, a raw control character or invalid UTF-8
    BadEscape,       // an unknown escape, a malformed \uXXXX, or \u0000
    BadSurrogate,    // a \uXXXX surrogate without its pair
    NonStringValue,  // a number, true / false / null, an array or a nested object as a value
    DuplicateKey,
    TrailingData,    // anything but whitespace after the object
};

inline const char* ParseErrorText(ParseError e) {
    switch (e) {
    case ParseError::None:           return "no error";
    case ParseError::TooLarge:       return "larger than 1 MiB";
    case ParseError::NotObject:      return "not one JSON object of strings";
    case ParseError::BadString:      return "a malformed string";
    case ParseError::BadEscape:      return "a bad escape";
    case ParseError::BadSurrogate:   return "a lone surrogate";
    case ParseError::NonStringValue: return "a value that is not a string";
    case ParseError::DuplicateKey:   return "a duplicate key";
    case ParseError::TrailingData:   return "data after the object";
    }
    return "an unknown error";
}

namespace Detail {

inline constexpr size_t kMaxFileBytes = 1024 * 1024;

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Four hex digits at s[i..i+3] -> *cp.
inline bool ReadHex4(std::string_view s, size_t i, uint32_t* cp) {
    if (i + 4 > s.size()) return false;
    uint32_t v = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char c = s[i + k];
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *cp = v;
    return true;
}

// The length of the well-formed UTF-8 sequence at s[i] (no overlong form, no surrogate, at most
// U+10FFFF), 0 when it is not one.
inline size_t Utf8SequenceLength(std::string_view s, size_t i) {
    const auto at = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = at(i);
    if (c < 0x80) return 1;
    size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;   // the allowed range of the second byte
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c == 0xE0) { len = 3; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) len = 3;
    else if (c == 0xED) { len = 3; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) len = 3;
    else if (c == 0xF0) { len = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) len = 4;
    else if (c == 0xF4) { len = 4; hi = 0x8F; }
    else return 0;
    if (i + len > s.size()) return 0;
    if (at(i + 1) < lo || at(i + 1) > hi) return 0;
    for (size_t k = 2; k < len; ++k)
        if (at(i + k) < 0x80 || at(i + k) > 0xBF) return 0;
    return len;
}

// The JSON string starting at s[i] (a '"'); i ends after its closing quote.
inline ParseError ReadString(std::string_view s, size_t& i, std::string& out) {
    ++i;   // the opening quote
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            ++i;
            return ParseError::None;
        }
        if (static_cast<unsigned char>(c) < 0x20) return ParseError::BadString;
        if (c != '\\') {
            const size_t len = Utf8SequenceLength(s, i);
            if (len == 0) return ParseError::BadString;
            out.append(s.data() + i, len);
            i += len;
            continue;
        }
        if (i + 1 >= s.size()) return ParseError::BadString;
        const char e = s[i + 1];
        i += 2;
        switch (e) {
        case '"':  out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/'); break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!ReadHex4(s, i, &cp) || cp == 0) return ParseError::BadEscape;
            i += 4;
            if (cp >= 0xDC00 && cp <= 0xDFFF) return ParseError::BadSurrogate;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t low = 0;
                if (i + 1 >= s.size() || s[i] != '\\' || s[i + 1] != 'u' || !ReadHex4(s, i + 2, &low) ||
                    low < 0xDC00 || low > 0xDFFF)
                    return ParseError::BadSurrogate;
                i += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            AppendUtf8(out, cp);
            break;
        }
        default:
            return ParseError::BadEscape;
        }
    }
    return ParseError::BadString;   // unterminated
}

inline void SkipSpace(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

}  // namespace Detail

// Reads one flat JSON object of strings ({"key": "text", ...}): \uXXXX escapes with surrogate
// pairs, a UTF-8 byte order mark tolerated, at most 1 MiB. `out` is cleared first and filled only
// on success (None).
inline ParseError ParseFlatJson(std::string_view utf8, std::unordered_map<std::string, std::string>& out) {
    out.clear();
    if (utf8.size() > Detail::kMaxFileBytes) return ParseError::TooLarge;
    size_t i = 0;
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF)
        i = 3;
    Detail::SkipSpace(utf8, i);
    if (i >= utf8.size() || utf8[i] != '{') return ParseError::NotObject;
    ++i;
    std::unordered_map<std::string, std::string> result;
    Detail::SkipSpace(utf8, i);
    if (i < utf8.size() && utf8[i] == '}') {
        ++i;
    } else {
        for (;;) {
            Detail::SkipSpace(utf8, i);
            if (i >= utf8.size() || utf8[i] != '"') return ParseError::NotObject;
            std::string key;
            ParseError e = Detail::ReadString(utf8, i, key);
            if (e != ParseError::None) return e;
            Detail::SkipSpace(utf8, i);
            if (i >= utf8.size() || utf8[i] != ':') return ParseError::NotObject;
            ++i;
            Detail::SkipSpace(utf8, i);
            if (i >= utf8.size()) return ParseError::NotObject;
            if (utf8[i] != '"') return ParseError::NonStringValue;
            std::string value;
            e = Detail::ReadString(utf8, i, value);
            if (e != ParseError::None) return e;
            if (!result.emplace(std::move(key), std::move(value)).second) return ParseError::DuplicateKey;
            Detail::SkipSpace(utf8, i);
            if (i < utf8.size() && utf8[i] == ',') {
                ++i;
                continue;
            }
            if (i < utf8.size() && utf8[i] == '}') {
                ++i;
                break;
            }
            return ParseError::NotObject;
        }
    }
    Detail::SkipSpace(utf8, i);
    if (i != utf8.size()) return ParseError::TrailingData;
    out = std::move(result);
    return ParseError::None;
}

// ---------------------------------------------------------------------------------------------
// The embedded English (written by tools/plugin_i18n/gen.py into lang_en.generated.h)
// ---------------------------------------------------------------------------------------------
struct Entry {
    const char* key;
    const char* text;
};

namespace Detail {

struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

struct Text {
    std::string text;
    std::string id;   // "<text>###<key>"
};

// One language's texts; never changed once built.
struct Table {
    std::unordered_map<std::string, Text, Hash, std::equal_to<>> texts;
};

// The host's kit texts (plugin_kit_*) in one UI language, fetched on first use. A null entry is
// a key the host refused: the caller's English literal.
struct KitCache {
    std::unordered_map<std::string, std::unique_ptr<std::string>, Hash, std::equal_to<>> texts;
};

// One UI language this DLL has seen: kept for the DLL's lifetime.
struct LanguageSlot {
    std::string code;
    std::unique_ptr<Table> table;   // null: English (en itself, or its file is absent or refused)
    std::unique_ptr<KitCache> kit;
};

// What Tr and friends read: published as a whole, so a reader never mixes two languages.
struct State {
    uint64_t revision = 0;         // the host's UI-language revision this state follows
    const char* language = "en";   // the language Tr serves (a slot's code, or "en")
    const Table* table = nullptr;  // null: English
    KitCache* kit = nullptr;       // the UI language's kit texts; null: the literals
};

inline constexpr size_t kMaxLanguages = 10;
inline constexpr uint64_t kDefaultPollMs = 250;

struct Globals {
    std::atomic<const HostAbi*> host{ nullptr };
    std::atomic<std::shared_ptr<const State>> state;   // null: English, revision 0
    std::atomic<uint64_t> lastPollMs{ 0 };
    std::atomic<uint64_t> pollIntervalMs{ kDefaultPollMs };
    std::atomic<const Entry*> englishEntries{ nullptr };
    std::atomic<size_t> englishCount{ 0 };
    std::atomic<const Table*> english{ nullptr };
    std::atomic<uint32_t> fileReads{ 0 };
    std::atomic<uint32_t> publishes{ 0 };

    std::mutex mutex;   // everything below
    std::wstring directory;
    std::string directoryUtf8;
    std::vector<std::unique_ptr<LanguageSlot>> slots;     // this directory's languages
    std::vector<std::unique_ptr<LanguageSlot>> retired;   // earlier directories' (pointers stay valid)
    std::unique_ptr<Table> englishOwner;
    std::unordered_map<std::string, std::unique_ptr<std::string>, Hash, std::equal_to<>> missingIds;
    bool capLogged = false;
};

inline Globals& G() {
    static Globals g;
    return g;
}

inline void RegisterEnglish(const Entry* entries, size_t count) noexcept {
    Globals& g = G();
    const Entry* expected = nullptr;
    g.englishCount.store(count, std::memory_order_relaxed);
    g.englishEntries.compare_exchange_strong(expected, entries, std::memory_order_release);
}

}  // namespace Detail

// The generated header defines one per DLL:
//     inline const EmbeddedEnglish g_pluginLangEn{kPluginLangEn, std::size(kPluginLangEn)};
struct EmbeddedEnglish {
    EmbeddedEnglish(const Entry* entries, size_t count) noexcept {
        if (entries && count) Detail::RegisterEnglish(entries, count);
    }
};

namespace Detail {

inline bool HasUiLanguage(const HostAbi* host) {
    return host && host->size_bytes >= offsetof(HostAbi, get_ui_language) + sizeof(host->get_ui_language) &&
           host->get_ui_language;
}

inline bool HasHostText(const HostAbi* host) {
    return host && host->size_bytes >= offsetof(HostAbi, get_host_text) + sizeof(host->get_host_text) &&
           host->get_host_text;
}

// A program code as a file name: 1-15 of [A-Za-z0-9_-].
inline bool ValidCode(const char* code, size_t n) {
    if (n == 0 || n > 15) return false;
    for (size_t k = 0; k < n; ++k) {
        const char c = code[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

inline std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

inline std::unique_ptr<Table> MakeTable(const std::unordered_map<std::string, std::string>& texts) {
    auto table = std::make_unique<Table>();
    table->texts.reserve(texts.size());
    for (const auto& [key, text] : texts) table->texts.emplace(key, Text{ text, text + "###" + key });
    return table;
}

// The embedded English, built on first use from the registered entries.
inline const Table* EnglishTable() {
    Globals& g = G();
    if (const Table* t = g.english.load(std::memory_order_acquire)) return t;
    const Entry* entries = g.englishEntries.load(std::memory_order_acquire);
    const size_t count = g.englishCount.load(std::memory_order_relaxed);
    if (!entries) return nullptr;
    std::lock_guard<std::mutex> lock(g.mutex);
    if (const Table* t = g.english.load(std::memory_order_acquire)) return t;
    auto table = std::make_unique<Table>();
    table->texts.reserve(count);
    for (size_t k = 0; k < count; ++k) {
        if (!entries[k].key || !entries[k].text) continue;
        const std::string text(entries[k].text);
        table->texts.emplace(entries[k].key, Text{ text, text + "###" + entries[k].key });
    }
    g.englishOwner = std::move(table);
    g.english.store(g.englishOwner.get(), std::memory_order_release);
    return g.englishOwner.get();
}

inline void Log(const HostAbi* host, const std::string& message) {
    if (host && host->log.log) host->log.log("Warning", message.c_str());
}

// lang/<code>.json of the attached directory; null when absent or refused (logged once: the slot
// remembers the outcome). Called under G().mutex.
inline std::unique_ptr<Table> LoadLanguageFile(Globals& g, const HostAbi* host, const std::string& code) {
    if (g.directory.empty()) return nullptr;
    std::wstring path = g.directory;
    if (path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
    path += L"lang\\" + Widen(code) + L".json";
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;   // absent: this plugin has no such language
    g.fileReads.fetch_add(1, std::memory_order_relaxed);
    const std::string shown = g.directoryUtf8 + "\\lang\\" + code + ".json";
    LARGE_INTEGER size{};
    std::string bytes;
    bool readOk = ::GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0;
    if (readOk && static_cast<unsigned long long>(size.QuadPart) <= kMaxFileBytes) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        readOk = bytes.empty() ||
                 (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) != 0 &&
                  got == bytes.size());
    }
    ::CloseHandle(file);
    if (!readOk) {
        Log(host, "PluginI18n: " + shown + " could not be read; this plugin stays English in " + code + ".");
        return nullptr;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > kMaxFileBytes) {
        Log(host, "PluginI18n: " + shown + " refused (" + ParseErrorText(ParseError::TooLarge) +
                      "); this plugin stays English in " + code + ".");
        return nullptr;
    }
    std::unordered_map<std::string, std::string> texts;
    const ParseError error = ParseFlatJson(bytes, texts);
    if (error != ParseError::None) {
        Log(host, "PluginI18n: " + shown + " refused (" + ParseErrorText(error) + "); this plugin stays English in " +
                      code + ".");
        return nullptr;
    }
    return MakeTable(texts);
}

// True when the published state already follows `revision` (nothing to do).
inline bool Follows(const std::shared_ptr<const State>& state, uint64_t revision) {
    return (state ? state->revision : 0) == revision;
}

// Asks the host for its UI language; on a new revision publishes that language's table (loading
// its file the first time this DLL meets the language).
inline void Refresh() {
    Globals& g = G();
    const HostAbi* host = g.host.load(std::memory_order_acquire);
    if (!HasUiLanguage(host)) return;
    char code[32] = {};
    uint64_t revision = 0;
    const int32_t n = host->get_ui_language(code, static_cast<int32_t>(sizeof(code)), &revision);
    if (Follows(g.state.load(std::memory_order_acquire), revision)) return;
    const bool known = n > 0 && n < static_cast<int32_t>(sizeof(code)) && ValidCode(code, static_cast<size_t>(n));
    const std::string language = known ? std::string(code, static_cast<size_t>(n)) : std::string("en");

    std::lock_guard<std::mutex> lock(g.mutex);
    if (Follows(g.state.load(std::memory_order_acquire), revision)) return;   // another thread did it
    LanguageSlot* slot = nullptr;
    for (const auto& s : g.slots)
        if (s->code == language) slot = s.get();
    if (!slot && g.slots.size() < kMaxLanguages) {
        auto made = std::make_unique<LanguageSlot>();
        made->code = language;
        made->kit = std::make_unique<KitCache>();
        if (language != "en") made->table = LoadLanguageFile(g, host, language);
        g.slots.push_back(std::move(made));
        slot = g.slots.back().get();
    } else if (!slot && !g.capLogged) {
        g.capLogged = true;
        Log(host, "PluginI18n: more than ten UI languages in one session; " + language + " stays English.");
    }
    auto next = std::make_shared<State>();
    next->revision = revision;
    next->table = slot ? slot->table.get() : nullptr;
    next->language = next->table ? slot->code.c_str() : "en";
    next->kit = slot ? slot->kit.get() : nullptr;
    g.state.store(std::shared_ptr<const State>(std::move(next)), std::memory_order_release);
    g.publishes.fetch_add(1, std::memory_order_relaxed);
}

// Refresh at most once per poll interval, from any thread (one thread at a time).
inline void MaybeRefresh() {
    Globals& g = G();
    if (!g.host.load(std::memory_order_acquire)) return;
    const uint64_t now = ::GetTickCount64();
    uint64_t last = g.lastPollMs.load(std::memory_order_relaxed);
    const uint64_t interval = g.pollIntervalMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < interval) return;
    if (last == 0 && interval == UINT64_MAX) return;   // polling switched off (tests)
    if (!g.lastPollMs.compare_exchange_strong(last, now == 0 ? 1 : now, std::memory_order_relaxed)) return;
    Refresh();
}

// `text` with every {name} of `args` replaced (an absent argument leaves {name}; values are never
// scanned again).
inline std::string Format(const char* text, std::initializer_list<std::pair<std::string_view, std::string_view>> args) {
    std::string out;
    if (!text) return out;
    out.reserve(std::strlen(text) + 16);
    for (const char* p = text; *p;) {
        if (*p == '{') {
            const char* close = std::strchr(p + 1, '}');
            if (close) {
                const std::string_view name(p + 1, static_cast<size_t>(close - p - 1));
                const std::string_view* value = nullptr;
                for (const auto& arg : args)
                    if (arg.first == name) {
                        value = &arg.second;
                        break;
                    }
                if (value) {
                    out.append(value->data(), value->size());
                    p = close + 1;
                    continue;
                }
            }
        }
        out.push_back(*p++);
    }
    return out;
}

// Copies `text` into out[size] NUL-terminated, cut on a UTF-8 character boundary.
inline void CopyUtf8(std::string_view text, char* out, size_t size) {
    if (!out || size == 0) return;
    size_t n = text.size() < size - 1 ? text.size() : size - 1;
    if (n < text.size())
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

// Called by PluginSDK_AttachHost: the host table and the plugin's folder (UTF-8). A new folder
// starts over (the old tables stay alive for pointers already handed out).
inline void Attach(const HostAbi* abi, const char* directory) {
    Globals& g = G();
    EnglishTable();
    std::lock_guard<std::mutex> lock(g.mutex);
    const std::string utf8 = directory ? directory : "";
    if (utf8 != g.directoryUtf8 || g.directory.empty()) {
        for (auto& slot : g.slots) g.retired.push_back(std::move(slot));
        g.slots.clear();
        g.directoryUtf8 = utf8;
        g.directory = Widen(utf8);
        g.capLogged = false;
    }
    g.host.store(abi, std::memory_order_release);
    g.state.store(std::shared_ptr<const State>(), std::memory_order_release);
    g.lastPollMs.store(0, std::memory_order_relaxed);
}

// Called by PluginSDK_BindImGui (the GUI thread): follows the host's UI language.
inline void OnBind() { Refresh(); }

// Test hooks: file reads and state publications so far; the poll interval (UINT64_MAX = never).
struct Counters {
    uint32_t fileReads;
    uint32_t publishes;
};
inline Counters ReadCounters() {
    return { G().fileReads.load(std::memory_order_relaxed), G().publishes.load(std::memory_order_relaxed) };
}
inline void SetPollIntervalMs(uint64_t ms) { G().pollIntervalMs.store(ms, std::memory_order_relaxed); }

}  // namespace Detail

// ---------------------------------------------------------------------------------------------
// The API
// ---------------------------------------------------------------------------------------------

// A key written where it is not looked up yet (a static table of keys, an argument passed on):
// the key itself, at compile time. It marks the literal for tools/plugin_i18n/gen.py --gate, which
// checks that every Tr / TrF / TrId / Key literal is a key of lang/en.json.
constexpr const char* Key(const char* key) { return key; }

// The text of `key` in the program language: the language's file, else the embedded English,
// else the key itself. The pointer stays valid for the DLL's lifetime.
inline const char* Tr(const char* key) {
    if (!key) return "";
    Detail::MaybeRefresh();
    const std::shared_ptr<const Detail::State> state = Detail::G().state.load(std::memory_order_acquire);
    if (state && state->table) {
        const auto it = state->table->texts.find(std::string_view(key));
        if (it != state->table->texts.end()) return it->second.text.c_str();
    }
    if (const Detail::Table* english = Detail::EnglishTable()) {
        const auto it = english->texts.find(std::string_view(key));
        if (it != english->texts.end()) return it->second.text.c_str();
    }
    return key;
}

// Tr(key) with its {name} placeholders filled: TrF("loot_count", {{"count", "3"}}). An argument the
// text does not name is ignored; a placeholder without an argument stays "{name}".
inline std::string TrF(const char* key, std::initializer_list<std::pair<std::string_view, std::string_view>> args) {
    return Detail::Format(Tr(key), args);
}

// "<text>###<key>": the label of a widget whose ImGui id must not change with the language.
inline const char* TrId(const char* key) {
    if (!key) return "";
    Detail::MaybeRefresh();
    const std::shared_ptr<const Detail::State> state = Detail::G().state.load(std::memory_order_acquire);
    if (state && state->table) {
        const auto it = state->table->texts.find(std::string_view(key));
        if (it != state->table->texts.end()) return it->second.id.c_str();
    }
    if (const Detail::Table* english = Detail::EnglishTable()) {
        const auto it = english->texts.find(std::string_view(key));
        if (it != english->texts.end()) return it->second.id.c_str();
    }
    Detail::Globals& g = Detail::G();
    std::lock_guard<std::mutex> lock(g.mutex);
    auto it = g.missingIds.find(std::string_view(key));
    if (it == g.missingIds.end())
        it = g.missingIds.emplace(key, std::make_unique<std::string>(std::string(key) + "###" + key)).first;
    return it->second->c_str();
}

// The language Tr serves: a program code ("ru", "pt_BR", ...), "en" when this plugin has no file
// for the UI language, before the host told it, or on an older host.
inline const char* Language() {
    Detail::MaybeRefresh();
    const std::shared_ptr<const Detail::State> state = Detail::G().state.load(std::memory_order_acquire);
    return state ? state->language : "en";
}

// Moves whenever the host loads a UI language (0 before that, or on an older host): rebuild what
// you cached from Tr when it moves.
inline uint64_t Revision() {
    Detail::MaybeRefresh();
    const std::shared_ptr<const Detail::State> state = Detail::G().state.load(std::memory_order_acquire);
    return state ? state->revision : 0;
}

// The host's own text for a kit key ("plugin_kit_yes": PluginUi.h's shared labels) in the UI
// language, fetched once per language and kept; `englishLiteral` whenever the host has none (an
// older host, an unknown key, not attached).
inline const char* KitText(const char* kitKey, const char* englishLiteral) {
    const char* literal = englishLiteral ? englishLiteral : "";
    if (!kitKey) return literal;
    Detail::MaybeRefresh();
    Detail::Globals& g = Detail::G();
    const HostAbi* host = g.host.load(std::memory_order_acquire);
    if (!Detail::HasHostText(host) || !Detail::HasUiLanguage(host)) return literal;
    const std::shared_ptr<const Detail::State> state = g.state.load(std::memory_order_acquire);
    if (!state || !state->kit) return literal;
    std::lock_guard<std::mutex> lock(g.mutex);
    auto& texts = state->kit->texts;
    const auto it = texts.find(std::string_view(kitKey));
    if (it != texts.end()) return it->second ? it->second->c_str() : literal;
    char buffer[512];
    buffer[0] = '\0';
    const int32_t n = host->get_host_text(kitKey, buffer, static_cast<int32_t>(sizeof(buffer)));
    // The host answers in ITS current language: keep the answer only while that is still the
    // language this state follows (the next refresh catches up otherwise).
    char code[32] = {};
    uint64_t revision = 0;
    host->get_ui_language(code, static_cast<int32_t>(sizeof(code)), &revision);
    if (revision != state->revision) return literal;
    if (n <= 0 || n >= static_cast<int32_t>(sizeof(buffer))) {
        texts.emplace(kitKey, nullptr);
        return literal;
    }
    const auto stored = texts.emplace(kitKey, std::make_unique<std::string>(buffer, static_cast<size_t>(n))).first;
    return stored->second->c_str();
}

}  // namespace PluginSDK::I18n

#endif  // POEFIXER_PLUGIN_I18N_H
