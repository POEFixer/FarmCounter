#pragma once
// FarmDb.h — SQLite persistence for FarmCounter statistics (map runs + their
// loot + session meta). The sqlite3 amalgamation (lib/sqlite3.c) is compiled
// into the plugin DLL. Mutations are atomic and report failure without changing
// caller-owned records; errors never turn a failed insert into an existing ID.
// The legacy map_history.txt is NOT
// migrated (by request) — a fresh data/farmstats.db starts empty.
#include "FarmTypes.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;   // forward declare — sqlite3.h included only in the .cpp

class FarmDb {
public:
    FarmDb() = default;
    ~FarmDb() { Close(); }
    FarmDb(const FarmDb&) = delete;
    FarmDb& operator=(const FarmDb&) = delete;

    bool Open(const std::filesystem::path& pluginDir);   // <dir>/data/farmstats.db
    void Close();
    bool IsOpen() const { return m_db != nullptr; }
    const std::string& LastError() const { return m_lastError; }

    // Loads every run (oldest first, loot attached) + session meta. sessionIdMax
    // is raised to the highest session id seen (high-water mark, never lowered).
    // Outputs are replaced only after a complete successful read.
    bool LoadAll(std::vector<MapRun>& runs, int& sessionActiveSec, int& sessionIdMax);

    // Inserts a fresh live run (stamps run.dbId + run.startedAt/startedText).
    bool InsertRun(MapRun& run);
    // Rewrites the run row and REPLACES its loot rows (loot lists are tiny).
    bool UpdateRun(const MapRun& run);
    bool DeleteRun(int64_t dbId);
    bool DeleteSession(int sessionId);                    // archived runs of one session
    bool DeleteAllArchived();
    // "New Session": stamps archived=1 + the given session id on all active rows.
    bool ArchiveActiveRuns(int sessionId);

    // Session meta (current session id + accumulated active seconds).
    bool SaveMeta(int sessionActiveSec, int currentSessionId);

    // A zero-ID live run is inserted; otherwise it is updated. IDs/timestamps
    // are published only after the run, loot and metadata commit together.
    bool SaveState(MapRun* liveRun, int sessionActiveSec, int currentSessionId);
    // Save the old live run, archive active rows, reset metadata, and optionally
    // insert a fresh live row in one transaction. A failed reset changes nothing.
    bool BeginNewSession(MapRun* oldLiveRun, int archivedSessionId, MapRun* freshRun);

private:
    template<class Action> bool Execute(Action&& action, bool write = true);
    void CreateTables();
    void InsertRunInner(MapRun& run);
    void UpdateRunInner(const MapRun& run);
    void ReplaceLoot(int64_t runId, const std::vector<LootEntry>& loot);
    void SaveMetaInner(int sessionActiveSec, int currentSessionId);
    void ArchiveActiveRunsInner(int sessionId);

    sqlite3* m_db = nullptr;
    std::string m_lastError;
};
