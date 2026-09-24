#pragma once

// PlatformCompat.h — the small handful of CRT/Win32 spellings that the shared
// core sources use and POSIX names differently. Windows includes this and gets
// nothing: every definition below is inside the POSIX branch, so the Windows
// build is unchanged down to the preprocessor.
//
// WHY A HEADER RATHER THAN AN #ifdef AT EACH SITE. `localtime_s(&tm, &t)` and
// `localtime_r(&t, &tm)` take THE SAME TWO ARGUMENTS IN THE OPPOSITE ORDER, and
// both compile if you swap them — the result is a timestamp built from garbage,
// which looks like a formatting bug and is found late. Writing that translation
// once, here, means there is exactly one place it can be wrong, and it is a
// place whose whole purpose is to be right.

#ifndef _WIN32

#include <ctime>
#include <cstring>

// Windows argument order, POSIX implementation. Returns 0 on success like the
// CRT one does (localtime_r returns the pointer, or null on failure).
inline int localtime_s(std::tm* tmDest, const std::time_t* src) {
    if (tmDest == nullptr || src == nullptr) return -1;
    return ::localtime_r(src, tmDest) == nullptr ? -1 : 0;
}

// A wipe the optimiser is not allowed to delete. glibc's explicit_bzero carries
// that guarantee; the volatile loop is the fallback for anything that lacks it.
inline void SecureZeroMemory(void* p, std::size_t n) {
    if (p == nullptr || n == 0) return;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    ::explicit_bzero(p, n);
#else
    auto* volatile q = static_cast<volatile unsigned char*>(p);
    while (n--) *q++ = 0;
#endif
}

#endif  // !_WIN32
