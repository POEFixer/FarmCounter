#include "FarmDb.h"
#include "sqlite3.h"
#include <Windows.h>
#include <ctime>
#include <cstdio>
#include <string>

// ── small statement helper (SekhemaHelper RunDatabase pattern) ────────────────
namespace {
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (db && sqlite3_prepare_v2(db, sql, -1, &m_st, nullptr) != SQLITE_OK)
            m_st = nullptr;
    }
    ~Stmt() { if (m_st) sqlite3_finalize(m_st); }
    Stmt(const Stmt&) = delete; Stmt& operator=(const Stmt&) = delete;

    bool valid() const { return m_st != nullptr; }
    void BindI64(int i, int64_t v)             { if (m_st) sqlite3_bind_int64(m_st, i, v); }
    void BindInt(int i, int v)                 { if (m_st) sqlite3_bind_int(m_st, i, v); }
    void BindReal(int i, double v)             { if (m_st) sqlite3_bind_double(m_st, i, v); }
    void BindText(int i, const std::string& s) { if (m_st) sqlite3_bind_text(m_st, i, s.c_str(), -1, SQLITE_TRANSIENT); }
    bool Step()                                { return m_st && sqlite3_step(m_st) == SQLITE_ROW; }
    void Run()                                 { if (m_st) sqlite3_step(m_st); }

    int64_t     I64(int c)  const { return sqlite3_column_int64(m_st, c); }
    int         Int(int c)  const { return sqlite3_column_int(m_st, c); }
    double      Real(int c) const { return sqlite3_column_double(m_st, c); }
    std::string Text(int c) const {
        const unsigned char* t = sqlite3_column_text(m_st, c);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
private:
    sqlite3_stmt* m_st = nullptr;
};

std::string FormatLocalTime(int64_t unixSec) {
    std::tm tmv{};
    time_t t = (time_t)unixSec;
    localtime_s(&tmv, &t);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min);
    return buf;
}
} // namespace

bool FarmDb::Open(const std::filesystem::path& pluginDir) {
    namespace fs = std::filesystem;
    fs::path dataDir = pluginDir / "data";
    std::error_code ec;
    if (!fs::exists(dataDir, ec)) fs::create_directories(dataDir, ec);

    fs::path dbPath = dataDir / "farmstats.db";
    // sqlite3_open expects UTF-8 on Windows; build it from the wide form
    // (path::string() would use the ANSI codepage — KillCount precedent).
    std::wstring wide = dbPath.wstring();
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          utf8.data(), needed, nullptr, nullptr);

    if (sqlite3_open(utf8.c_str(), &m_db) != SQLITE_OK) {
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        return false;
    }
    Exec("PRAGMA foreign_keys = ON;");
    CreateTables();
    return true;
}

void FarmDb::Close() {
    if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
}

void FarmDb::Exec(const char* sql) {
    if (!m_db) return;
    char* err = nullptr;
    sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
}

