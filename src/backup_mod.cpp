// AutoBackupSaveGame: backs up your Forza Horizon 2 profile (Azimuth) when the game starts and every few minutes.
//
// Azimuth keeps each title's saves in data\titles\<title id>\saves\player-1\. The profile, progress, cars, credits
// and all, is User_9000000000000. Like every save there, it's a folder of revisions (revisions\<16 hex digits>\),
// the game keeping only its newest two, and each revision is a data folder plus a commit file with the size and
// SHA-256 of every file in it:
//
//   Azimuth complete save revision v2
//   blobs 6
//   <sha256> <size> BinarySaveState
//   ...
//
// A backup copies User_* whole into AutoBackupSaveGame\Backups\<title id>\<date>_<time>\, next to azimuth.exe,
// and checks every revision it copied against its commit file. A revision the game was still writing doesn't
// match, and is tried again a little later; one that never matches is left out, so every revision in a backup is
// one the game would load. Nothing changed since the last backup: no new one. Past Keep backups, the oldest go.
//
// Restoring is by hand, with the game closed: see the README.

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "fh2mod.h"

#pragma comment(lib, "bcrypt.lib")

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod = nullptr;

std::wstring g_dir;   // AutoBackupSaveGame\ next to azimuth.exe
std::wstring g_ini;   // its AutoBackupSaveGame.ini

// The intervals the Mods page offers, in minutes (0: only when the game starts).
const int kIntervals[] = {0, 5, 10, 15, 30, 60};
const char* const kIntervalNames[] = {"Off", "5 min", "10 min", "15 min", "30 min", "60 min"};
const int kIntervalCount = int(sizeof kIntervals / sizeof kIntervals[0]);

struct Config {
    bool enabled = true;     // [Settings] Enabled
    bool on_start = true;    // OnStart: a backup when the game starts
    int interval = 15;       // Interval: minutes between backups while playing, 0 none
    int keep = 10;           // Keep: backups kept for each title
    std::wstring folder;     // Folder: where they go ("" : AutoBackupSaveGame\Backups)
};

SRWLOCK g_cfg_lock = SRWLOCK_INIT;
Config g_cfg;
HANDLE g_wake = nullptr;         // set: the timer starts over (the interval changed, the mod was switched)
SRWLOCK g_backup_lock = SRWLOCK_INIT;   // one backup at a time (the timer, a command)
std::wstring g_title;            // the title being played, upper case ("1A96545B")
std::atomic<bool> g_running{false};   // FH2 is running
std::wstring g_last_result = L"no backup yet";

Config config() {
    AcquireSRWLockShared(&g_cfg_lock);
    Config c = g_cfg;
    ReleaseSRWLockShared(&g_cfg_lock);
    return c;
}

// ---- Text --------------------------------------------------------------------------------------------------------

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

void log(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (g_api && g_mod) g_api->log(g_mod, line);
}

// ---- The ini -----------------------------------------------------------------------------------------------------

int ini_int(const wchar_t* key, int def) {
    return int(GetPrivateProfileIntW(L"Settings", key, def, g_ini.c_str()));
}

void load_config() {
    Config c;
    c.enabled = ini_int(L"Enabled", 1) != 0;
    c.on_start = ini_int(L"OnStart", 1) != 0;
    c.interval = std::clamp(ini_int(L"Interval", 15), 0, 24 * 60);
    c.keep = std::clamp(ini_int(L"Keep", 10), 1, 1000);
    wchar_t w[MAX_PATH];
    GetPrivateProfileStringW(L"Settings", L"Folder", L"", w, MAX_PATH, g_ini.c_str());
    c.folder = w;
    AcquireSRWLockExclusive(&g_cfg_lock);
    g_cfg = c;
    ReleaseSRWLockExclusive(&g_cfg_lock);
}

void save_config() {
    const Config c = config();
    const wchar_t* ini = g_ini.c_str();
    WritePrivateProfileStringW(L"Settings", L"Enabled", c.enabled ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Settings", L"OnStart", c.on_start ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Settings", L"Interval", std::to_wstring(c.interval).c_str(), ini);
    WritePrivateProfileStringW(L"Settings", L"Keep", std::to_wstring(c.keep).c_str(), ini);
    WritePrivateProfileStringW(L"Settings", L"Folder", c.folder.c_str(), ini);
}

