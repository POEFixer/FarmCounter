/*
 * Local override: static linking — no dllimport/dllexport needed.
 */
#ifndef SQLITE_API
#define SQLITE_API
#endif

#define SQLITE_ENABLE_UNLOCK_NOTIFY 1
#define SQLITE_OS_WIN 1
#define SQLITE_ENABLE_COLUMN_METADATA 1
