#include "FarmDb.h"
#include "sqlite3.h"
#include <Windows.h>
#include <ctime>
#include <cstdio>
#include <string>
#include <map>
#include <stdexcept>
#include <charconv>

// ── small statement helper (SekhemaHelper RunDatabase pattern) ────────────────
namespace {
void CheckSql(sqlite3* db, int code) {
    if (code != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
void ExecSql(sqlite3* db, const char* sql) {
    CheckSql(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
// A failed statement must stop the whole mutation. In particular SQLITE_FULL
// may roll a transaction back itself; never continue later writes in autocommit.
class Transaction {
public:
    Transaction(sqlite3* db, bool write) : m_db(db) {
        ExecSql(db, write ? "BEGIN IMMEDIATE;" : "BEGIN;");
    }
    ~Transaction() {
        if (!m_committed && !sqlite3_get_autocommit(m_db))
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    void Commit() { ExecSql(m_db, "COMMIT;"); m_committed = true; }
private:
    sqlite3* m_db;
    bool m_committed = false;
};
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : m_db(db) {
        CheckSql(db, sqlite3_prepare_v2(db, sql, -1, &m_st, nullptr));
    }
    ~Stmt() { if (m_st) sqlite3_finalize(m_st); }
    Stmt(const Stmt&) = delete; Stmt& operator=(const Stmt&) = delete;

    void BindI64(int i, int64_t v) { CheckSql(m_db, sqlite3_bind_int64(m_st, i, v)); }
    void BindInt(int i, int v) { CheckSql(m_db, sqlite3_bind_int(m_st, i, v)); }
    void BindReal(int i, double v) { CheckSql(m_db, sqlite3_bind_double(m_st, i, v)); }
    void BindText(int i, const std::string& s) {
        CheckSql(m_db, sqlite3_bind_text64(m_st, i, s.data(), s.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    }
    bool Step() {
        const int code = sqlite3_step(m_st);
        if (code == SQLITE_ROW) return true;
        if (code != SQLITE_DONE) CheckSql(m_db, code);
        return false;
    }
    void Run() {
        const int code = sqlite3_step(m_st);
        if (code != SQLITE_DONE) CheckSql(m_db, code);
    }

    int64_t     I64(int c)  const { return sqlite3_column_int64(m_st, c); }
    int         Int(int c)  const { return sqlite3_column_int(m_st, c); }
    double      Real(int c) const { return sqlite3_column_double(m_st, c); }
    std::string Text(int c) const {
        const unsigned char* t = sqlite3_column_text(m_st, c);
        return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(m_st, c)) : "";
    }
private:
    sqlite3_stmt* m_st = nullptr;
    sqlite3* m_db;
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
struct ColumnInfo { std::string type; int primaryKey = 0; };
std::map<std::string, ColumnInfo> Columns(sqlite3* db, const char* pragma) {
    std::map<std::string, ColumnInfo> columns;
    Stmt statement(db, pragma);
    while (statement.Step()) columns.emplace(statement.Text(1), ColumnInfo{statement.Text(2), statement.Int(5)});
    return columns;
}
void ValidateColumns(const std::map<std::string, ColumnInfo>& actual,
    std::initializer_list<std::pair<const char*, const char*>> expected,
    std::initializer_list<std::pair<const char*, int>> keys) {
    for (const auto& [name, type] : expected) {
        const auto found = actual.find(name);
        if (found == actual.end() || found->second.type != type)
            throw std::runtime_error(std::string("Unsupported farm database column: ") + name);
    }
    for (const auto& [name, position] : keys)
        if (actual.at(name).primaryKey != position)
            throw std::runtime_error(std::string("Unsupported farm database primary key: ") + name);
}
int ReadMetaInteger(const std::string& value) {
    int number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || number < 0)
        throw std::runtime_error("Invalid farm session metadata");
    return number;
}
} // namespace

bool FarmDb::Open(const std::filesystem::path& pluginDir) {
    Close();
    m_lastError.clear();
    try {
        namespace fs = std::filesystem;
        fs::path dataDir = pluginDir / "data";
        std::error_code ec;
        fs::create_directories(dataDir, ec);
        if (ec) throw std::runtime_error("Cannot create farm data directory: " + ec.message());

        fs::path dbPath = dataDir / "farmstats.db";
        // sqlite3_open expects UTF-8 on Windows; build it from the wide form
        // (path::string() would use the ANSI codepage — KillCount precedent).
        std::wstring wide = dbPath.wstring();
        int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
        if (needed <= 0) throw std::runtime_error("Cannot encode farm database path");
        std::string utf8(static_cast<size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                              utf8.data(), needed, nullptr, nullptr);

        const int opened = sqlite3_open(utf8.c_str(), &m_db);
        CheckSql(m_db, opened);
        // No busy timeout: these calls run on the render thread and must fail fast.
        ExecSql(m_db, "PRAGMA foreign_keys = ON;");
        { Stmt pragma(m_db, "PRAGMA foreign_keys;");
          if (!pragma.Step() || pragma.Int(0) != 1) throw std::runtime_error("Cannot enable farm database foreign keys"); }
        Transaction transaction(m_db, true);
        CreateTables();
        transaction.Commit();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
    } catch (...) {
        m_lastError = "Cannot open farm database";
    }
    Close();
    return false;
}

void FarmDb::Close() {
    if (m_db) { sqlite3_close_v2(m_db); m_db = nullptr; }
}

template<class Action> bool FarmDb::Execute(Action&& action, bool write) {
    m_lastError.clear();
    if (!m_db) { m_lastError = "Farm database is not open"; return false; }
    try {
        Transaction transaction(m_db, write);
        action();
        transaction.Commit();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
    } catch (...) {
        m_lastError = "Farm database operation failed";
    }
    return false;
}

void FarmDb::CreateTables() {
    ExecSql(m_db, R"(
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
          gold_gain INTEGER NOT NULL DEFAULT 0,
          kills_normal INTEGER NOT NULL DEFAULT 0,
          kills_magic INTEGER NOT NULL DEFAULT 0,
          kills_rare INTEGER NOT NULL DEFAULT 0,
          kills_unique INTEGER NOT NULL DEFAULT 0,
          kills_rogue INTEGER NOT NULL DEFAULT 0,
          session_id INTEGER NOT NULL DEFAULT 0,
          archived INTEGER NOT NULL DEFAULT 0,
          map_mods TEXT NOT NULL DEFAULT '');
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
    // Check which known migration is needed. A duplicate-column exception must
    // not hide a real corruption, permission, or incompatible-schema error.
    const auto before = Columns(m_db, "PRAGMA table_info(runs);");
    if (!before.contains("gold_gain"))
        ExecSql(m_db, "ALTER TABLE runs ADD COLUMN gold_gain INTEGER NOT NULL DEFAULT 0;"); // v54
    if (!before.contains("map_mods"))
        ExecSql(m_db, "ALTER TABLE runs ADD COLUMN map_mods TEXT NOT NULL DEFAULT '';"); // v55
    if (!before.contains("kills_rogue"))
        ExecSql(m_db, "ALTER TABLE runs ADD COLUMN kills_rogue INTEGER NOT NULL DEFAULT 0;"); // v56
    ValidateColumns(Columns(m_db, "PRAGMA table_info(runs);"), {
        {"id", "INTEGER"}, {"map_name", "TEXT"}, {"started_at", "INTEGER"}, {"started_text", "TEXT"},
        {"duration_sec", "INTEGER"}, {"total_chaos", "REAL"}, {"exalted_rate", "REAL"},
        {"hiveblood_gain", "INTEGER"}, {"beacon_gain", "INTEGER"}, {"gold_gain", "INTEGER"},
        {"kills_normal", "INTEGER"}, {"kills_magic", "INTEGER"}, {"kills_rare", "INTEGER"},
        {"kills_unique", "INTEGER"}, {"kills_rogue", "INTEGER"}, {"session_id", "INTEGER"},
        {"archived", "INTEGER"}, {"map_mods", "TEXT"}}, {{"id", 1}});
    ValidateColumns(Columns(m_db, "PRAGMA table_info(loot);"), {
        {"run_id", "INTEGER"}, {"name", "TEXT"}, {"stack", "INTEGER"}, {"chaos_each", "REAL"},
        {"rarity", "INTEGER"}, {"icon_path", "TEXT"}}, {{"run_id", 1}, {"name", 2}});
    ValidateColumns(Columns(m_db, "PRAGMA table_info(meta);"), {
        {"key", "TEXT"}, {"value", "TEXT"}}, {{"key", 1}});
}

// Join / split the map-mod lines for the single map_mods TEXT column ('\n' sep;
// the rendered lines never contain a bare newline of their own — combined lines
// are stored as separate entries by the core renderer).
static std::string JoinMods(const std::vector<std::string>& mods) {
    std::string s;
    for (size_t i = 0; i < mods.size(); ++i) { if (i) s += '\n'; s += mods[i]; }
    return s;
}
static std::vector<std::string> SplitMods(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string part = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!part.empty()) out.push_back(std::move(part));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

bool FarmDb::LoadAll(std::vector<MapRun>& outputRuns, int& outputSeconds, int& outputSessionId) {
    std::vector<MapRun> runs;
    int sessionActiveSec = 0;
    int sessionIdMax = outputSessionId;
    const bool loaded = Execute([&] {
        Stmt s(m_db, "SELECT id, map_name, started_at, started_text, duration_sec, total_chaos, "
                     "exalted_rate, hiveblood_gain, beacon_gain, kills_normal, kills_magic, "
                     "kills_rare, kills_unique, session_id, archived, gold_gain, map_mods, kills_rogue "
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
            r.goldGain      = s.Int(15);
            r.mapMods       = SplitMods(s.Text(16));
            r.killsRogue    = s.Int(17);
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
                if (key == "session_active_sec") sessionActiveSec = ReadMetaInteger(val);
                else if (key == "current_session_id") {
                    int sid = ReadMetaInteger(val);
                    if (sid > sessionIdMax) sessionIdMax = sid;
                }
            }
        }
    }, false);
    if (loaded) {
        outputRuns = std::move(runs);
        outputSeconds = sessionActiveSec;
        outputSessionId = sessionIdMax;
    }
    return loaded;
}

void FarmDb::InsertRunInner(MapRun& run) {
    if (run.dbId != 0) throw std::runtime_error("Cannot insert a run with an existing database ID");
    if (run.startedAt == 0) run.startedAt = (int64_t)time(nullptr);
    if (run.startedText.empty()) run.startedText = FormatLocalTime(run.startedAt);
    Stmt s(m_db, "INSERT INTO runs (map_name, started_at, started_text, duration_sec, "
                 "total_chaos, exalted_rate, hiveblood_gain, beacon_gain, kills_normal, "
                 "kills_magic, kills_rare, kills_unique, session_id, archived, gold_gain, map_mods, kills_rogue) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17);");
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
    s.BindInt(15, run.goldGain);
    s.BindText(16, JoinMods(run.mapMods));
    s.BindInt(17, run.killsRogue);
    s.Run();
    run.dbId = sqlite3_last_insert_rowid(m_db);
    ReplaceLoot(run.dbId, run.loot);
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

void FarmDb::UpdateRunInner(const MapRun& run) {
    if (run.dbId <= 0) throw std::runtime_error("Cannot update an unsaved farm run");
    Stmt s(m_db, "UPDATE runs SET map_name=?2, duration_sec=?3, total_chaos=?4, exalted_rate=?5, "
                 "hiveblood_gain=?6, beacon_gain=?7, kills_normal=?8, kills_magic=?9, "
                 "kills_rare=?10, kills_unique=?11, session_id=?12, archived=?13, gold_gain=?14, "
                 "map_mods=?15, kills_rogue=?16 WHERE id=?1;");
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
    s.BindInt(14, run.goldGain);
    s.BindText(15, JoinMods(run.mapMods));
    s.BindInt(16, run.killsRogue);
    s.Run();
    if (sqlite3_changes(m_db) != 1) throw std::runtime_error("Farm run no longer exists");
    ReplaceLoot(run.dbId, run.loot);
}

bool FarmDb::InsertRun(MapRun& run) {
    MapRun candidate;
    const bool saved = Execute([&] { candidate = run; InsertRunInner(candidate); });
    if (saved) run = std::move(candidate);
    return saved;
}

bool FarmDb::UpdateRun(const MapRun& run) {
    return Execute([&] { UpdateRunInner(run); });
}

bool FarmDb::DeleteRun(int64_t dbId) {
    return Execute([&] {
        // Explicit child delete — independent of the foreign_keys pragma state.
        { Stmt s(m_db, "DELETE FROM loot WHERE run_id=?1;"); s.BindI64(1, dbId); s.Run(); }
        { Stmt s(m_db, "DELETE FROM runs WHERE id=?1;");     s.BindI64(1, dbId); s.Run(); }
    });
}

bool FarmDb::DeleteSession(int sessionId) {
    return Execute([&] {
        { Stmt s(m_db, "DELETE FROM loot WHERE run_id IN "
                       "(SELECT id FROM runs WHERE archived=1 AND session_id=?1);");
          s.BindInt(1, sessionId); s.Run(); }
        { Stmt s(m_db, "DELETE FROM runs WHERE archived=1 AND session_id=?1;");
          s.BindInt(1, sessionId); s.Run(); }
    });
}

bool FarmDb::DeleteAllArchived() {
    return Execute([&] { ExecSql(m_db, "DELETE FROM loot WHERE run_id IN (SELECT id FROM runs WHERE archived=1);"
         "DELETE FROM runs WHERE archived=1;");
    });
}

void FarmDb::ArchiveActiveRunsInner(int sessionId) {
    Stmt s(m_db, "UPDATE runs SET archived=1, session_id=?1 WHERE archived=0;");
    s.BindInt(1, sessionId);
    s.Run();
}

bool FarmDb::ArchiveActiveRuns(int sessionId) {
    return Execute([&] { ArchiveActiveRunsInner(sessionId); });
}

void FarmDb::SaveMetaInner(int sessionActiveSec, int currentSessionId) {
    if (sessionActiveSec < 0 || currentSessionId < 0)
        throw std::runtime_error("Invalid farm session metadata");
    { Stmt s(m_db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('session_active_sec', ?1);");
      s.BindText(1, std::to_string(sessionActiveSec)); s.Run(); }
    { Stmt s(m_db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('current_session_id', ?1);");
      s.BindText(1, std::to_string(currentSessionId)); s.Run(); }
}

bool FarmDb::SaveMeta(int sessionActiveSec, int currentSessionId) {
    return Execute([&] { SaveMetaInner(sessionActiveSec, currentSessionId); });
}

bool FarmDb::SaveState(MapRun* liveRun, int sessionActiveSec, int currentSessionId) {
    MapRun candidate;
    const bool saved = Execute([&] {
        if (liveRun) {
            candidate = *liveRun;
            if (candidate.dbId == 0) InsertRunInner(candidate);
            else UpdateRunInner(candidate);
        }
        SaveMetaInner(sessionActiveSec, currentSessionId);
    });
    if (saved && liveRun) *liveRun = std::move(candidate);
    return saved;
}

bool FarmDb::BeginNewSession(MapRun* oldLiveRun, int archivedSessionId, MapRun* freshRun) {
    MapRun oldCandidate, freshCandidate;
    const bool saved = Execute([&] {
        if (oldLiveRun && oldLiveRun == freshRun)
            throw std::runtime_error("New farm session requires separate run records");
        if (oldLiveRun) {
            oldCandidate = *oldLiveRun;
            if (oldCandidate.dbId == 0) InsertRunInner(oldCandidate);
            else UpdateRunInner(oldCandidate);
        }
        ArchiveActiveRunsInner(archivedSessionId);
        SaveMetaInner(0, archivedSessionId);
        if (freshRun) {
            freshCandidate = *freshRun;
            freshCandidate.archived = false;
            InsertRunInner(freshCandidate);
        }
    });
    if (saved) {
        if (oldLiveRun) {
            oldCandidate.archived = true;
            oldCandidate.sessionId = archivedSessionId;
            *oldLiveRun = std::move(oldCandidate);
        }
        if (freshRun) *freshRun = std::move(freshCandidate);
    }
    return saved;
}