void FarmDb::CreateTables() {
    Exec(R"(
        CREATE TABLE IF NOT EXISTS runs (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          map_name TEXT NOT NULL,
          started_at INTEGER NOT NULL,
          started_text TEXT NOT NULL DEFAULT '',
          duration_sec INTEGER NOT NULL DEFAULT 0,
          total_chaos REAL NOT NULL DEFAULT 0,
          exalted_rate REAL NOT NULL DEFAULT 1,
          hiveblood_gain INTEGER NOT NULL DEFAULT 0,
          beacon_gain INTEGER NOT NULL DEFAULT 0,
          kills_normal INTEGER NOT NULL DEFAULT 0,
          kills_magic INTEGER NOT NULL DEFAULT 0,
          kills_rare INTEGER NOT NULL DEFAULT 0,
          kills_unique INTEGER NOT NULL DEFAULT 0,
          session_id INTEGER NOT NULL DEFAULT 0,
          archived INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS loot (
          run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
          name TEXT NOT NULL,
          stack INTEGER NOT NULL DEFAULT 0,
          chaos_each REAL NOT NULL DEFAULT 0,
          rarity INTEGER NOT NULL DEFAULT 0,
          icon_path TEXT NOT NULL DEFAULT '',
          PRIMARY KEY (run_id, name));
        CREATE TABLE IF NOT EXISTS meta (
          key TEXT PRIMARY KEY,
          value TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS idx_runs_session ON runs(archived, session_id);
        CREATE INDEX IF NOT EXISTS idx_loot_run ON loot(run_id);
    )");
}

void FarmDb::LoadAll(std::vector<MapRun>& runs, int& sessionActiveSec, int& sessionIdMax) {
    runs.clear();
    if (!m_db) return;

    Stmt s(m_db, "SELECT id, map_name, started_at, started_text, duration_sec, total_chaos, "
                 "exalted_rate, hiveblood_gain, beacon_gain, kills_normal, kills_magic, "
                 "kills_rare, kills_unique, session_id, archived "
                 "FROM runs ORDER BY started_at, id;");
    while (s.Step()) {
        MapRun r;
        r.dbId          = s.I64(0);
        r.mapName       = s.Text(1);
        r.startedAt     = s.I64(2);
        r.startedText   = s.Text(3);
        r.durationSec   = s.Int(4);
        r.totalChaos    = (float)s.Real(5);
        r.exaltedRate   = (float)s.Real(6);
        r.hivebloodGain = s.Int(7);
        r.beaconGain    = s.Int(8);
        r.killsNormal   = s.Int(9);
        r.killsMagic    = s.Int(10);
        r.killsRare     = s.Int(11);
        r.killsUnique   = s.Int(12);
        r.sessionId     = s.Int(13);
        r.archived      = s.Int(14) != 0;
        runs.push_back(std::move(r));
    }
    // Attach loot per run (run counts are small; a query per run is fine here).
    for (auto& r : runs) {
        Stmt l(m_db, "SELECT name, stack, chaos_each, rarity, icon_path "
                     "FROM loot WHERE run_id=?1 ORDER BY chaos_each*stack DESC;");
        l.BindI64(1, r.dbId);
        while (l.Step()) {
            LootEntry e;
            e.name       = l.Text(0);
            e.stackCount = l.Int(1);
            e.chaosEach  = (float)l.Real(2);
            e.rarity     = l.Int(3);
            e.iconPath   = l.Text(4);
            r.loot.push_back(std::move(e));
        }
        if (r.sessionId > sessionIdMax) sessionIdMax = r.sessionId;
    }
    // Session meta.
    {
        Stmt m(m_db, "SELECT key, value FROM meta;");
        while (m.Step()) {
            const std::string key = m.Text(0);
            const std::string val = m.Text(1);
            try {
                if (key == "session_active_sec") sessionActiveSec = std::stoi(val);
                else if (key == "current_session_id") {
                    int sid = std::stoi(val);
                    if (sid > sessionIdMax) sessionIdMax = sid;
                }
            } catch (...) {}
        }
    }
}

void FarmDb::InsertRun(MapRun& run) {
    if (!m_db) return;
    if (run.startedAt == 0) run.startedAt = (int64_t)time(nullptr);
    if (run.startedText.empty()) run.startedText = FormatLocalTime(run.startedAt);
    Stmt s(m_db, "INSERT INTO runs (map_name, started_at, started_text, duration_sec, "
                 "total_chaos, exalted_rate, hiveblood_gain, beacon_gain, kills_normal, "
                 "kills_magic, kills_rare, kills_unique, session_id, archived) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);");
    s.BindText(1, run.mapName);
    s.BindI64 (2, run.startedAt);
    s.BindText(3, run.startedText);
    s.BindInt (4, run.durationSec);
    s.BindReal(5, run.totalChaos);
    s.BindReal(6, run.exaltedRate);
    s.BindInt (7, run.hivebloodGain);
    s.BindInt (8, run.beaconGain);
    s.BindInt (9, run.killsNormal);
    s.BindInt(10, run.killsMagic);
    s.BindInt(11, run.killsRare);
    s.BindInt(12, run.killsUnique);
    s.BindInt(13, run.sessionId);
    s.BindInt(14, run.archived ? 1 : 0);
    s.Run();
    run.dbId = sqlite3_last_insert_rowid(m_db);
}

void FarmDb::ReplaceLoot(int64_t runId, const std::vector<LootEntry>& loot) {
    { Stmt d(m_db, "DELETE FROM loot WHERE run_id=?1;"); d.BindI64(1, runId); d.Run(); }
    for (const auto& e : loot) {
        Stmt s(m_db, "INSERT OR REPLACE INTO loot (run_id, name, stack, chaos_each, rarity, icon_path) "
                     "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
        s.BindI64 (1, runId);
        s.BindText(2, e.name);
        s.BindInt (3, e.stackCount);
        s.BindReal(4, e.chaosEach);
        s.BindInt (5, e.rarity);
        s.BindText(6, e.iconPath);
        s.Run();
    }
}

void FarmDb::UpdateRun(const MapRun& run) {
    if (!m_db || run.dbId == 0) return;
    Exec("BEGIN;");
    Stmt s(m_db, "UPDATE runs SET map_name=?2, duration_sec=?3, total_chaos=?4, exalted_rate=?5, "
                 "hiveblood_gain=?6, beacon_gain=?7, kills_normal=?8, kills_magic=?9, "
                 "kills_rare=?10, kills_unique=?11, session_id=?12, archived=?13 WHERE id=?1;");
    s.BindI64 (1, run.dbId);
    s.BindText(2, run.mapName);
    s.BindInt (3, run.durationSec);
    s.BindReal(4, run.totalChaos);
    s.BindReal(5, run.exaltedRate);
    s.BindInt (6, run.hivebloodGain);
    s.BindInt (7, run.beaconGain);
    s.BindInt (8, run.killsNormal);
    s.BindInt (9, run.killsMagic);
    s.BindInt(10, run.killsRare);
    s.BindInt(11, run.killsUnique);
    s.BindInt(12, run.sessionId);
    s.BindInt(13, run.archived ? 1 : 0);
    s.Run();
    ReplaceLoot(run.dbId, run.loot);
    Exec("COMMIT;");
}

void FarmDb::DeleteRun(int64_t dbId) {
    if (!m_db || dbId == 0) return;
    // Explicit child delete — independent of the foreign_keys pragma state.
    { Stmt s(m_db, "DELETE FROM loot WHERE run_id=?1;"); s.BindI64(1, dbId); s.Run(); }
    { Stmt s(m_db, "DELETE FROM runs WHERE id=?1;");     s.BindI64(1, dbId); s.Run(); }
}

void FarmDb::DeleteSession(int sessionId) {
    if (!m_db) return;
    { Stmt s(m_db, "DELETE FROM loot WHERE run_id IN "
                   "(SELECT id FROM runs WHERE archived=1 AND session_id=?1);");
      s.BindInt(1, sessionId); s.Run(); }
    { Stmt s(m_db, "DELETE FROM runs WHERE archived=1 AND session_id=?1;");
      s.BindInt(1, sessionId); s.Run(); }
}

void FarmDb::DeleteAllArchived() {
    if (!m_db) return;
    Exec("DELETE FROM loot WHERE run_id IN (SELECT id FROM runs WHERE archived=1);"
         "DELETE FROM runs WHERE archived=1;");
}

void FarmDb::ArchiveActiveRuns(int sessionId) {
    if (!m_db) return;
    Stmt s(m_db, "UPDATE runs SET archived=1, session_id=?1 WHERE archived=0;");
    s.BindInt(1, sessionId);
    s.Run();
}

void FarmDb::SaveMeta(int sessionActiveSec, int currentSessionId) {
    if (!m_db) return;
    { Stmt s(m_db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('session_active_sec', ?1);");
      s.BindText(1, std::to_string(sessionActiveSec)); s.Run(); }
    { Stmt s(m_db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('current_session_id', ?1);");
      s.BindText(1, std::to_string(currentSessionId)); s.Run(); }
}
