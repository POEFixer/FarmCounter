#pragma once
// FarmDb.h — SQLite persistence for FarmCounter statistics (map runs + their
// loot + session meta). Follows the SekhemaHelper RunDatabase pattern: the
// sqlite3 amalgamation (lib/sqlite3.c) is compiled into the plugin DLL, every
// method no-ops when the DB failed to open, and the schema is created with
// CREATE TABLE IF NOT EXISTS on Open(). The legacy map_history.txt is NOT
// migrated (by request) — a fresh data/farmstats.db starts empty.
#include "FarmTypes.h"
#include <cstdint>
#include <filesystem>
#include <vector>

struct sqlite3;   // forward declare — sqlite3.h included only in the .cpp

class FarmDb {
public:
    ~FarmDb() { Close(); }

    bool Open(const std::filesystem::path& pluginDir);   // <dir>/data/farmstats.db
    void Close();
    bool IsOpen() const { return m_db != nullptr; }

    // Loads every run (oldest first, loot attached) + session meta. sessionIdMax
    // is raised to the highest session id seen (high-water mark, never lowered).
    void LoadAll(std::vector<MapRun>& runs, int& sessionActiveSec, int& sessionIdMax);

    // Inserts a fresh live run (stamps run.dbId + run.startedAt/startedText).
    void InsertRun(MapRun& run);
    // Rewrites the run row and REPLACES its loot rows (loot lists are tiny).
    void UpdateRun(const MapRun& run);
    void DeleteRun(int64_t dbId);
    void DeleteSession(int sessionId);                    // archived runs of one session
    void DeleteAllArchived();
    // "New Session": stamps archived=1 + the given session id on all active rows.
    void ArchiveActiveRuns(int sessionId);

    // Session meta (current session id + accumulated active seconds).
    void SaveMeta(int sessionActiveSec, int currentSessionId);

private:
    void Exec(const char* sql);
    void CreateTables();
    void ReplaceLoot(int64_t runId, const std::vector<LootEntry>& loot);

    sqlite3* m_db = nullptr;
};