// Where backups go: the ini's Folder (relative to azimuth.exe's folder), or AutoBackupSaveGame\Backups.
std::wstring backups_root(const Config& c) {
    if (c.folder.empty()) return g_dir + L"\\Backups";
    std::wstring f = c.folder;
    while (!f.empty() && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    const bool absolute = f.size() >= 2 && (f[1] == L':' || (f[0] == L'\\' && f[1] == L'\\'));
    return absolute ? f : std::wstring(g_api->azimuth_dir) + L"\\" + f;
}

// ---- Files -------------------------------------------------------------------------------------------------------

bool is_dir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::wstring& p) {
    if (p.empty() || is_dir(p)) return true;
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2 && !make_dirs(p.substr(0, slash))) return false;
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// The names in a folder: its subfolders (dirs) or its files.
std::vector<std::wstring> list(const std::wstring& dir, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        if (((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == dirs) out.push_back(f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

bool copy_tree(const std::wstring& from, const std::wstring& to) {
    if (!make_dirs(to)) return false;
    for (const auto& f : list(from, false))
        if (!CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE)) return false;
    for (const auto& d : list(from, true))
        if (!copy_tree(from + L"\\" + d, to + L"\\" + d)) return false;
    return true;
}

// Deletes a folder and everything in it. Only ever called on folders inside the backups folder.
void delete_tree(const std::wstring& dir) {
    for (const auto& f : list(dir, false)) {
        const std::wstring p = dir + L"\\" + f;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
    }
    for (const auto& d : list(dir, true)) delete_tree(dir + L"\\" + d);
    RemoveDirectoryW(dir.c_str());
}

bool read_file(const std::wstring& p, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[64 * 1024];
    DWORD n = 0;
    bool ok = true;
    while ((ok = ReadFile(h, buf, sizeof buf, &n, nullptr) != 0) && n) out.append(buf, n);
    CloseHandle(h);
    return ok;
}

// A file's SHA-256 in lower-case hex and its size, or false if it can't be read.
bool sha256_file(const std::wstring& p, std::string& hex, uint64_t& size) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    static thread_local std::vector<unsigned char> buf(1 << 20);
    size = 0;
    DWORD n = 0;
    while (ok && (ok = ReadFile(h, buf.data(), DWORD(buf.size()), &n, nullptr) != 0) && n) {
        ok = BCryptHashData(hash, buf.data(), n, 0) == 0;
        size += n;
    }
    unsigned char digest[32];
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    if (!ok) return false;
    static const char digits[] = "0123456789abcdef";
    hex.clear();
    for (unsigned char b : digest) {
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return true;
}

// ---- Revisions ---------------------------------------------------------------------------------------------------

// Whether a revision folder (with commit and data\) is whole: every file in its commit there with the size and
// SHA-256 given. `why` says what's wrong when it isn't.
bool revision_ok(const std::wstring& rev, std::string& why) {
    std::string commit;
    if (!read_file(rev + L"\\commit", commit) || commit.empty()) {
        why = "no commit file";
        return false;
    }
    size_t pos = 0, blobs = 0, expected = size_t(-1);
    int line_no = 0;
    while (pos < commit.size()) {
        size_t end = commit.find('\n', pos);
        if (end == std::string::npos) end = commit.size();
        std::string line = commit.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        ++line_no;
        if (line_no == 1) {
            if (line.rfind("Azimuth complete save revision", 0) != 0) {
                why = "not an Azimuth commit file";
                return false;
            }
            continue;
        }
        if (line.rfind("blobs ", 0) == 0) {
            expected = size_t(strtoull(line.c_str() + 6, nullptr, 10));
            continue;
        }
        // <sha256> <size> <name>
        const size_t s1 = line.find(' '), s2 = s1 == std::string::npos ? s1 : line.find(' ', s1 + 1);
        if (s2 == std::string::npos) {
            why = "a line it can't read: " + line;
            return false;
        }
        const std::string want_hash = line.substr(0, s1);
        const uint64_t want_size = strtoull(line.c_str() + s1 + 1, nullptr, 10);
        const std::string name = line.substr(s2 + 1);
        std::string hash;
        uint64_t size = 0;
        if (!sha256_file(rev + L"\\data\\" + wide(name), hash, size)) {
            why = name + " missing";
            return false;
        }
        if (size != want_size || _stricmp(hash.c_str(), want_hash.c_str()) != 0) {
            why = name + " doesn't match the commit file";
            return false;
        }
        ++blobs;
    }
    if (expected != size_t(-1) && blobs != expected) {
        why = "the commit file lists " + std::to_string(blobs) + " of its " + std::to_string(expected) + " files";
        return false;
    }
    if (!blobs) {
        why = "an empty commit file";
        return false;
    }
    return true;
}

// ---- The saves ---------------------------------------------------------------------------------------------------

// The title Azimuth is playing, from data\azimuth.ini ([launcher] game=1a96545b), in upper case as its folder is.
std::wstring playing_title() {
    const std::wstring ini = std::wstring(g_api->azimuth_dir) + L"\\data\\azimuth.ini";
    wchar_t w[32] = {};
    GetPrivateProfileStringW(L"launcher", L"game", L"", w, 32, ini.c_str());
    std::wstring t = w;
    for (auto& ch : t) ch = wchar_t(towupper(ch));
    return t;
}

std::wstring saves_dir(const std::wstring& title) {
    return std::wstring(g_api->azimuth_dir) + L"\\data\\titles\\" + title + L"\\saves\\player-1";
}

// The profiles in a title's saves (User_9000000000000).
std::vector<std::wstring> profiles(const std::wstring& saves) {
    std::vector<std::wstring> out;
    for (const auto& d : list(saves, true))
        if (d.rfind(L"User_", 0) == 0 && is_dir(saves + L"\\" + d + L"\\revisions")) out.push_back(d);
    return out;
}

// A save's state, to tell whether it changed: each revision's name and commit file.
std::string fingerprint(const std::wstring& saves_or_backup) {
    std::string out;
    for (const auto& p : profiles(saves_or_backup)) {
        const std::wstring revs = saves_or_backup + L"\\" + p + L"\\revisions";
        for (const auto& r : list(revs, true)) {
            std::string commit;
            read_file(revs + L"\\" + r + L"\\commit", commit);
            out += utf8(p) + "/" + utf8(r) + "\n" + commit + "\n";
        }
    }
    return out;
}

// The backups of a title, oldest first: the folders named <date>_<time> ("2026-11-08_14-30-05").
bool is_backup_name(const std::wstring& n) {
    if (n.size() != 19) return false;
    for (size_t i = 0; i < n.size(); ++i) {
        const wchar_t ch = n[i];
        const bool sep = i == 4 || i == 7 || i == 13 || i == 16;
        if (i == 10 ? ch != L'_' : sep ? ch != L'-' : !iswdigit(ch)) return false;
    }
    return true;
}

std::vector<std::wstring> backups(const std::wstring& title_dir) {
    std::vector<std::wstring> out;
    for (const auto& d : list(title_dir, true))
        if (is_backup_name(d)) out.push_back(d);
    return out;   // sorted by name: by date
}

void prune(const std::wstring& title_dir, int keep) {
    auto all = backups(title_dir);
    for (size_t i = 0; i + size_t(keep) < all.size(); ++i) {
        delete_tree(title_dir + L"\\" + all[i]);
        log("deleted the old backup %s", utf8(all[i]).c_str());
    }
}

// Copies the title's profiles into a new backup. `force`: even if nothing changed since the last one.
// Returns a line for the log and the `backup` command.
std::string backup_now(bool force, const char* why) {
    AcquireSRWLockExclusive(&g_backup_lock);
    struct Unlock {
        ~Unlock() { ReleaseSRWLockExclusive(&g_backup_lock); }
    } unlock;

    const Config c = config();
    const std::wstring title = g_title.empty() ? playing_title() : g_title;
    if (title.empty()) return "error: data\\azimuth.ini doesn't say which game is being played";
    const std::wstring saves = saves_dir(title);
    const auto users = profiles(saves);
    if (users.empty()) return "nothing to back up yet: no profile in " + utf8(saves);

    const std::wstring title_dir = backups_root(c) + L"\\" + title;
    if (!make_dirs(title_dir)) return "error: can't create " + utf8(title_dir);
    for (const auto& d : list(title_dir, true))   // a backup cut short (the game closed during it)
        if (d.size() > 8 && d.compare(d.size() - 8, 8, L".partial") == 0) delete_tree(title_dir + L"\\" + d);

    const auto old = backups(title_dir);
    const std::string now_print = fingerprint(saves);
    if (!force && !old.empty() && fingerprint(title_dir + L"\\" + old.back()) == now_print)
        return "the save hasn't changed since the backup " + utf8(old.back()) + ": no new one";

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[32];
    swprintf_s(name, L"%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring final_dir = title_dir + L"\\" + name;
    const std::wstring work = final_dir + L".partial";
    if (is_dir(final_dir)) return "a backup was made this second already";

    // Each revision: copied, then checked against its commit file. One that doesn't match was being written
    // while it was copied (or deleted): it's tried again a few times, then left out.
    int kept = 0, skipped = 0;
    uint64_t bytes = 0;
    std::string skipped_why;
    for (const auto& user : users) {
        const std::wstring revs = saves + L"\\" + user + L"\\revisions";
        for (const auto& rev : list(revs, true)) {
            const std::wstring from = revs + L"\\" + rev;
            const std::wstring to = work + L"\\" + user + L"\\revisions\\" + rev;
            bool ok = false;
            std::string err;
            for (int attempt = 0; attempt < 6 && !ok; ++attempt) {
                if (attempt) Sleep(5000);
                if (!is_dir(from)) {
                    err = "the game removed it";
                    break;
                }
                delete_tree(to);
                if (!copy_tree(from, to)) {
                    err = "couldn't copy it";
                    continue;
                }
                ok = revision_ok(to, err);
            }
            if (ok) {
                ++kept;
                for (const auto& f : list(to + L"\\data", false)) {
                    WIN32_FILE_ATTRIBUTE_DATA a;
                    if (GetFileAttributesExW((to + L"\\data\\" + f).c_str(), GetFileExInfoStandard, &a))
                        bytes += (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
                }
            } else {
                delete_tree(to);
                ++skipped;
                skipped_why += " " + utf8(user) + "/" + utf8(rev) + " (" + err + ")";
            }
        }
    }
    if (!kept) {
        delete_tree(work);
        return "error: no revision of the profile could be copied whole:" + skipped_why;
    }
    if (!MoveFileW(work.c_str(), final_dir.c_str())) {
        delete_tree(work);
        return "error: can't rename the backup to " + utf8(final_dir);
    }
    prune(title_dir, c.keep);

    char line[1024];
    snprintf(line, sizeof line, "backed up (%s): %s, %d revision%s, %.1f MB%s%s", why, utf8(final_dir).c_str(), kept,
             kept == 1 ? "" : "s", double(bytes) / (1024.0 * 1024.0), skipped ? "; left out:" : "",
             skipped ? skipped_why.c_str() : "");
    return line;
}

void run_backup(bool force, const char* why) {
    std::string r = backup_now(force, why);
    log("%s", r.c_str());
    AcquireSRWLockExclusive(&g_cfg_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t at[16];
    swprintf_s(at, L"%02u:%02u ", t.wHour, t.wMinute);
    g_last_result = at + wide(r);
    ReleaseSRWLockExclusive(&g_cfg_lock);
}

// ---- The thread --------------------------------------------------------------------------------------------------

DWORD WINAPI backup_thread(void*) {
    // Azimuth maps FH2 when Play is pressed; the game only loads the profile later, on the title screen, so a
    // backup now has the save as the last session left it. Other Forza games: null, and nothing to do.
    if (!g_api->wait_for_game(0xFFFFFFFF)) {
        log("not Forza Horizon 2: no backups this session");
        return 0;
    }
    g_title = playing_title();
    if (g_title.empty()) {
        log("data\\azimuth.ini doesn't say which game is being played: no backups this session");
        return 0;
    }
    g_running = true;
    log("title %s, saves in %s", utf8(g_title).c_str(), utf8(saves_dir(g_title)).c_str());

    Config c = config();
    if (c.enabled && c.on_start) run_backup(false, "game start");
    for (;;) {
        c = config();
        const DWORD wait = c.enabled && c.interval > 0 ? DWORD(c.interval) * 60000 : INFINITE;
        if (WaitForSingleObject(g_wake, wait) == WAIT_OBJECT_0) continue;   // settings changed: start over
        c = config();
        if (c.enabled && c.interval > 0) run_backup(false, "timer");
    }
}

// ---- Mods page and commands --------------------------------------------------------------------------------------

int interval_index(int minutes) {
    for (int i = 0; i < kIntervalCount; ++i)
        if (kIntervals[i] == minutes) return i;
    return -1;
}

void on_setting(void*, const char* key, int value) {
    AcquireSRWLockExclusive(&g_cfg_lock);
    if (!strcmp(key, "Enabled")) g_cfg.enabled = value != 0;
    else if (!strcmp(key, "OnStart")) g_cfg.on_start = value != 0;
    else if (!strcmp(key, "Interval") && value >= 0 && value < kIntervalCount) g_cfg.interval = kIntervals[value];
    ReleaseSRWLockExclusive(&g_cfg_lock);
    save_config();
    SetEvent(g_wake);
}

void add_settings() {
    if (g_api->api_version < 4) return;
    const Config c = config();
    FH2Setting s{};
    s.size = sizeof s;
    s.key = "Enabled";
    s.label = "Auto Backup";
    s.description = "Backs up your profile into AutoBackupSaveGame\\Backups, next to azimuth.exe.";
    s.value = c.enabled ? 1 : 0;
    s.on_change = on_setting;
    g_api->add_setting(g_mod, &s);

    FH2Setting st = s;
    st.key = "OnStart";
    st.label = "Auto Backup: On Game Start";
    st.description = "A backup each time the game starts, before it loads your profile.";
    st.value = c.on_start ? 1 : 0;
    g_api->add_setting(g_mod, &st);

    if (g_api->api_version < 9) return;   // choice rows
    FH2Setting iv = s;
    iv.key = "Interval";
    iv.label = "Auto Backup: Every";
    iv.description = "A backup this often while you play, when the save has changed. Other intervals: "
                     "AutoBackupSaveGame.ini.";
    iv.kind = FH2_SETTING_CHOICE;
    iv.choices = kIntervalNames;
    iv.choice_count = kIntervalCount;
    const int i = interval_index(c.interval);
    if (i < 0) return;   // an interval of the ini's own: no row, so the page can't change it unasked
    iv.value = i;
    g_api->add_setting(g_mod, &iv);
}

void cmd_backup(void*, const char*, const char* args, char* reply, size_t size) {
    if (!g_running) {
        snprintf(reply, size, "error: Forza Horizon 2 isn't running yet");
        return;
    }
    const bool force = args && strstr(args, "force");
    run_backup(force, "command");
    AcquireSRWLockShared(&g_cfg_lock);
    snprintf(reply, size, "%s", utf8(g_last_result).c_str());
    ReleaseSRWLockShared(&g_cfg_lock);
}

void cmd_status(void*, const char*, const char*, char* reply, size_t size) {
    const Config c = config();
    const std::wstring title = g_title.empty() ? playing_title() : g_title;
    const std::wstring dir = backups_root(c) + L"\\" + title;
    const auto all = backups(dir);
    AcquireSRWLockShared(&g_cfg_lock);
    const std::string last = utf8(g_last_result);
    ReleaseSRWLockShared(&g_cfg_lock);
    snprintf(reply, size,
             "%s, on game start %s, every %d min (0: never), keeping %d\ntitle %s, %zu backup%s in %s%s%s\nlast: %s",
             c.enabled ? "on" : "off", c.on_start ? "on" : "off", c.interval, c.keep, utf8(title).c_str(), all.size(),
             all.size() == 1 ? "" : "s", utf8(dir).c_str(), all.empty() ? "" : ", newest ",
             all.empty() ? "" : utf8(all.back()).c_str(), last.c_str());
}

}   // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{};
    info.size = sizeof info;
    info.name = "AutoBackupSaveGame";
    info.version = AUTOBACKUP_VERSION;
    info.github_repo = "qiangqiang101/FH2AutoBackupSaveGame";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates = 1;
    info.description = "Backs up your profile when the game starts and every few minutes while you play, into "
                       "AutoBackupSaveGame\\Backups next to azimuth.exe.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;

    g_dir = std::wstring(api->azimuth_dir) + L"\\AutoBackupSaveGame";
    make_dirs(g_dir);
    g_ini = g_dir + L"\\AutoBackupSaveGame.ini";
    load_config();
    save_config();   // a new ini gets every key, with its default
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    add_settings();
    if (api->api_version >= 9) {
        api->add_command(g_mod, "backup", "back up the profile now (\"backup force\": even if it hasn't changed)",
                         cmd_backup, nullptr);
        api->add_command(g_mod, "status", "the settings, the backups and the last result", cmd_status, nullptr);
    }

    HANDLE t = CreateThread(nullptr, 0, backup_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(dll);
    return TRUE;
}
