#pragma once
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cstdio>
#include <unistd.h>
#include "PlatformCompat.h"
#endif

namespace FileIO {
    // Unicode-safe file open — works with any characters in the path
    // (Russian, Chinese, Arabic, special chars, etc.)
    //
    // On Windows, std::ifstream(std::string) uses ANSI encoding and FAILS
    // for non-ASCII paths. std::ifstream(fs::path) uses wchar_t internally
    // and handles all Unicode correctly.

    inline std::ifstream OpenRead(const std::string& path,
                                  std::ios_base::openmode mode = std::ios_base::in) {
        return std::ifstream(std::filesystem::path(path), mode);
    }

    inline std::ofstream OpenWrite(const std::string& path,
                                   std::ios_base::openmode mode = std::ios_base::out) {
        return std::ofstream(std::filesystem::path(path), mode);
    }

    // For C-style fopen (used by stb_image, crash reports, etc.)
    inline FILE* FOpen(const std::string& path, const char* mode) {
#ifdef _WIN32
        std::filesystem::path fsPath(path);
        // Convert mode to wide string for _wfopen
        std::wstring wmode;
        while (*mode) { wmode += static_cast<wchar_t>(*mode++); }
        return _wfopen(fsPath.c_str(), wmode.c_str());
#else
        // POSIX paths are byte strings and the encoding is already UTF-8, so the
        // whole reason the wide detour exists on Windows does not arise here.
        return std::fopen(path.c_str(), mode);
#endif
    }

    // Write content atomically via tmp file + FlushFileBuffers + rename.
    // Returns false on failure. On NTFS within the same volume, rename is
    // atomic (metadata-level); FlushFileBuffers forces file data to disk
    // before rename to survive power-loss. Caller is responsible for
    // directory existence.
    inline bool AtomicWrite(const std::filesystem::path& dst, std::string_view content) {
        std::filesystem::path tmp = dst;
#ifdef _WIN32
        tmp += L".tmp";
#else
        tmp += ".tmp";
#endif
        {
            std::ofstream out(tmp, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
            if (!out.is_open()) return false;
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.flush();
            if (!out) { std::error_code ignore; std::filesystem::remove(tmp, ignore); return false; }
        }
        // Force data to disk before renaming — guards against power-loss producing
        // a renamed-but-zero-length file. Process-kill without this is usually safe
        // because the OS flushes on termination, but power-loss is not.
        {
#ifdef _WIN32
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(h);
                CloseHandle(h);
            }
            // If CreateFileW fails (rare — we just wrote the file), skip the flush.
            // Best-effort durability; rename below is still atomic metadata-wise.
#else
            if (FILE* f = std::fopen(tmp.c_str(), "rb")) {
                ::fsync(::fileno(f));
                std::fclose(f);
            }
#endif
        }
        std::error_code ec;
        std::filesystem::rename(tmp, dst, ec);
        if (ec) { std::error_code ignore; std::filesystem::remove(tmp, ignore); return false; }
        return true;
    }

    // AtomicWrite variant that additionally preserves the file being replaced
    // as <dst>.bak (previous good version). Used for config files so a corrupt
    // main file can be recovered from the backup on the next load.
    // dst existing → ReplaceFileW (atomic swap, old content moved to .bak).
    // dst missing  → plain atomic rename (no .bak yet).
    inline bool AtomicWriteBackup(const std::filesystem::path& dst, std::string_view content) {
        std::filesystem::path tmp = dst;
#ifdef _WIN32
        tmp += L".tmp";
#else
        tmp += ".tmp";
#endif
        {
            std::ofstream out(tmp, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
            if (!out.is_open()) return false;
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.flush();
            if (!out) { std::error_code ignore; std::filesystem::remove(tmp, ignore); return false; }
        }
        {
#ifdef _WIN32
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(h);
                CloseHandle(h);
            }
#else
            if (FILE* f = std::fopen(tmp.c_str(), "rb")) {
                ::fsync(::fileno(f));
                std::fclose(f);
            }
#endif
        }
        std::error_code ec;
        if (std::filesystem::exists(dst, ec)) {
            std::filesystem::path bak = dst;
#ifdef _WIN32
            bak += L".bak";
            if (ReplaceFileW(dst.c_str(), tmp.c_str(), bak.c_str(),
                             REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
                return true;
            }
            // ReplaceFileW can fail under AV interference or exotic filesystems —
            // fall back to a plain replace (loses this round's .bak, keeps the data).
            if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return true;
            }
            std::error_code ignore;
            std::filesystem::remove(tmp, ignore);
            return false;
#else
            bak += ".bak";
            // COPY the outgoing version aside rather than renaming it there. A rename
            // would leave `dst` missing for an instant, and a reader that looked in
            // that instant would see "first run" and start from defaults — which is
            // the exact failure the .bak exists to prevent. The copy costs one file
            // write on a config-sized file.
            std::error_code cec;
            std::filesystem::copy_file(dst, bak,
                                       std::filesystem::copy_options::overwrite_existing, cec);
            std::filesystem::rename(tmp, dst, ec);
            if (ec) { std::error_code ignore; std::filesystem::remove(tmp, ignore); return false; }
            return true;
#endif
        }
        std::filesystem::rename(tmp, dst, ec);
        if (ec) { std::error_code ignore; std::filesystem::remove(tmp, ignore); return false; }
        return true;
    }
}
