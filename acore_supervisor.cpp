// ============================================================================================
//  acore_supervisor.exe - native supervisor for AzerothCore worldserver + authserver
// ============================================================================================
//  Why native, and what it buys over the PowerShell watchdog:
//    * owns the child process handle + a Job Object, so process death is an event, not a poll
//      (no WMI, no .NET, no $pid pitfalls, no execution-policy / language-mode dependency)
//    * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE + TerminateJobObject: the whole process tree goes
//      away together and a supervisor crash cannot leave an orphaned server
//    * the worldserver health signal is the WORLD LOOP's own heartbeat line, which the core
//      already writes (World::Update -> WorldUpdateTime::RecordUpdateTime): "Update time diff:
//      Xms with N players online".  A chatty background logger can no longer fake liveness,
//      and CPU-time progress is used as a cross-check.
//    * authserver has no such heartbeat, so it is probed over TCP: connect to RealmServerPort
//      and send one AUTH_LOGON_CHALLENGE for a random NON-EXISTENT account.  The server
//      answers with AUTH_LOGON_CHALLENGE + WOW_FAIL_UNKNOWN_ACCOUNT, which proves the acceptor,
//      the packet handler and a login-DB round trip all work - and it writes nothing to
//      Auth.log, counts no failed logins and bans nothing (AuthSession.cpp:322-330).
//    * console control handlers: closing the window / logoff / shutdown stop the servers first
//      (the PowerShell version could not do this and orphaned the server)
//    * the console stays visible and interactive, so the GM command line keeps working
//
//  Build:  build.bat   (uses vcvars64 + cl, no external dependencies)
//  Run:    acore_supervisor.exe [--config supervisor.ini] [--once] [--help]
// ============================================================================================

#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------------------------------------
//  constants
// ---------------------------------------------------------------------------------------------
static const wchar_t* SUPERVISOR_VERSION = L"1.1.6";

enum class Level { Info, Ok, Warn, Error, Stall, Debug };

// ---------------------------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------------------------
static ULONGLONG Tick()
{
    return GetTickCount64();
}

// Seconds elapsed since `when`, never wrapped and never negative.
// A sample taken later in the same tick (SampleCpuAndMemory calls Tick() after the caller
// captured `now`) makes `when` slightly NEWER than `now`; an unsigned subtraction would then
// wrap to ~2^64 - which showed up in production as
// "cpu last advanced 18446744073709551s ago" and, worse, made the CPU cross-check in the
// log-activity fallback always report "CPU also stalled" (so it never protected a busy server).
static ULONGLONG AgeSeconds(ULONGLONG now, ULONGLONG when)
{
    if (when == 0 || when >= now) return 0;
    return (now - when) / 1000;
}

static std::wstring Format(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::wstring(buf);
}

static std::wstring Trim(const std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    return s;
}

static bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b)
{
    return ToLower(a) == ToLower(b);
}

static bool ToBool(const std::wstring& v, bool dflt)
{
    std::wstring s = ToLower(Trim(v));
    if (s.empty()) return dflt;                     // key absent -> keep the built-in default
    if (s == L"1" || s == L"true" || s == L"yes" || s == L"on" || s == L"y") return true;
    if (s == L"0" || s == L"false" || s == L"no" || s == L"off" || s == L"n") return false;
    return dflt;
}

static int ToInt(const std::wstring& v, int dflt)
{
    if (v.empty()) return dflt;
    return _wtoi(v.c_str());
}

static std::wstring GetExeDirectory()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

static std::wstring GetFullPath(const std::wstring& path)
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetFullPathNameW(path.c_str(), _countof(buf), buf, NULL);
    if (n == 0 || n >= _countof(buf)) return path;
    return std::wstring(buf);
}

// resolve a possibly relative path against a base directory
static std::wstring ResolvePath(const std::wstring& baseDir, const std::wstring& p)
{
    if (p.empty()) return p;
    std::wstring s = p;
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (s.size() >= 2 && s[1] == L':') return GetFullPath(s);           // drive absolute
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') return GetFullPath(s); // UNC
    if (!s.empty() && s[0] == L'\\') return GetFullPath(s);             // root-relative
    std::wstring joined = baseDir;
    if (!joined.empty() && joined.back() != L'\\') joined += L'\\';
    joined += s;
    return GetFullPath(joined);
}

static std::wstring DirName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : path.substr(0, pos);
}

static bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void EnsureDirectory(const std::wstring& path)
{
    if (path.empty() || DirExists(path)) return;
    std::wstring parent = DirName(path);
    if (!parent.empty() && parent != path && !DirExists(parent)) EnsureDirectory(parent);
    CreateDirectoryW(path.c_str(), NULL);
}

static bool ReadFileBytes(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    if (size.QuadPart > 0)
    {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0, total = 0;
        while (total < out.size())
        {
            if (!ReadFile(h, &out[total], (DWORD)(out.size() - total), &read, NULL) || read == 0) break;
            total += read;
        }
        out.resize(total);
    }
    CloseHandle(h);
    return true;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::wstring ReadTextFileW(const std::wstring& path, bool& ok)
{
    std::string bytes;
    ok = ReadFileBytes(path, bytes);
    if (!ok) return L"";
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    return Utf8ToWide(bytes);
}

// ---------------------------------------------------------------------------------------------
//  logging (console via WriteConsoleW so Chinese text renders in any code page, plus a log file)
// ---------------------------------------------------------------------------------------------
static std::wstring g_logPath;
static int g_logMaxBytes = 16 * 1024 * 1024;
static bool g_logReady = false;
static CRITICAL_SECTION g_logLock;

static const wchar_t* LevelName(Level l)
{
    switch (l)
    {
        case Level::Info:  return L"INFO ";
        case Level::Ok:    return L"OK   ";
        case Level::Warn:  return L"WARN ";
        case Level::Error: return L"ERROR";
        case Level::Stall: return L"STALL";
        default:           return L"DEBUG";
    }
}

static WORD LevelColor(Level l)
{
    switch (l)
    {
        case Level::Ok:    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Level::Warn:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case Level::Stall: return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case Level::Debug: return FOREGROUND_INTENSITY;
        default:           return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}

static void RotateLogIfNeeded()
{
    if (!g_logReady || g_logPath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad)) return;
    ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size <= (ULONGLONG)g_logMaxBytes) return;
    std::wstring rotated = g_logPath + L".1";
    DeleteFileW(rotated.c_str());
    MoveFileExW(g_logPath.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void Log(Level level, const std::wstring& msg)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring line = Format(L"[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s",
                               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                               LevelName(level), msg.c_str());

    EnterCriticalSection(&g_logLock);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    bool isConsole = (hOut != INVALID_HANDLE_VALUE) && GetConsoleMode(hOut, &mode);
    if (isConsole)
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        WORD old = 0;
        bool haveOld = GetConsoleScreenBufferInfo(hOut, &info) != 0;
        if (haveOld) old = info.wAttributes;
        SetConsoleTextAttribute(hOut, LevelColor(level));
        DWORD written = 0;
        std::wstring out = line + L"\r\n";
        WriteConsoleW(hOut, out.c_str(), (DWORD)out.size(), &written, NULL);
        if (haveOld) SetConsoleTextAttribute(hOut, old);
    }
    else
    {
        std::string utf8 = WideToUtf8(line + L"\r\n");
        DWORD written = 0;
        WriteFile(hOut, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    }

    if (g_logReady && !g_logPath.empty())
    {
        HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            std::string utf8 = WideToUtf8(line + L"\r\n");
            DWORD written = 0;
            WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
            CloseHandle(h);
            static int writes = 0;
            if (++writes % 200 == 0) RotateLogIfNeeded();
        }
    }
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------------------------
//  configuration (INI: sections -> key/value, resolved against the ini's directory)
// ---------------------------------------------------------------------------------------------
typedef std::map<std::wstring, std::wstring> KeyValueMap;
typedef std::map<std::wstring, KeyValueMap> IniMap;

static bool LoadIni(const std::wstring& path, IniMap& out)
{
    bool ok = false;
    std::wstring text = ReadTextFileW(path, ok);
    if (!ok) return false;

    std::wstring section;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring raw = (nl == std::wstring::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;

        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line[0] == L'[')
        {
            size_t close = line.find(L']');
            if (close != std::wstring::npos)
            {
                section = ToLower(Trim(line.substr(1, close - 1)));
                if (out.find(section) == out.end()) out[section] = KeyValueMap();
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = ToLower(Trim(line.substr(0, eq)));
        std::wstring val = Trim(line.substr(eq + 1));
        if (val.size() >= 2 && ((val.front() == L'"' && val.back() == L'"') || (val.front() == L'\'' && val.back() == L'\'')))
            val = val.substr(1, val.size() - 2);
        out[section][key] = val;
    }
    return true;
}

struct ServiceConfig
{
    std::wstring Name;                  // section name, also used for log lines
    bool Enabled = false;
    std::wstring Exe;                   // absolute path to the server executable
    std::wstring WorkDir;
    std::wstring Args;
    std::wstring Role = L"world";       // world | auth
    std::wstring LogFile;               // resolved log file to watch
    std::wstring LogFileSetting;        // raw setting (may be "auto")
    std::wstring ConfFile;              // server config, used by LogFile=auto

    std::wstring HeartbeatPattern = L"Update time diff: ";
    std::wstring StartupReadyPattern = L"World Initialized In";
    int HeartbeatTimeoutSeconds = 180;
    // Cadence the SERVER configures for the heartbeat line (RecordUpdateTimeDiffInterval /
    // MinRecordUpdateTimeDiff in worldserver.conf) and the timeout actually enforced. The effective
    // value is raised above the interval, because a timeout below it MUST fire on a healthy server.
    bool HeartbeatCadenceKnown = false;      // server config was parsed
    int HeartbeatIntervalSec = 0;            // RecordUpdateTimeDiffInterval (ms -> s)
    int HeartbeatMinRecordMs = 0;            // MinRecordUpdateTimeDiff (ms)
    int HeartbeatTimeoutEffectiveSec = 180;
    // "auto" (default): never use a timeout shorter than the server's own heartbeat interval,
    // because that is guaranteed to fire on a healthy server. "strict": use the configured value as
    // it is (only warn) - for tests that deliberately want sub-interval detection.
    std::wstring HeartbeatTimeoutMode = L"auto";
    // The server config PROVES the heartbeat line cannot be relied on for a tight timeout, because
    // the core only writes it when `diff > MinRecordUpdateTimeDiff` (UpdateTime.cpp:165). With
    // MinRecordUpdateTimeDiff > 0 a quiet server legitimately produces no line for far longer than
    // RecordUpdateTimeDiffInterval, so the heartbeat is not a usable health rule at all: use the
    // log-activity + CPU fallback from the start instead of waiting out a widened timeout that then
    // kills a healthy server anyway (production 2026-09-23).
    bool HeartbeatUnreliable = false;
    int LogStallSeconds = 180;
    int StartupTimeoutSeconds = 900;
    int StartupStallSeconds = 300;
    int StartupGraceSeconds = 60;
    int PollSeconds = 15;

    int StopGraceSeconds = 25;
    int RestartDelaySeconds = 10;
    int PlannedRestartDelaySeconds = 15;
    int MaxBackoffSeconds = 600;
    int StableRunSeconds = 300;
    int MaxRestartsPerHour = 0;         // 0 = unlimited, otherwise pause after that many in an hour

    bool AutoRestartOnCleanExit = false;
    bool AdoptExisting = true;
    bool CpuCrossCheck = true;
    bool RequireHeartbeat = true;
    int MaxWorkingSetMB = 0;

    std::wstring Console = L"shared";   // shared | own | hidden
    std::wstring ProbeHost = L"127.0.0.1";
    int ProbePort = 0;                  // 0 = disabled
    std::wstring ProbeMode = L"auth";   // auth | tcp
    int ProbeTimeoutMs = 3000;
    int ProbeFailuresBeforeRestart = 3;
};

struct GeneralConfig
{
    std::wstring IniDir;
    std::wstring GuardLog;
    int GuardLogMaxMB = 16;
    std::wstring StatusFile;
    bool StatusEnabled = true;
    std::wstring ControlFile;                 // panel -> supervisor command drop box
    std::wstring InstanceName = L"AcoreSupervisor";
    int TickMs = 500;
    bool ExitWhenAllStopped = false;          // keep running (default) so the panel can start services again
};

// Parse LogsDir / the role's own file appender out of an AzerothCore server config so the
// supervisor always watches the file the server actually writes (LogFile = auto).
// The appender name matters: a deployment can define several file appenders
// (e.g. Appender.Server, Appender.Errors, Appender.AuctionatorLog) and only the role's own one
// is the heartbeat/health log.
static bool ResolveAutoLogFile(const std::wstring& serverConf, const std::wstring& workDir,
                               const std::wstring& role, std::wstring& result, std::wstring& why)
{
    IniMap ini;
    if (!LoadIni(serverConf, ini))
    {
        why = L"server config not readable: " + serverConf;
        return false;
    }
    const std::wstring wantAppender = (role == L"auth") ? L"appender.auth" : L"appender.server";
    const std::wstring fallbackFile = (role == L"auth") ? L"Auth.log" : L"Server.log";

    // helper: "2,5,0,Server.log,w" -> "Server.log", but only for file appenders (type 2)
    auto appenderFileName = [](const std::wstring& v) -> std::wstring {
        size_t c1 = v.find(L',');
        if (c1 == std::wstring::npos) return L"";
        if (Trim(v.substr(0, c1)) != L"2") return L"";      // 2 = file appender
        size_t c2 = v.find(L',', c1 + 1);
        if (c2 == std::wstring::npos) return L"";
        size_t c3 = v.find(L',', c2 + 1);
        if (c3 == std::wstring::npos) return L"";
        size_t c4 = v.find(L',', c3 + 1);
        std::wstring name = (c4 == std::wstring::npos) ? v.substr(c3 + 1) : v.substr(c3 + 1, c4 - c3 - 1);
        return Trim(name);
    };

    for (auto& sec : ini)
    {
        const KeyValueMap& kv = sec.second;
        auto itLogs = kv.find(L"logsdir");
        if (itLogs == kv.end()) continue;

        std::wstring logsDir = itLogs->second;
        if (logsDir.empty()) logsDir = workDir;

        std::wstring fileName;
        auto preferred = kv.find(wantAppender);
        if (preferred != kv.end()) fileName = appenderFileName(preferred->second);
        if (fileName.empty())                              // fall back to any file appender
        {
            for (auto& item : kv)
            {
                if (item.first.rfind(L"appender.", 0) != 0) continue;
                std::wstring candidate = appenderFileName(item.second);
                if (!candidate.empty()) { fileName = candidate; break; }
            }
        }
        if (fileName.empty()) fileName = fallbackFile;

        result = ResolvePath(workDir, logsDir);
        if (!result.empty() && result.back() != L'\\') result += L'\\';
        result += fileName;
        why = L"from " + serverConf;
        return true;
    }
    why = L"no LogsDir found in " + serverConf;
    return false;
}

// Read the heartbeat cadence the SERVER configures. AzerothCore's WorldUpdateTime::RecordUpdateTime
// writes the line only when a world tick exceeded MinRecordUpdateTimeDiff AND at least
// RecordUpdateTimeDiffInterval has passed since the previous line - so the line can legitimately be
// absent for the whole interval. Production 2026-09-23 had interval 300000 / min 100 while the
// supervisor used a 180 s timeout: a HEALTHY worldserver was "stalled" every ~3 minutes.
static bool ReadHeartbeatCadence(const std::wstring& serverConf, int& intervalSec, int& minRecordMs, std::wstring& why)
{
    intervalSec = 300;                       // AzerothCore default: 300000 ms
    minRecordMs = 100;                       // AzerothCore default: 100 ms
    IniMap ini;
    if (!LoadIni(serverConf, ini))
    {
        why = L"server config not readable: " + serverConf;
        return false;
    }
    const std::wstring keyInterval = ToLower(L"RecordUpdateTimeDiffInterval");
    const std::wstring keyMin = ToLower(L"MinRecordUpdateTimeDiff");
    bool seen = false;
    for (auto& sec : ini)
    {
        const KeyValueMap& kv = sec.second;
        auto itInterval = kv.find(keyInterval);
        if (itInterval != kv.end())
        {
            intervalSec = ToInt(itInterval->second, intervalSec * 1000) / 1000;
            seen = true;
        }
        auto itMin = kv.find(keyMin);
        if (itMin != kv.end())
        {
            minRecordMs = ToInt(itMin->second, minRecordMs);
            seen = true;
        }
    }
    why = Format(L"%s (RecordUpdateTimeDiffInterval %ds, MinRecordUpdateTimeDiff %dms)", serverConf.c_str(),
                 intervalSec, minRecordMs);
    return seen;
}

static bool LoadConfig(const std::wstring& iniPath, GeneralConfig& gen, std::vector<ServiceConfig>& services,
                       std::wstring& error){
    IniMap ini;
    if (!LoadIni(iniPath, ini))
    {
        error = L"cannot read config file: " + iniPath;
        return false;
    }
    gen.IniDir = DirName(GetFullPath(iniPath));

    if (ini.count(L"general"))
    {
        const KeyValueMap& g = ini[L"general"];
        auto get = [&](const wchar_t* k) -> std::wstring {
            auto it = g.find(k);
            return (it == g.end()) ? L"" : it->second;
        };
        if (!get(L"guardlog").empty()) gen.GuardLog = ResolvePath(gen.IniDir, get(L"guardlog"));
        gen.GuardLogMaxMB = ToInt(get(L"guardlogmaxmb"), gen.GuardLogMaxMB);
        if (!get(L"statusfile").empty()) gen.StatusFile = ResolvePath(gen.IniDir, get(L"statusfile"));
        gen.StatusEnabled = ToBool(get(L"statusenabled"), gen.StatusEnabled);
        if (!get(L"controlfile").empty()) gen.ControlFile = ResolvePath(gen.IniDir, get(L"controlfile"));
        gen.ExitWhenAllStopped = ToBool(get(L"exitwhenallstopped"), gen.ExitWhenAllStopped);
        if (!get(L"instancename").empty()) gen.InstanceName = get(L"instancename");
        gen.TickMs = ToInt(get(L"tickms"), gen.TickMs);
        if (gen.TickMs < 100) gen.TickMs = 100;
    }
    else
    {
        gen.GuardLog = ResolvePath(gen.IniDir, L"logs\\supervisor.log");
        gen.StatusFile = ResolvePath(gen.IniDir, L"logs\\supervisor_status.json");
        gen.ControlFile = ResolvePath(gen.IniDir, L"logs\\supervisor_control.txt");
    }
    if (gen.ControlFile.empty())
        gen.ControlFile = ResolvePath(gen.IniDir, L"logs\\supervisor_control.txt");

    static const wchar_t* serviceSections[] = { L"worldserver", L"authserver" };
    for (const wchar_t* sectionName : serviceSections)
    {
        std::wstring sec = sectionName;
        if (!ini.count(sec)) continue;
        const KeyValueMap& k = ini[sec];
        auto get = [&](const wchar_t* key) -> std::wstring {
            auto it = k.find(key);
            return (it == k.end()) ? L"" : it->second;
        };

        ServiceConfig s;
        s.Name = sec;
        s.Enabled = ToBool(get(L"enabled"), false);
        s.Role = get(L"role").empty() ? ((sec == L"authserver") ? L"auth" : L"world") : ToLower(get(L"role"));
        s.WorkDir = get(L"workdir").empty() ? gen.IniDir : ResolvePath(gen.IniDir, get(L"workdir"));

        std::wstring exe = get(L"exe");
        if (exe.empty()) exe = (s.Role == L"auth") ? L"authserver.exe" : L"worldserver.exe";
        s.Exe = ResolvePath(s.WorkDir, exe);

        s.Args = get(L"args");
        s.ConfFile = get(L"serverconf").empty()
                         ? ResolvePath(s.WorkDir, (s.Role == L"auth") ? L"configs\\authserver.conf" : L"configs\\worldserver.conf")
                         : ResolvePath(s.WorkDir, get(L"serverconf"));

        s.LogFileSetting = get(L"logfile").empty() ? L"auto" : get(L"logfile");
        s.HeartbeatPattern = get(L"heartbeatpattern");
        s.StartupReadyPattern = get(L"startupreadypattern");
        if (k.find(L"heartbeatpattern") == k.end()) s.HeartbeatPattern = (s.Role == L"auth") ? L"" : L"Update time diff: ";
        if (k.find(L"startupreadypattern") == k.end()) s.StartupReadyPattern = (s.Role == L"auth") ? L"" : L"World Initialized In";

        s.HeartbeatTimeoutSeconds = ToInt(get(L"heartbeattimeoutseconds"), s.HeartbeatTimeoutSeconds);
        s.HeartbeatTimeoutMode = ToLower(get(L"heartbeattimeoutmode").empty() ? L"auto" : get(L"heartbeattimeoutmode"));
        // Enforce a timeout that can actually be met: the heartbeat line may legitimately be missing
        // for the server's configured RecordUpdateTimeDiffInterval.
        s.HeartbeatTimeoutEffectiveSec = s.HeartbeatTimeoutSeconds;
        if (!s.HeartbeatPattern.empty())
        {
            std::wstring cadenceWhy;
            s.HeartbeatCadenceKnown = ReadHeartbeatCadence(s.ConfFile, s.HeartbeatIntervalSec, s.HeartbeatMinRecordMs, cadenceWhy);
            if (s.HeartbeatTimeoutMode != L"strict" && s.HeartbeatCadenceKnown && s.HeartbeatIntervalSec > 0)
            {
                int needed = s.HeartbeatIntervalSec + 120;      // one full interval + slack
                if (needed > s.HeartbeatTimeoutEffectiveSec) s.HeartbeatTimeoutEffectiveSec = needed;
            }
            // Deciding this from the config is what separates "healthy but quiet" from "frozen".
            // Both cases look identical at runtime (heartbeat absent, CPU advancing, log written),
            // so the config - not a runtime heuristic - has to say whether the line is even
            // producible on time. Only services that actually HAVE a heartbeat pattern are affected;
            // authserver has none by design and is probed over TCP instead.
            if (!s.HeartbeatCadenceKnown)
            {
                // pattern configured but the server config could not be read (or declares no
                // cadence): there is no evidence the line can be produced at the configured
                // cadence, so the bare HeartbeatTimeoutSeconds is an unverified guess.
                s.HeartbeatUnreliable = true;
            }
            else if (s.HeartbeatMinRecordMs > 0)
            {
                // The core gates the line on `diff > _recordUpdateTimeMin.count()`
                // (UpdateTime.cpp:165), so MinRecordUpdateTimeDiff > 0 means a quiet server writes
                // it far less often than RecordUpdateTimeDiffInterval - the supervisor then waits
                // out the widened limit and restarts a HEALTHY server anyway (production
                // 2026-09-23: min tick 100ms, heartbeat 425s old, CPU advancing 1s earlier).
                s.HeartbeatUnreliable = true;
            }
        }
        s.LogStallSeconds = ToInt(get(L"logstallseconds"), s.LogStallSeconds);
        s.StartupTimeoutSeconds = ToInt(get(L"startuptimeoutseconds"), s.StartupTimeoutSeconds);
        s.StartupStallSeconds = ToInt(get(L"startupstallseconds"), s.StartupStallSeconds);
        s.StartupGraceSeconds = ToInt(get(L"startupgraceseconds"), s.StartupGraceSeconds);
        s.PollSeconds = ToInt(get(L"pollseconds"), s.PollSeconds);
        if (s.PollSeconds < 3) s.PollSeconds = 3;
        s.StopGraceSeconds = ToInt(get(L"stopgraceseconds"), s.StopGraceSeconds);
        s.RestartDelaySeconds = ToInt(get(L"restartdelayseconds"), s.RestartDelaySeconds);
        s.PlannedRestartDelaySeconds = ToInt(get(L"plannedrestartdelayseconds"), s.PlannedRestartDelaySeconds);
        s.MaxBackoffSeconds = ToInt(get(L"maxbackoffseconds"), s.MaxBackoffSeconds);
        s.StableRunSeconds = ToInt(get(L"stablerunseconds"), s.StableRunSeconds);
        s.MaxRestartsPerHour = ToInt(get(L"maxrestartsperhour"), s.MaxRestartsPerHour);
        s.AutoRestartOnCleanExit = ToBool(get(L"autorestartoncleanexit"), s.AutoRestartOnCleanExit);
        s.AdoptExisting = ToBool(get(L"adoptexisting"), s.AdoptExisting);
        s.CpuCrossCheck = ToBool(get(L"cpucrosscheck"), s.CpuCrossCheck);
        s.RequireHeartbeat = ToBool(get(L"requireheartbeat"), s.RequireHeartbeat);
        s.MaxWorkingSetMB = ToInt(get(L"maxworkingsetmb"), s.MaxWorkingSetMB);
        s.Console = ToLower(get(L"console").empty() ? L"shared" : get(L"console"));
        s.ProbeHost = get(L"probehost").empty() ? L"127.0.0.1" : get(L"probehost");
        s.ProbePort = ToInt(get(L"probeport"), s.ProbePort);
        s.ProbeMode = ToLower(get(L"probemode").empty() ? L"auth" : get(L"probemode"));
        s.ProbeTimeoutMs = ToInt(get(L"probeTimeoutms"), s.ProbeTimeoutMs);
        s.ProbeFailuresBeforeRestart = ToInt(get(L"probefailuresbeforerestart"), s.ProbeFailuresBeforeRestart);
        if (s.ProbeFailuresBeforeRestart < 1) s.ProbeFailuresBeforeRestart = 1;

        // resolve the log file
        if (EqualsIgnoreCase(s.LogFileSetting, L"auto"))
        {
            std::wstring why, resolved;
            if (ResolveAutoLogFile(s.ConfFile, s.WorkDir, s.Role, resolved, why))
                s.LogFile = resolved;
            else
                s.LogFile = ResolvePath(s.WorkDir, (s.Role == L"auth") ? L"logs\\Auth.log" : L"logs\\Server.log");
        }
        else
        {
            s.LogFile = ResolvePath(s.WorkDir, s.LogFileSetting);
        }

        if (s.Enabled && !FileExists(s.Exe))
        {
            error = L"[" + s.Name + L"] executable not found: " + s.Exe;
            return false;
        }
        services.push_back(s);
    }

    if (services.empty())
    {
        error = L"no service sections found (expected [worldserver] and/or [authserver])";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
//  process / job plumbing
// ---------------------------------------------------------------------------------------------
struct ServiceRuntime
{
    enum class State { Stopped, Starting, Running, WaitingRestart };

    ServiceConfig cfg;

    HANDLE hJob = NULL;
    HANDLE hProcess = NULL;
    HANDLE hPort = NULL;              // job completion port
    DWORD pid = 0;
    bool adopted = false;
    bool canTerminate = false;        // false: PROCESS_TERMINATE was refused at adoption time
    bool stopPending = false;         // a stop was attempted but the process survived it
    bool restartAfterStop = true;     // what to do once a pending stop finally succeeds

    State state = State::Stopped;
    ULONGLONG launchTick = 0;
    ULONGLONG nextCheckTick = 0;
    ULONGLONG nextActionTick = 0;

    // log watching
    unsigned long long logOffset = 0;
    ULONGLONG lastLogChangeTick = 0;
    unsigned long long lastLogSize = 0;
    FILETIME lastLogWrite{};
    bool logSeen = false;
    bool logWarned = false;
    bool logMtimeValid = false;
    std::wstring carry;
    // The pre-existing end of the log, captured when a NEW process is launched: the offset is only
    // trusted while those bytes are still in the file (see CaptureLogStart / ScanLogFile).
    unsigned long long logSkipOffset = 0;
    std::string logSkipFingerprint;

    // health
    bool startupReady = false;
    bool heartbeatSeen = false;
    ULONGLONG lastHeartbeatTick = 0;
    int observedHeartbeatSec = 0;           // gap between the last two heartbeat lines
    bool cadenceWarned = false;             // cadence > timeout: the rule would fire on a healthy server
    bool heartbeatUnavailable = false;      // pattern configured but never seen -> fall back
    bool stallWarned = false;
    ULONGLONG cpuTotalLast = 0;
    ULONGLONG lastCpuChangeTick = 0;
    double workingSetMB = 0;
    bool jobEmpty = false;

    // probe
    int probeFailures = 0;
    bool probeOk = true;
    std::wstring probeDetail;

    // policy / reporting
    int consecutiveFailures = 0;
    int restarts = 0;
    int restartsThisHour = 0;
    ULONGLONG hourWindowStart = 0;
    DWORD lastExitCode = 0;
    bool killedByUs = false;
    std::wstring lastEvent = L"";
    std::wstring lastStopHow = L"";
    bool serviceStopped = false;            // stopped deliberately, never restart
    bool stoppedByUser = false;             // stopped through the panel control channel
};

struct WinProbe
{
    std::wstring detail;
    bool ok = false;
};

// ------------------------------------------------------------------ process helpers
static bool IsAlive(ServiceRuntime& s)
{
    if (!s.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(s.hProcess, &code)) return false;
    return code == STILL_ACTIVE;
}

static void CloseProcessHandles(ServiceRuntime& s)
{
    if (s.hPort) { CloseHandle(s.hPort); s.hPort = NULL; }
    if (s.hJob) { CloseHandle(s.hJob); s.hJob = NULL; }
    if (s.hProcess) { CloseHandle(s.hProcess); s.hProcess = NULL; }
    s.pid = 0;
}

static void SampleCpuAndMemory(ServiceRuntime& s)
{
    ULONGLONG total = 0;
    if (s.hJob)
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (QueryInformationJobObject(s.hJob, JobObjectBasicAccountingInformation, &info, sizeof(info), NULL))
            total = (ULONGLONG)info.TotalUserTime.QuadPart + (ULONGLONG)info.TotalKernelTime.QuadPart;
    }
    if (total == 0 && s.hProcess)
    {
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(s.hProcess, &c, &e, &k, &u))
            total = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                    (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
    }
    if (total != s.cpuTotalLast)
    {
        s.cpuTotalLast = total;
        s.lastCpuChangeTick = Tick();
    }

    if (s.hProcess)
    {
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(s.hProcess, &pmc, sizeof(pmc)))
            s.workingSetMB = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
}

static void DrainJobPort(ServiceRuntime& s)
{
    if (!s.hPort) return;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* ov = nullptr;
    while (GetQueuedCompletionStatus(s.hPort, &bytes, &key, &ov, 0))
    {
        DWORD msg = bytes;
        DWORD msgPid = (DWORD)(ULONG_PTR)ov;
        if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO)
            s.jobEmpty = true;
        else if (msg == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT || msg == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT)
            Log(Level::Warn, Format(L"[%s] job memory limit reached (pid %lu)", s.cfg.Name.c_str(), msgPid));
    }
}

static HANDLE CreateServiceJob(ServiceRuntime& s, int index)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (s.cfg.MaxWorkingSetMB > 0)
    {
        li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        li.ProcessMemoryLimit = (SIZE_T)s.cfg.MaxWorkingSetMB * 1024ull * 1024ull;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li)))
        Log(Level::Warn, Format(L"[%s] SetInformationJobObject(limits) failed (%lu)", s.cfg.Name.c_str(), GetLastError()));

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (port)
    {
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp{};
        acp.CompletionKey = (PVOID)(ULONG_PTR)(index + 1);
        acp.CompletionPort = port;
        if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &acp, sizeof(acp)))
        {
            Log(Level::Warn, Format(L"[%s] cannot associate job completion port (%lu)", s.cfg.Name.c_str(), GetLastError()));
            CloseHandle(port);
            port = NULL;
        }
    }
    s.hPort = port;
    return job;
}

static std::vector<DWORD> FindProcessesByExe(const std::wstring& exePath)
{
    std::vector<DWORD> found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD size = _countof(buf);
            if (QueryFullProcessImageNameW(h, 0, buf, &size))
            {
                if (EqualsIgnoreCase(GetFullPath(buf), exePath))
                    found.push_back(pe.th32ProcessID);
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool StartServiceProcess(ServiceRuntime& s, int index, std::wstring& error)
{
    CloseProcessHandles(s);

    s.hJob = CreateServiceJob(s, index);

    std::wstring cmdline = L"\"" + s.cfg.Exe + L"\"";
    if (!Trim(s.cfg.Args).empty()) cmdline += L" " + Trim(s.cfg.Args);

    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(L'\0');

    DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (s.cfg.Console == L"own")
    {
        flags |= CREATE_NEW_CONSOLE;
    }
    else if (s.cfg.Console == L"hidden")
    {
        flags |= CREATE_NO_WINDOW;
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    // "shared": no console flag at all -> the server keeps using this console (GM command line)

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, flags, NULL,
                        s.cfg.WorkDir.empty() ? NULL : s.cfg.WorkDir.c_str(), &si, &pi))
    {
        error = Format(L"CreateProcess failed (Win32 error %lu) for %s", GetLastError(), s.cfg.Exe.c_str());
        if (s.hJob) { CloseHandle(s.hJob); s.hJob = NULL; }
        if (s.hPort) { CloseHandle(s.hPort); s.hPort = NULL; }
        return false;
    }

    if (s.hJob)
    {
        if (!AssignProcessToJobObject(s.hJob, pi.hProcess))
            Log(Level::Warn, Format(L"[%s] AssignProcessToJobObject failed (%lu); process tree kill unavailable",
                                    s.cfg.Name.c_str(), GetLastError()));
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    s.hProcess = pi.hProcess;      // kept for waiting, exit codes, CPU/memory sampling
    s.pid = pi.dwProcessId;
    s.adopted = false;
    s.jobEmpty = false;
    return true;
}

// returns "graceful", "forced", "gone" or "failed"
static std::wstring StopServiceProcess(ServiceRuntime& s, bool graceful)
{
    if (!s.hProcess || !IsAlive(s)) return L"gone";

    if (graceful && s.cfg.StopGraceSeconds > 0 && s.pid != 0)
    {
        // CTRL_BREAK to the server's own process group: AzerothCore turns SIGBREAK into
        // World::StopNow(SHUTDOWN_EXIT_CODE), i.e. a clean shutdown that saves characters.
        if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, s.pid))
        {
            Log(Level::Info, Format(L"[%s] sent CTRL_BREAK to pid %lu, waiting up to %ds for a clean shutdown",
                                    s.cfg.Name.c_str(), s.pid, s.cfg.StopGraceSeconds));
            DWORD w = WaitForSingleObject(s.hProcess, (DWORD)s.cfg.StopGraceSeconds * 1000);
            if (w == WAIT_OBJECT_0) return L"graceful";
            Log(Level::Warn, Format(L"[%s] pid %lu did not exit after the graceful signal; forcing",
                                    s.cfg.Name.c_str(), s.pid));
        }
        else
        {
            DWORD err = GetLastError();
            Log(Level::Warn, Format(L"[%s] cannot send CTRL_BREAK to pid %lu (Win32 error %lu)%s",
                                    s.cfg.Name.c_str(), s.pid, err,
                                    s.adopted ? L" - an adopted process does not share the supervisor's console, "
                                                L"so it can only be killed, never shut down gracefully" : L""));
        }
    }

    if (s.hJob)
    {
        if (!TerminateJobObject(s.hJob, 1))
            Log(Level::Error, Format(L"[%s] TerminateJobObject failed (Win32 error %lu) - pid %lu may survive",
                                     s.cfg.Name.c_str(), GetLastError(), s.pid));
    }
    else if (s.hProcess)
    {
        if (!TerminateProcess(s.hProcess, 1))
        {
            DWORD err = GetLastError();
            Log(Level::Error, Format(L"[%s] TerminateProcess(pid %lu) failed (Win32 error %lu)", s.cfg.Name.c_str(),
                                     s.pid, err));
            if (!s.canTerminate)
                Log(Level::Error, Format(L"[%s] the handle was opened without PROCESS_TERMINATE (adopted "
                                         L"hand-started process) - it can only be stopped manually",
                                         s.cfg.Name.c_str()));
            return L"failed";
        }
    }

    DWORD w = WaitForSingleObject(s.hProcess, 15000);
    return (w == WAIT_OBJECT_0) ? L"forced" : L"failed";
}

// ---------------------------------------------------------------------------------------------
//  log watching: find the world-loop heartbeat / startup marker in the new bytes only
// ---------------------------------------------------------------------------------------------
struct LogScan
{
    bool fileSeen = false;
    bool changed = false;
    bool heartbeat = false;
    bool ready = false;
    unsigned long long size = 0;
};

// A newly launched server must only be judged by the lines IT writes. Capture where the log ends
// right after CreateProcess, together with a fingerprint (the last bytes before that offset) that
// proves the offset still means something later on:
//   * appender appends  -> the fingerprint still matches, so the log is read from the captured end
//     and a stale "World Initialized In" / "Update time diff:" line from an EARLIER run can no
//     longer satisfy the startup rule or fake a heartbeat. (Production 2026-09-23: a worldserver
//     that needed 19s reported "finished startup after 1s" because the supervisor read the whole
//     old Server.log from offset 0, which also skipped the startup timeout/stall guards.)
//   * appender truncates (mode "w", the AzerothCore default) -> the fingerprint no longer matches
//     (or the file got shorter than the captured offset) and ScanLogFile starts from 0, so the
//     real startup lines are still seen.
// Adoption deliberately keeps logOffset = 0: for a process that was already running there is no
// "since when" boundary, and reading what is on disk is how the supervisor learns it is alive.
static void CaptureLogStart(ServiceRuntime& s)
{
    s.logOffset = 0;
    s.lastLogSize = 0;
    s.logSkipOffset = 0;
    s.logSkipFingerprint.clear();

    HANDLE h = CreateFileW(s.cfg.LogFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;                                 // no log yet: read everything the server writes

    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0)
    {
        unsigned long long sz = (unsigned long long)size.QuadPart;
        s.logOffset = sz;
        s.lastLogSize = sz;

        const size_t want = 32;
        if (sz >= want)
        {
            LARGE_INTEGER off{};
            off.QuadPart = (LONGLONG)(sz - want);
            std::string tail;
            tail.resize(want);
            DWORD read = 0;
            if (SetFilePointerEx(h, off, NULL, FILE_BEGIN) &&
                ReadFile(h, &tail[0], (DWORD)want, &read, NULL) && read == want)
            {
                s.logSkipOffset = sz - want;
                s.logSkipFingerprint = tail;
            }
        }
    }
    CloseHandle(h);
}

static LogScan ScanLogFile(ServiceRuntime& s)
{
    LogScan r;
    HANDLE h = CreateFileW(s.cfg.LogFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        r.fileSeen = false;
        return r;
    }
    r.fileSeen = true;

    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    r.size = (unsigned long long)size.QuadPart;

    FILETIME mtime{};
    BY_HANDLE_FILE_INFORMATION fi{};
    if (GetFileInformationByHandle(h, &fi)) mtime = fi.ftLastWriteTime;

    // Once only, verify the offset captured at launch: a size comparison alone cannot tell
    // "appended" from "truncated and already regrown past the old size by the first poll" - the
    // fingerprint can, and it decides whether the pre-existing content is still there.
    if (!s.logSkipFingerprint.empty())
    {
        bool same = false;
        if (r.size >= s.logSkipOffset + s.logSkipFingerprint.size())
        {
            LARGE_INTEGER off{};
            off.QuadPart = (LONGLONG)s.logSkipOffset;
            std::string tail;
            tail.resize(s.logSkipFingerprint.size());
            DWORD read = 0;
            if (SetFilePointerEx(h, off, NULL, FILE_BEGIN) &&
                ReadFile(h, &tail[0], (DWORD)tail.size(), &read, NULL) &&
                read == tail.size() && tail == s.logSkipFingerprint)
                same = true;
        }
        if (!same)
        {
            // rewritten (or gone): the captured offset points into unrelated bytes
            s.logOffset = 0;
            s.lastLogSize = 0;
            s.carry.clear();
        }
        s.logSkipFingerprint.clear();
    }

    if (s.logMtimeValid && (mtime.dwLowDateTime != s.lastLogWrite.dwLowDateTime ||
                            mtime.dwHighDateTime != s.lastLogWrite.dwHighDateTime))
        r.changed = true;
    if (r.size != s.lastLogSize) r.changed = true;

    if (r.size < s.logOffset)      // truncated or recreated (Appender mode "w")
    {
        s.logOffset = 0;
        s.carry.clear();
    }

    if (r.size > s.logOffset)
    {
        unsigned long long toRead = r.size - s.logOffset;
        const unsigned long long cap = 2ull * 1024 * 1024;
        if (toRead > cap) { s.logOffset = r.size - cap; }
        toRead = r.size - s.logOffset;

        LARGE_INTEGER off{};
        off.QuadPart = (LONGLONG)s.logOffset;
        if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        {
            std::string buf;
            buf.resize((size_t)toRead);
            DWORD read = 0;
            if (ReadFile(h, &buf[0], (DWORD)buf.size(), &read, NULL) && read > 0)
            {
                buf.resize(read);
                std::wstring text = s.carry + Utf8ToWide(buf);
                if (!s.cfg.HeartbeatPattern.empty() &&
                    text.find(s.cfg.HeartbeatPattern) != std::wstring::npos)
                    r.heartbeat = true;
                if (!s.cfg.StartupReadyPattern.empty() &&
                    text.find(s.cfg.StartupReadyPattern) != std::wstring::npos)
                    r.ready = true;
                s.carry = text.size() > 96 ? text.substr(text.size() - 96) : text;
                s.logOffset = r.size;
            }
        }
    }

    s.lastLogSize = r.size;
    s.lastLogWrite = mtime;
    s.logMtimeValid = true;
    CloseHandle(h);
    return r;
}

// ---------------------------------------------------------------------------------------------
//  network probes
// ---------------------------------------------------------------------------------------------
static bool EnsureWinsock()
{
    static bool done = false;
    static bool ok = false;
    if (!done)
    {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        done = true;
    }
    return ok;
}

// AUTH_LOGON_CHALLENGE for build 12340 with a random NON-EXISTENT account: the server answers
// AUTH_LOGON_CHALLENGE + WOW_FAIL_UNKNOWN_ACCOUNT, which proves acceptor + handler + login DB
// are alive, and writes nothing to Auth.log (AuthSession.cpp:322-330).
//
// Wire format of sAuthLogonChallenge_C (AzerothCore AuthSession.cpp, #pragma pack(1)):
//   cmd(1) error(1) size(2) gamename[4] version1..3(3) build(2) platform[4] os[4] country[4]
//   timezone_bias(4) ip(4) I_len(1) I[I_len]
//
// Two details the server enforces, and that this packet has to get exactly right:
//   * `size` is a NATIVE uint16: ReadHandler() reinterprets the receive buffer and reads
//     challenge->size straight out of it (AuthSession.cpp:245-254), i.e. little endian on x86.
//     A big endian size makes the server decode 0x2B00 instead of 0x002B for a 43 byte
//     challenge, so `4 + size` blows past MAX_ACCEPTED_CHALLENGE_SIZE (51) and the socket is
//     closed with NO reply at all - which surfaces as "auth closed the connection without
//     answering" (formerly "short/empty auth response") on a perfectly healthy authserver.
//   * size must satisfy `size - (sizeof(sAuthLogonChallenge_C) - 4 - 1) == I_len`
//     (AuthSession.cpp:289), i.e. size = 30 + account length; version1..3 and build are the
//     per byte / little endian fields the client sends for 3.3.5a (build 12340).
static std::string BuildProbeChallenge(const std::string& account)
{
    std::string p;
    p.push_back((char)0x00);            // cmd AUTH_LOGON_CHALLENGE
    p.push_back((char)0x08);            // error (unused by server)
    unsigned short size = (unsigned short)(30 + account.size());
    p.push_back((char)(size & 0xFF));          // size, little endian (native uint16)
    p.push_back((char)((size >> 8) & 0xFF));
    p += "WoW";
    p.push_back('\0');
    p.push_back((char)3);
    p.push_back((char)3);
    p.push_back((char)5);
    unsigned short build = 12340;
    p.push_back((char)(build & 0xFF));
    p.push_back((char)((build >> 8) & 0xFF));
    p += "x86";
    p.push_back('\0');
    p += "Win";
    p.push_back('\0');
    p += "enUS";
    p.push_back((char)0); p.push_back((char)0); p.push_back((char)0); p.push_back((char)0);   // timezone
    p.push_back((char)0); p.push_back((char)0); p.push_back((char)0); p.push_back((char)127); // ip 127.0.0.1 (LE)
    p.push_back((char)account.size());
    p += account;
    return p;
}

static bool ProbeTcpOnly(const std::wstring& host, int port, int timeoutMs, std::wstring& detail)
{
    if (!EnsureWinsock()) { detail = L"winsock unavailable"; return false; }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { detail = L"socket() failed"; return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    std::string hostA = WideToUtf8(host);
    if (InetPtonA(AF_INET, hostA.c_str(), &addr.sin_addr) != 1)
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostA.c_str(), NULL, &hints, &res) != 0 || !res)
        {
            closesocket(sock);
            detail = L"cannot resolve host";
            return false;
        }
        addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);

    bool ok = false;
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0)
    {
        ok = true;
    }
    else if (WSAGetLastError() == WSAEWOULDBLOCK)
    {
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(sock, &wf); FD_SET(sock, &ef);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, NULL, &wf, &ef, &tv);
        if (sel > 0 && FD_ISSET(sock, &wf))
        {
            int err = 0, len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0) ok = true;
        }
    }

    if (!ok) { detail = L"connect failed"; closesocket(sock); return false; }

    // authentication round trip
    std::string account = WideToUtf8(Format(L"acprobe%06x", (unsigned)(GetTickCount64() & 0xFFFFFF)));
    if (account.empty()) account = "acprobe";
    std::string packet = BuildProbeChallenge(account);
    if (send(sock, packet.data(), (int)packet.size(), 0) == SOCKET_ERROR)
    {
        detail = L"send failed";
        closesocket(sock);
        return false;
    }

    fd_set rf, ef2;
    FD_ZERO(&rf); FD_ZERO(&ef2);
    FD_SET(sock, &rf); FD_SET(sock, &ef2);
    timeval tv2{};
    tv2.tv_sec = timeoutMs / 1000;
    tv2.tv_usec = (timeoutMs % 1000) * 1000;
    int sel2 = select(0, &rf, NULL, &ef2, &tv2);
    if (sel2 <= 0 || !FD_ISSET(sock, &rf))
    {
        detail = L"no response to AUTH_LOGON_CHALLENGE";
        closesocket(sock);
        return false;
    }
    unsigned char buf[64];
    int got = recv(sock, (char*)buf, sizeof(buf), 0);
    closesocket(sock);
    if (got <= 0)
    {
        // 0 = orderly FIN, -1 = reset: the authserver hung up without a reply, which it only
        // does when it rejects the challenge packet itself (malformed size / I_len) or an IP ban
        // raced the connection. A healthy authserver always answers (see BuildProbeChallenge).
        detail = L"auth closed the connection without answering";
        return false;
    }
    if (got < 2)
    {
        detail = Format(L"short auth response (%d byte)", got);
        return false;
    }
    unsigned char cmd = buf[0];
    // AzerothCore answers [cmd=AUTH_LOGON_CHALLENGE][0x00][fail code], so the code is the THIRD
    // byte; a 2 byte answer only comes from a fake/other implementation, so fall back to buf[1].
    unsigned char err = (got >= 3) ? buf[2] : buf[1];
    if (cmd == 0x00)          // AUTH_LOGON_CHALLENGE
    {
        detail = Format(L"auth responded (fail code 0x%02X)", err);
        return true;
    }
    detail = Format(L"unexpected auth response 0x%02X", cmd);
    return false;
}

static WinProbe RunProbe(const ServiceConfig& cfg)
{
    WinProbe r;
    if (cfg.ProbePort <= 0) { r.ok = true; r.detail = L"probe disabled"; return r; }
    if (!EnsureWinsock()) { r.detail = L"winsock unavailable"; return r; }
    if (EqualsIgnoreCase(cfg.ProbeMode, L"tcp"))
    {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) { r.detail = L"socket() failed"; return r; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)cfg.ProbePort);
        InetPtonA(AF_INET, WideToUtf8(cfg.ProbeHost).c_str(), &addr.sin_addr);
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        bool ok = false;
        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) ok = true;
        else if (WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set wf;
            FD_ZERO(&wf); FD_SET(sock, &wf);
            timeval tv{};
            tv.tv_sec = cfg.ProbeTimeoutMs / 1000;
            tv.tv_usec = (cfg.ProbeTimeoutMs % 1000) * 1000;
            if (select(0, NULL, &wf, NULL, &tv) > 0 && FD_ISSET(sock, &wf))
            {
                int err = 0, len = sizeof(err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0) ok = true;
            }
        }
        closesocket(sock);
        r.ok = ok;
        r.detail = ok ? Format(L"port %d accepts connections", cfg.ProbePort)
                      : Format(L"port %d not accepting", cfg.ProbePort);
        return r;
    }
    r.ok = ProbeTcpOnly(cfg.ProbeHost, cfg.ProbePort, cfg.ProbeTimeoutMs, r.detail);
    return r;
}

// ---------------------------------------------------------------------------------------------
//  status file
// ---------------------------------------------------------------------------------------------
static ULONGLONG g_runStartedTick = 0;
static bool g_exitAfterCommand = false;

// last command received through the control channel (exposed in the status file)
struct ControlCommandState
{
    bool Valid = false;
    std::wstring Id;
    std::wstring Action;
    std::wstring Target;
    std::wstring Result;        // ok | rejected | failed
    std::wstring Message;
    ULONGLONG RequestedTick = 0;
    ULONGLONG DoneTick = 0;
};
static ControlCommandState g_lastCommand;

static std::wstring JsonEscape(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in)
    {
        switch (c)
        {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': break;
            case L'\t': out += L"\\t"; break;
            default:    out += c; break;
        }
    }
    return out;
}

static std::wstring StateName(ServiceRuntime::State st)
{
    switch (st)
    {
        case ServiceRuntime::State::Starting:       return L"starting";
        case ServiceRuntime::State::Running:        return L"running";
        case ServiceRuntime::State::WaitingRestart: return L"waiting-restart";
        default:                                    return L"stopped";
    }
}

static void WriteStatusFile(const GeneralConfig& gen, const std::vector<ServiceRuntime>& services)
{
    if (!gen.StatusEnabled || gen.StatusFile.empty()) return;

    ULONGLONG now = Tick();
    std::wstring json = L"{\n";
    json += L"  \"version\": \"" + std::wstring(SUPERVISOR_VERSION) + L"\",\n";
    json += Format(L"  \"pid\": %lu,\n", (unsigned long)GetCurrentProcessId());
    json += Format(L"  \"updatedAtTickMs\": %llu,\n", now);
    json += Format(L"  \"startedAtTickMs\": %llu,\n", g_runStartedTick);
    json += Format(L"  \"uptimeSec\": %llu,\n", g_runStartedTick ? (now - g_runStartedTick) / 1000 : 0);
    json += Format(L"  \"tickMs\": %d,\n", gen.TickMs);
    json += L"  \"instance\": \"" + JsonEscape(gen.InstanceName) + L"\",\n";
    json += L"  \"configFile\": \"" + JsonEscape(gen.IniDir + L"\\supervisor.ini") + L"\",\n";
    json += L"  \"controlFile\": \"" + JsonEscape(gen.ControlFile) + L"\",\n";
    json += L"  \"statusFile\": \"" + JsonEscape(gen.StatusFile) + L"\",\n";
    json += L"  \"guardLog\": \"" + JsonEscape(gen.GuardLog) + L"\",\n";
    json += L"  \"lastCommand\": ";
    if (!g_lastCommand.Valid)
    {
        json += L"null,\n";
    }
    else
    {
        json += L"{\n";
        json += L"    \"id\": \"" + JsonEscape(g_lastCommand.Id) + L"\",\n";
        json += L"    \"action\": \"" + JsonEscape(g_lastCommand.Action) + L"\",\n";
        json += L"    \"target\": \"" + JsonEscape(g_lastCommand.Target) + L"\",\n";
        json += L"    \"result\": \"" + JsonEscape(g_lastCommand.Result) + L"\",\n";
        json += L"    \"message\": \"" + JsonEscape(g_lastCommand.Message) + L"\",\n";
        json += Format(L"    \"requestedAtTickMs\": %llu,\n", g_lastCommand.RequestedTick);
        json += Format(L"    \"doneAtTickMs\": %llu\n", g_lastCommand.DoneTick);
        json += L"  },\n";
    }
    json += L"  \"services\": [\n";
    for (size_t i = 0; i < services.size(); ++i)
    {
        const ServiceRuntime& s = services[i];
        ULONGLONG uptime = (s.launchTick && s.state != ServiceRuntime::State::Stopped)
                               ? (now - s.launchTick) / 1000 : 0;
        long long logAge = -1;
        if (s.logMtimeValid)
        {
            ULONGLONG lastChange = s.lastLogChangeTick ? s.lastLogChangeTick : s.launchTick;
            logAge = (long long)AgeSeconds(now, lastChange);
        }
        long long hbAge = s.heartbeatSeen ? (long long)AgeSeconds(now, s.lastHeartbeatTick) : -1;
        json += L"    {\n";
        json += L"      \"name\": \"" + JsonEscape(s.cfg.Name) + L"\",\n";
        json += L"      \"role\": \"" + JsonEscape(s.cfg.Role) + L"\",\n";
        json += L"      \"enabled\": " + std::wstring(s.cfg.Enabled ? L"true" : L"false") + L",\n";
        json += L"      \"state\": \"" + StateName(s.state) + L"\",\n";
        json += Format(L"      \"stoppedByUser\": %s,\n", s.stoppedByUser ? L"true" : L"false");
        json += Format(L"      \"pid\": %lu,\n", (unsigned long)s.pid);
        json += Format(L"      \"adopted\": %s,\n", s.adopted ? L"true" : L"false");
        json += Format(L"      \"uptimeSec\": %llu,\n", uptime);
        json += L"      \"exe\": \"" + JsonEscape(s.cfg.Exe) + L"\",\n";
        json += L"      \"workDir\": \"" + JsonEscape(s.cfg.WorkDir) + L"\",\n";
        json += L"      \"logFile\": \"" + JsonEscape(s.cfg.LogFile) + L"\",\n";
        json += Format(L"      \"logStaleSec\": %lld,\n", logAge);
        json += Format(L"      \"heartbeatSeen\": %s,\n", s.heartbeatSeen ? L"true" : L"false");
        json += Format(L"      \"heartbeatCadenceSec\": %d,\n", s.observedHeartbeatSec);
        json += Format(L"      \"heartbeatAgeSec\": %lld,\n", hbAge);
        json += Format(L"      \"heartbeatTimeoutSec\": %d,\n", s.cfg.HeartbeatTimeoutEffectiveSec);
        json += Format(L"      \"heartbeatTimeoutConfiguredSec\": %d,\n", s.cfg.HeartbeatTimeoutSeconds);
        json += Format(L"      \"heartbeatIntervalSec\": %d,\n", s.cfg.HeartbeatIntervalSec);
        json += Format(L"      \"heartbeatMinRecordMs\": %d,\n", s.cfg.HeartbeatMinRecordMs);
        json += Format(L"      \"cpuTotalMs\": %llu,\n", s.cpuTotalLast / 10000ull);
        json += Format(L"      \"workingSetMB\": %.1f,\n", s.workingSetMB);
        json += Format(L"      \"probeOk\": %s,\n", s.probeOk ? L"true" : L"false");
        json += Format(L"      \"probeFailures\": %d,\n", s.probeFailures);
        json += L"      \"probeDetail\": \"" + JsonEscape(s.probeDetail) + L"\",\n";
        json += Format(L"      \"restarts\": %d,\n", s.restarts);
        json += Format(L"      \"consecutiveFailures\": %d,\n", s.consecutiveFailures);
        json += Format(L"      \"lastExitCode\": %lu,\n", (unsigned long)s.lastExitCode);
        json += L"      \"lastEvent\": \"" + JsonEscape(s.lastEvent) + L"\",\n";
        json += L"      \"lastStop\": \"" + JsonEscape(s.lastStopHow) + L"\"\n";
        json += (i + 1 == services.size()) ? L"    }\n" : L"    },\n";
    }
    json += L"  ]\n}\n";

    EnsureDirectory(DirName(gen.StatusFile));
    std::wstring tmp = gen.StatusFile + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string utf8 = WideToUtf8(json);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), gen.StatusFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// ---------------------------------------------------------------------------------------------
//  supervisor state
// ---------------------------------------------------------------------------------------------
static GeneralConfig g_gen;
static std::vector<ServiceRuntime> g_services;
static volatile LONG g_stopRequested = 0;
static volatile LONG g_hardExit = 0;

static int GetRestartDelay(ServiceRuntime& s)
{
    int base = s.cfg.RestartDelaySeconds;
    if (s.consecutiveFailures <= 1) return base;
    int delay = base;
    for (int i = 1; i < s.consecutiveFailures && delay < s.cfg.MaxBackoffSeconds; ++i)
        delay *= 2;
    if (delay > s.cfg.MaxBackoffSeconds) delay = s.cfg.MaxBackoffSeconds;
    return delay;
}

static void ScheduleRestart(ServiceRuntime& s, int delaySeconds, const std::wstring& reason)
{
    s.state = ServiceRuntime::State::WaitingRestart;
    s.nextActionTick = Tick() + (ULONGLONG)delaySeconds * 1000ull;
    Log(Level::Info, Format(L"[%s] %s - restarting in %ds (restart #%d, consecutive failures %d)",
                            s.cfg.Name.c_str(), reason.c_str(), delaySeconds, s.restarts, s.consecutiveFailures));
}

static void NoteRestart(ServiceRuntime& s)
{
    ULONGLONG now = Tick();
    if (s.hourWindowStart == 0 || now - s.hourWindowStart > 3600ull * 1000ull)
    {
        s.hourWindowStart = now;
        s.restartsThisHour = 0;
    }
    s.restarts++;
    s.restartsThisHour++;
}

// stop the service because of a health problem or a stop request; schedules the next action
static void KillAndSchedule(ServiceRuntime& s, const std::wstring& reason, bool failure)
{
    Log(Level::Stall, Format(L"[%s] %s - pid %lu", s.cfg.Name.c_str(), reason.c_str(), (unsigned long)s.pid));
    s.killedByUs = true;
    s.lastEvent = reason;
    std::wstring how = StopServiceProcess(s, true);
    s.lastStopHow = how;
    Log(Level::Warn, Format(L"[%s] stop result: %s", s.cfg.Name.c_str(), how.c_str()));

    // A process that SURVIVED the stop must not be "restarted": that is how two worldservers ended
    // up running side by side in production (2026-09-23), fighting over the same ports and database.
    // Keep the handle (so we can retry the stop) and come back instead of starting a second instance.
    if (IsAlive(s))
    {
        int retry = s.cfg.RestartDelaySeconds > 15 ? s.cfg.RestartDelaySeconds : 15;
        if (failure) s.consecutiveFailures++;
        s.stopPending = true;
        s.lastEvent = L"stop-failed";
        Log(Level::Error, Format(L"[%s] pid %lu is STILL RUNNING after the stop (%s) - refusing to start a "
                                 L"second instance; retrying the stop in %ds%s",
                                 s.cfg.Name.c_str(), (unsigned long)s.pid, how.c_str(), retry,
                                 s.canTerminate
                                     ? L""
                                     : L" (it was adopted without PROCESS_TERMINATE, so only a manual stop can work)"));
        s.state = ServiceRuntime::State::WaitingRestart;
        s.nextActionTick = Tick() + (ULONGLONG)retry * 1000ull;
        return;
    }

    CloseProcessHandles(s);

    if (failure) s.consecutiveFailures++;
    NoteRestart(s);

    if (s.cfg.MaxRestartsPerHour > 0 && s.restartsThisHour > s.cfg.MaxRestartsPerHour)
    {
        int longDelay = s.cfg.MaxBackoffSeconds > 0 ? s.cfg.MaxBackoffSeconds : 600;
        Log(Level::Error, Format(L"[%s] %d restarts within an hour (limit %d) - backing off %ds",
                                 s.cfg.Name.c_str(), s.restartsThisHour, s.cfg.MaxRestartsPerHour, longDelay));
        ScheduleRestart(s, longDelay, L"restart limit reached");
        return;
    }
    ScheduleRestart(s, GetRestartDelay(s), reason);
}

static void HandleProcessExit(ServiceRuntime& s)
{
    DWORD code = 0;
    GetExitCodeProcess(s.hProcess, &code);
    ULONGLONG lifeSec = s.launchTick ? (Tick() - s.launchTick) / 1000 : 0;
    s.lastExitCode = code;

    bool wasStable = (lifeSec >= (ULONGLONG)s.cfg.StableRunSeconds);
    if (wasStable) s.consecutiveFailures = 0;

    Log(Level::Info, Format(L"[%s] pid %lu exited with code %lu after %llus",
                            s.cfg.Name.c_str(), (unsigned long)s.pid, (unsigned long)code, lifeSec));

    // A stop we initiated ourselves is never a "clean shutdown" for policy purposes.
    bool selfKilled = s.killedByUs;
    s.killedByUs = false;
    CloseProcessHandles(s);

    if (!selfKilled && code == 0 && !s.cfg.AutoRestartOnCleanExit)
    {
        Log(Level::Ok, Format(L"[%s] clean shutdown (exit code 0) - this service is not restarted",
                              s.cfg.Name.c_str()));
        s.state = ServiceRuntime::State::Stopped;
        s.serviceStopped = true;
        s.lastEvent = L"clean-exit";
        return;
    }

    if (code == 2 && !selfKilled)
    {
        s.consecutiveFailures = 0;
        s.lastEvent = L"planned-restart";
        NoteRestart(s);
        ScheduleRestart(s, s.cfg.PlannedRestartDelaySeconds, L"planned restart requested (exit code 2)");
        return;
    }

    s.consecutiveFailures++;
    s.lastEvent = L"abnormal-exit";
    NoteRestart(s);
    ScheduleRestart(s, GetRestartDelay(s),
                    Format(L"abnormal exit (code %lu, failure %d)", (unsigned long)code, s.consecutiveFailures));
}

static void HealthCheck(ServiceRuntime& s, ULONGLONG now)
{
    LogScan scan = ScanLogFile(s);
    if (scan.fileSeen)
    {
        if (!s.logSeen)
        {
            s.logSeen = true;
            Log(Level::Debug, Format(L"[%s] watching log %s", s.cfg.Name.c_str(), s.cfg.LogFile.c_str()));
        }
        if (scan.changed || s.lastLogChangeTick == 0) s.lastLogChangeTick = now;
        if (scan.heartbeat)
        {
            if (s.lastHeartbeatTick != 0)
            {
                s.observedHeartbeatSec = (int)AgeSeconds(now, s.lastHeartbeatTick);
                // Only a warning: the cadence itself is fine, but it leaves almost no margin, so a
                // single missed beat will look like a stall (production 2026-09-23: cadence 62s).
                if (!s.cadenceWarned && s.observedHeartbeatSec > 0 &&
                    (ULONGLONG)s.observedHeartbeatSec * 2 > (ULONGLONG)s.cfg.HeartbeatTimeoutSeconds)
                {
                    s.cadenceWarned = true;
                    Log(Level::Warn, Format(L"[%s] the heartbeat line appears every ~%ds while HeartbeatTimeoutSeconds is %d: "
                                            L"one missed beat already looks like a stall. Set MinRecordUpdateTimeDiff = 0 (and "
                                            L"RecordUpdateTimeDiffInterval = 60000) in %s, or raise HeartbeatTimeoutSeconds.",
                                            s.cfg.Name.c_str(), s.observedHeartbeatSec, s.cfg.HeartbeatTimeoutSeconds,
                                            s.cfg.ConfFile.c_str()));
                }
            }
            s.heartbeatSeen = true;
            s.lastHeartbeatTick = now;
            s.stallWarned = false;
        }
        if (scan.ready) s.startupReady = true;
    }
    else if (!s.logSeen && !s.logWarned && (now - s.launchTick) / 1000 > 15)
    {
        s.logWarned = true;
        Log(Level::Warn, Format(L"[%s] log file not found: %s", s.cfg.Name.c_str(), s.cfg.LogFile.c_str()));
    }

    SampleCpuAndMemory(s);
    DrainJobPort(s);

    const ULONGLONG lifeSec = (now - s.launchTick) / 1000;
    const ULONGLONG logStaleSec = s.lastLogChangeTick ? AgeSeconds(now, s.lastLogChangeTick) : lifeSec;
    const ULONGLONG cpuStaleSec = s.lastCpuChangeTick ? AgeSeconds(now, s.lastCpuChangeTick) : lifeSec;

    // ---------------- probe (runs in both phases: for authserver it is the main signal) -----
    if (s.cfg.ProbePort > 0)
    {
        WinProbe p = RunProbe(s.cfg);
        s.probeOk = p.ok;
        s.probeDetail = p.detail;
        if (p.ok)
        {
            if (s.probeFailures > 0)
                Log(Level::Ok, Format(L"[%s] probe recovered (%s)", s.cfg.Name.c_str(), p.detail.c_str()));
            s.probeFailures = 0;
        }
        else
        {
            s.probeFailures++;
            Log(Level::Warn, Format(L"[%s] probe failed %d/%d: %s", s.cfg.Name.c_str(), s.probeFailures,
                                    s.cfg.ProbeFailuresBeforeRestart, p.detail.c_str()));
            if (s.probeFailures >= s.cfg.ProbeFailuresBeforeRestart)
            {
                int failures = s.probeFailures;
                s.probeFailures = 0;
                KillAndSchedule(s, Format(L"probe failed %d times in a row (%s)", failures, p.detail.c_str()), true);
                return;
            }
        }
    }

    // ---------------- startup phase ----------------
    if (s.state == ServiceRuntime::State::Starting)
    {
        bool ready = s.startupReady;
        if (!ready && !s.cfg.HeartbeatPattern.empty() && s.heartbeatSeen)
            ready = true;                           // the world loop runs => startup finished
        if (!ready && s.cfg.StartupReadyPattern.empty() && s.cfg.HeartbeatPattern.empty())
        {
            // no log-based signal at all: either the probe is the signal, or just wait the grace
            ULONGLONG grace = (s.cfg.ProbePort > 0) ? 10ull : (ULONGLONG)s.cfg.StartupGraceSeconds;
            if (lifeSec >= grace) ready = true;
        }

        if (ready)
        {
            s.state = ServiceRuntime::State::Running;
            s.lastEvent = L"startup-complete";
            std::wstring rule = !s.cfg.HeartbeatPattern.empty() ? L"world-loop heartbeat"
                              : (s.cfg.ProbePort > 0 ? L"TCP/auth probe" : L"log activity");
            Log(Level::Ok, Format(L"[%s] pid %lu finished startup after %llus; health rule: %s",
                                  s.cfg.Name.c_str(), (unsigned long)s.pid, lifeSec, rule.c_str()));
            return;
        }
        if (lifeSec > (ULONGLONG)s.cfg.StartupTimeoutSeconds)
        {
            KillAndSchedule(s, Format(L"startup did not finish within %ds", s.cfg.StartupTimeoutSeconds), true);
            return;
        }
        if (s.logSeen && logStaleSec > (ULONGLONG)s.cfg.StartupStallSeconds)
        {
            KillAndSchedule(s, Format(L"startup stalled (no log activity for %llus)", logStaleSec), true);
            return;
        }
        return;
    }

    // ---------------- running phase ----------------
    // 1) world-loop heartbeat: the signal that cannot be faked by a background logger.
    //    Skipped for a heartbeat the server config proved unreliable: there the line is absent by
    //    design on a quiet server, so judging on it can only produce false stalls.
    if (!s.cfg.HeartbeatPattern.empty() && !s.cfg.HeartbeatUnreliable)
    {
        if (s.heartbeatSeen)
        {
            ULONGLONG hbAge = AgeSeconds(now, s.lastHeartbeatTick);
            if (hbAge > (ULONGLONG)s.cfg.HeartbeatTimeoutEffectiveSec)
            {
                // The observed cadence is part of the message on purpose: it tells a cadence problem
                // (e.g. MinRecordUpdateTimeDiff left at its default 100) from a real frozen world loop.
                std::wstring detail = Format(L"world-loop heartbeat missing for %llus (limit %ds", hbAge,
                                             s.cfg.HeartbeatTimeoutEffectiveSec);
                if (s.cfg.HeartbeatTimeoutEffectiveSec != s.cfg.HeartbeatTimeoutSeconds)
                    detail += Format(L" = max(HeartbeatTimeoutSeconds %d, RecordUpdateTimeDiffInterval %ds + 120)",
                                     s.cfg.HeartbeatTimeoutSeconds, s.cfg.HeartbeatIntervalSec);
                if (s.observedHeartbeatSec > 0)
                    detail += Format(L", last cadence %ds", s.observedHeartbeatSec);
                else
                    detail += L", no cadence observed yet - check RecordUpdateTimeDiffInterval/MinRecordUpdateTimeDiff";
                detail += L")";

                // A timeout here is a genuine frozen world loop, and it is killed. The CPU is
                // deliberately NOT used to veto this rule: a frozen world loop with a chatty
                // background logger still advances the process CPU time (the logger thread keeps
                // running), so a CPU gate would silently disable the one signal that catches exactly
                // that case (tests\run_scenario.ps1 "chatty"). Whether this rule may be trusted at
                // all is decided from the SERVER CONFIG instead (HeartbeatUnreliable, above): with
                // MinRecordUpdateTimeDiff > 0 the line is absent by design on a quiet server, so the
                // rule is skipped and log-activity + CPU monitoring is used, which is what stops the
                // 2026-09-23 false restart.
                if (s.cfg.CpuCrossCheck)
                    detail += Format(L" [cpu last advanced %llus ago]", cpuStaleSec);
                KillAndSchedule(s, detail, true);
                return;
            }
        }
        else if (lifeSec > (ULONGLONG)s.cfg.StartupTimeoutSeconds && !s.heartbeatUnavailable)
        {
            s.heartbeatUnavailable = true;
            if (s.cfg.RequireHeartbeat)
                Log(Level::Error, Format(L"[%s] heartbeat line '%s' never appeared in %s - check "
                                         L"RecordUpdateTimeDiffInterval/MinRecordUpdateTimeDiff; "
                                         L"falling back to log-activity monitoring",
                                         s.cfg.Name.c_str(), s.cfg.HeartbeatPattern.c_str(), s.cfg.LogFile.c_str()));
            else
                Log(Level::Warn, Format(L"[%s] heartbeat line never appeared - log-activity monitoring only",
                                        s.cfg.Name.c_str()));
        }
    }

    // 2) fallback: log activity, cross-checked against CPU progress
    if ((s.cfg.HeartbeatPattern.empty() || s.heartbeatUnavailable || s.cfg.HeartbeatUnreliable) && s.cfg.ProbePort <= 0)
    {
        if (s.logSeen && logStaleSec > (ULONGLONG)s.cfg.LogStallSeconds)
        {
            bool cpuAlsoStalled = cpuStaleSec > (ULONGLONG)s.cfg.HeartbeatTimeoutEffectiveSec;
            if (!s.cfg.CpuCrossCheck || cpuAlsoStalled)
            {
                KillAndSchedule(s, Format(L"no log activity for %llus and CPU stalled for %llus",
                                          logStaleSec, cpuStaleSec), true);
                return;
            }
            if (!s.stallWarned)
            {
                s.stallWarned = true;
                Log(Level::Warn, Format(L"[%s] no log activity for %llus but CPU is still advancing "
                                        L"(cpu changed %llus ago) - not killing",
                                        s.cfg.Name.c_str(), logStaleSec, cpuStaleSec));
            }
            return;
        }
    }

    // 3) memory guard
    if (s.cfg.MaxWorkingSetMB > 0 && s.workingSetMB > (double)s.cfg.MaxWorkingSetMB)
    {
        KillAndSchedule(s, Format(L"working set %.0f MB exceeds the %d MB limit",
                                  s.workingSetMB, s.cfg.MaxWorkingSetMB), true);
        return;
    }
}

static bool AdoptService(ServiceRuntime& s, DWORD pid);   // defined below (used by StartService)

static void StartService(ServiceRuntime& s, int index)
{
    // Safety net: never launch a second copy of a server that is already running. The stop path
    // refuses to restart a surviving process, but a process could also appear from somewhere else
    // (started by hand, a leftover from a previous supervisor, a scheduled task). Whatever the
    // reason, one worldserver + one authserver is the only correct state.
    for (DWORD other : FindProcessesByExe(s.cfg.Exe))
    {
        if (other == s.pid || other == GetCurrentProcessId()) continue;
        if (s.cfg.AdoptExisting && AdoptService(s, other))
        {
            Log(Level::Warn, Format(L"[%s] pid %lu is already running with this exe - adopting it instead of "
                                    L"starting a second instance", s.cfg.Name.c_str(), (unsigned long)other));
            return;
        }
        s.consecutiveFailures++;
        s.lastEvent = L"start-refused";
        Log(Level::Error, Format(L"[%s] pid %lu already runs %s and AdoptExisting is off - refusing to start a "
                                 L"second instance", s.cfg.Name.c_str(), (unsigned long)other, s.cfg.Exe.c_str()));
        ScheduleRestart(s, GetRestartDelay(s), L"another instance is already running");
        return;
    }

    std::wstring error;
    if (!StartServiceProcess(s, index, error))
    {
        s.consecutiveFailures++;
        s.lastEvent = L"start-failed";
        Log(Level::Error, Format(L"[%s] %s", s.cfg.Name.c_str(), error.c_str()));
        ScheduleRestart(s, GetRestartDelay(s), L"start failed");
        return;
    }

    s.state = ServiceRuntime::State::Starting;
    s.stopPending = false;
    s.launchTick = Tick();
    s.nextCheckTick = Tick() + 1000;             // first health check after a second
    CaptureLogStart(s);                          // judge only what THIS process writes
    s.lastLogChangeTick = 0;
    s.logMtimeValid = false;
    s.carry.clear();
    s.startupReady = false;
    s.heartbeatSeen = false;
    s.heartbeatUnavailable = false;
    s.observedHeartbeatSec = 0;
    s.cadenceWarned = false;
    s.stallWarned = false;
    s.lastHeartbeatTick = 0;
    s.lastCpuChangeTick = Tick();
    s.cpuTotalLast = 0;
    s.probeFailures = 0;
    s.jobEmpty = false;
    s.lastEvent = L"started";

    Log(Level::Info, Format(L"[%s] started pid %lu (cwd %s, console %s)",
                            s.cfg.Name.c_str(), (unsigned long)s.pid, s.cfg.WorkDir.c_str(), s.cfg.Console.c_str()));
}

static bool AdoptService(ServiceRuntime& s, DWORD pid)
{
    // PROCESS_TERMINATE is what makes a later stop possible. Opening the handle without it (as an
    // earlier version did) leaves TerminateProcess() failing with ERROR_ACCESS_DENIED, and a restart
    // then produced a SECOND instance next to the surviving one (production incident 2026-09-23).
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    bool canTerminate = (h != NULL);
    if (!h) h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    s.hProcess = h;
    s.pid = pid;
    s.adopted = true;
    s.canTerminate = canTerminate;
    s.hJob = NULL;                                // we cannot re-parent a foreign process
    s.hPort = NULL;
    s.state = ServiceRuntime::State::Running;     // it is already running; no startup phase
    s.launchTick = Tick();
    s.nextCheckTick = Tick() + 1000;
    // Adoption keeps the old behaviour on purpose: there is no launch boundary for a process that
    // was already running, so read the log from the start and take what is in it as the state.
    s.logOffset = 0;
    s.lastLogSize = 0;
    s.logSkipOffset = 0;
    s.logSkipFingerprint.clear();
    s.logMtimeValid = false;
    s.carry.clear();
    s.startupReady = true;
    s.lastCpuChangeTick = Tick();
    s.observedHeartbeatSec = 0;
    s.cadenceWarned = false;
    s.lastEvent = L"adopted";
    Log(Level::Warn, Format(L"[%s] adopting the already running pid %lu (process tree kill unavailable "
                            L"for adopted processes)", s.cfg.Name.c_str(), (unsigned long)pid));
    if (!canTerminate)
    {
        Log(Level::Error, Format(L"[%s] pid %lu does not grant PROCESS_TERMINATE: it can be monitored but "
                                 L"NOT stopped, so it will never be restarted automatically. Stop it by hand and "
                                 L"let the supervisor start it (a hand-started server is also not in the "
                                 L"supervisor's job object, so closing the supervisor leaves it running).",
                                 s.cfg.Name.c_str(), (unsigned long)pid));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
//  control channel (file drop box used by the AGMP panel)
//
//  The panel (PHP) cannot control processes that live in the interactive session, so it writes a
//  small key=value file instead:
//
//      id=8f2c1  action=restart  target=worldserver
//
//  The supervisor takes ownership of the file (rename to <file>.running), executes the command,
//  deletes the file and publishes the outcome in the status JSON as "lastCommand".
// ---------------------------------------------------------------------------------------------
static int FindServiceIndex(const std::wstring& name)
{
    for (size_t i = 0; i < g_services.size(); ++i)
    {
        if (EqualsIgnoreCase(g_services[i].cfg.Name, name))
            return (int)i;
    }
    return -1;
}

static std::wstring ExecuteControlCommand(const std::wstring& id, const std::wstring& actionRaw,
                                          const std::wstring& targetRaw)
{
    (void)id;                                   // commands are correlated through g_lastCommand
    std::wstring action = ToLower(Trim(actionRaw));
    std::wstring target = ToLower(Trim(targetRaw));
    if (target.empty()) target = L"all";

    static const wchar_t* validActions[] = { L"ping", L"start", L"stop", L"restart", L"shutdown" };
    bool actionOk = false;
    for (const wchar_t* a : validActions)
        if (action == a) actionOk = true;

    std::vector<int> targets;
    if (target == L"all")
    {
        // a broadcast only touches the services THIS supervisor runs; a disabled one belongs to
        // another supervisor (a shared authserver is Enabled = false in every realm but one)
        for (size_t i = 0; i < g_services.size(); ++i)
            if (g_services[i].cfg.Enabled) targets.push_back((int)i);
    }
    else
    {
        int idx = FindServiceIndex(target);
        if (idx >= 0) targets.push_back(idx);
    }

    if (!actionOk)
        return L"rejected: unknown action '" + actionRaw + L"' (ping|start|stop|restart|shutdown)";
    if (targets.empty())
    {
        if (target == L"all")
            return L"rejected: this supervisor runs no enabled service (check Enabled in its ini)";

        return L"rejected: unknown target '" + targetRaw + L"' (use a configured service name or all)";
    }

    // Naming a service this supervisor does not run must be an ERROR, never a silent action:
    // StartService() would happily launch it, and on a machine where several realms share one
    // authserver that means a SECOND authserver next to the one another supervisor owns (production
    // 2026-09-24: one click on the wrong realm's page, or a hand-written control file, was enough).
    // "ping" is exempt on purpose: it only asks whether the control channel is alive and touches no
    // service.
    if (target != L"all" && action != L"ping" && !g_services[(size_t)targets[0]].cfg.Enabled)
    {
        return L"rejected: '" + g_services[(size_t)targets[0]].cfg.Name +
               L"' is disabled in this supervisor (Enabled = false) - another supervisor owns it";
    }

    if (action == L"ping")
        return L"ok: control channel alive";

    std::vector<std::wstring> results;
    for (int idx : targets)
    {
        ServiceRuntime& s = g_services[(size_t)idx];

        if (action == L"start")
        {
            if (IsAlive(s))
            {
                s.serviceStopped = false;
                s.stoppedByUser = false;
                results.push_back(s.cfg.Name + L": already running (pid " + std::to_wstring(s.pid) + L")");
                continue;
            }
            s.serviceStopped = false;
            s.stoppedByUser = false;
            s.consecutiveFailures = 0;
            Log(Level::Info, Format(L"[%s] panel requested start", s.cfg.Name.c_str()));
            StartService(s, idx);
            results.push_back(s.cfg.Name + L": start requested");
            continue;
        }

        if (action == L"stop")
        {
            if (!IsAlive(s))
            {
                s.state = ServiceRuntime::State::Stopped;
                s.serviceStopped = true;
                s.stoppedByUser = true;
                results.push_back(s.cfg.Name + L": already stopped");
                continue;
            }
            Log(Level::Warn, Format(L"[%s] panel requested stop - restarting is suppressed until a start command arrives",
                                    s.cfg.Name.c_str()));
            s.killedByUs = true;
            s.serviceStopped = true;
            s.stoppedByUser = true;
            s.lastEvent = L"stopped-by-panel";
            std::wstring how = StopServiceProcess(s, true);
            s.lastStopHow = how;
            if (IsAlive(s))
            {
                // keep tracking it: an operator that believes the server is stopped is worse than
                // one that is told the stop failed
                int retry = s.cfg.RestartDelaySeconds > 15 ? s.cfg.RestartDelaySeconds : 15;
                s.stopPending = true;
                s.restartAfterStop = false;         // the operator asked for "stopped", not "restarted"
                s.state = ServiceRuntime::State::WaitingRestart;
                s.nextActionTick = Tick() + (ULONGLONG)retry * 1000ull;
                Log(Level::Error, Format(L"[%s] stop failed (%s) - pid %lu is still running, retrying in %ds",
                                         s.cfg.Name.c_str(), how.c_str(), (unsigned long)s.pid, retry));
                results.push_back(s.cfg.Name + L": stop FAILED (" + how + L"), still running as pid " + std::to_wstring(s.pid));
                continue;
            }
            CloseProcessHandles(s);
            s.state = ServiceRuntime::State::Stopped;
            results.push_back(s.cfg.Name + L": stopped (" + how + L")");
            continue;
        }

        if (action == L"restart")
        {
            if (!IsAlive(s))
            {
                s.serviceStopped = false;
                s.stoppedByUser = false;
                s.consecutiveFailures = 0;
                Log(Level::Info, Format(L"[%s] panel requested restart (was not running) - starting",
                                        s.cfg.Name.c_str()));
                StartService(s, idx);
                results.push_back(s.cfg.Name + L": started");
                continue;
            }
            Log(Level::Info, Format(L"[%s] panel requested restart", s.cfg.Name.c_str()));
            s.killedByUs = true;
            std::wstring how = StopServiceProcess(s, true);
            s.lastStopHow = how;
            s.consecutiveFailures = 0;              // an operator-requested restart is not a failure
            s.serviceStopped = false;
            s.stoppedByUser = false;
            if (IsAlive(s))
            {
                // the old process survived: retry the stop in the background and only then start the
                // new one (the start path refuses to run two copies anyway)
                int retry = s.cfg.RestartDelaySeconds > 15 ? s.cfg.RestartDelaySeconds : 15;
                s.stopPending = true;
                s.restartAfterStop = true;
                s.state = ServiceRuntime::State::WaitingRestart;
                s.nextActionTick = Tick() + (ULONGLONG)retry * 1000ull;
                Log(Level::Error, Format(L"[%s] restart: stop failed (%s) - pid %lu is still running, retrying in %ds",
                                         s.cfg.Name.c_str(), how.c_str(), (unsigned long)s.pid, retry));
                results.push_back(s.cfg.Name + L": restart deferred, stop FAILED (" + how + L"), pid " + std::to_wstring(s.pid) + L" still running");
                continue;
            }
            CloseProcessHandles(s);
            StartService(s, idx);
            results.push_back(s.cfg.Name + L": restarted (" + how + L")");
            continue;
        }

        if (action == L"shutdown")
        {
            Log(Level::Warn, Format(L"[%s] panel requested supervisor shutdown - stopping this service",
                                    s.cfg.Name.c_str()));
            s.killedByUs = true;
            s.serviceStopped = true;
            s.stoppedByUser = true;
            s.lastEvent = L"stopped-by-panel";
            std::wstring how = StopServiceProcess(s, true);
            s.lastStopHow = how;
            if (IsAlive(s))
            {
                Log(Level::Error, Format(L"[%s] shutdown: pid %lu survived the stop (%s) - it will keep running "
                                         L"after the supervisor exits", s.cfg.Name.c_str(), (unsigned long)s.pid, how.c_str()));
                results.push_back(s.cfg.Name + L": stop FAILED (" + how + L"), pid " + std::to_wstring(s.pid) + L" still running");
                g_exitAfterCommand = true;
                continue;
            }
            CloseProcessHandles(s);
            s.state = ServiceRuntime::State::Stopped;
            results.push_back(s.cfg.Name + L": stopped (" + how + L")");
            g_exitAfterCommand = true;
            continue;
        }
    }

    std::wstring joined = L"ok: ";
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (i) joined += L"; ";
        joined += results[i];
    }
    return joined;
}

static void ProcessControlFile()
{
    if (g_gen.ControlFile.empty()) return;

    std::string bytes;
    if (!ReadFileBytes(g_gen.ControlFile, bytes) || bytes.empty())
        return;                                     // nothing pending

    // take ownership first: a crash or a repeated loop must not execute the same command twice
    std::wstring running = g_gen.ControlFile + L".running";
    DeleteFileW(running.c_str());
    if (!MoveFileExW(g_gen.ControlFile.c_str(), running.c_str(), MOVEFILE_REPLACE_EXISTING))
        return;                                     // the panel is still writing; try again next tick

    std::wstring text = Utf8ToWide(bytes);
    std::wstring id, action, target;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring raw = (nl == std::wstring::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;

        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = ToLower(Trim(line.substr(0, eq)));
        std::wstring val = Trim(line.substr(eq + 1));
        if (key == L"id") id = val;
        else if (key == L"action") action = val;
        else if (key == L"target") target = val;
    }

    g_lastCommand.Valid = true;
    g_lastCommand.Id = id;
    g_lastCommand.Action = ToLower(action);
    g_lastCommand.Target = ToLower(target.empty() ? L"all" : target);
    g_lastCommand.RequestedTick = Tick();

    Log(Level::Info, Format(L"control command received: id=%s action=%s target=%s",
                            id.empty() ? L"-" : id.c_str(), action.c_str(), g_lastCommand.Target.c_str()));

    std::wstring result = ExecuteControlCommand(id, action, target);

    g_lastCommand.Result = (result.rfind(L"ok", 0) == 0) ? L"ok"
                          : (result.rfind(L"rejected", 0) == 0) ? L"rejected" : L"failed";
    g_lastCommand.Message = result;
    g_lastCommand.DoneTick = Tick();
    Log(g_lastCommand.Result == L"ok" ? Level::Ok : Level::Warn,
        Format(L"control command %s -> %s", id.empty() ? L"-" : id.c_str(), result.c_str()));

    DeleteFileW(running.c_str());
}

// stop everything within the time the OS gives us on window close / logoff / shutdown
static void StopAllFast(DWORD graceMs)
{
    for (auto& s : g_services)
    {
        if (s.state == ServiceRuntime::State::Stopped || !IsAlive(s)) continue;
        Log(Level::Warn, Format(L"[%s] stopping pid %lu (console closing)", s.cfg.Name.c_str(), (unsigned long)s.pid));
        if (s.pid) GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, s.pid);
    }
    DWORD deadline = GetTickCount() + graceMs;
    for (auto& s : g_services)
    {
        if (!s.hProcess) continue;
        DWORD left = 0;
        DWORD now = GetTickCount();
        left = (now < deadline) ? (deadline - now) : 0;
        WaitForSingleObject(s.hProcess, left);
        if (IsAlive(s))
        {
            Log(Level::Warn, Format(L"[%s] forcing pid %lu", s.cfg.Name.c_str(), (unsigned long)s.pid));
            if (s.hJob) TerminateJobObject(s.hJob, 1);
            else TerminateProcess(s.hProcess, 1);
        }
    }
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    switch (type)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            if (InterlockedIncrement(&g_stopRequested) == 1)
                Log(Level::Warn, L"Ctrl+C received - stopping the supervised servers");
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            Log(Level::Warn, L"console closing / session ending - stopping the supervised servers");
            StopAllFast(4000);
            InterlockedExchange(&g_hardExit, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

// ---------------------------------------------------------------------------------------------
//  diagnostics: --once
// ---------------------------------------------------------------------------------------------
static int RunOnce()
{
    Log(Level::Info, Format(L"acore_supervisor %s - one-shot health check", SUPERVISOR_VERSION));
    for (auto& s : g_services)
    {
        if (!s.cfg.Enabled) continue;
        Log(Level::Info, Format(L"[%s] enabled=%s exe=%s", s.cfg.Name.c_str(), L"yes", s.cfg.Exe.c_str()));
        Log(Level::Info, Format(L"[%s] workdir=%s logfile=%s (%s)", s.cfg.Name.c_str(), s.cfg.WorkDir.c_str(),
                                s.cfg.LogFile.c_str(), EqualsIgnoreCase(s.cfg.LogFileSetting, L"auto") ? L"auto" : L"configured"));
        Log(Level::Info, Format(L"[%s] heartbeat pattern: %s", s.cfg.Name.c_str(),
                                s.cfg.HeartbeatPattern.empty() ? L"<none>" : s.cfg.HeartbeatPattern.c_str()));
        if (!s.cfg.HeartbeatPattern.empty())
        {
            Log(s.cfg.HeartbeatCadenceKnown ? Level::Info : Level::Warn,
                Format(L"[%s] heartbeat cadence from the server config: every %ds (RecordUpdateTimeDiffInterval), "
                       L"only when a tick exceeds %dms (MinRecordUpdateTimeDiff) -> the enforced timeout is %ds%s",
                       s.cfg.Name.c_str(), s.cfg.HeartbeatIntervalSec, s.cfg.HeartbeatMinRecordMs,
                       s.cfg.HeartbeatTimeoutEffectiveSec,
                       s.cfg.HeartbeatCadenceKnown ? L"" : L" (server config not readable - assuming the AzerothCore defaults)"));
        }
        Log(Level::Info, Format(L"[%s] probe: %s port %d", s.cfg.Name.c_str(), s.cfg.ProbeMode.c_str(), s.cfg.ProbePort));
        Log(Level::Info, Format(L"[%s] status file: %s", s.cfg.Name.c_str(), g_gen.StatusFile.c_str()));
        Log(Level::Info, Format(L"[%s] control file: %s", s.cfg.Name.c_str(), g_gen.ControlFile.c_str()));

        auto pids = FindProcessesByExe(s.cfg.Exe);
        Log(Level::Info, Format(L"[%s] running processes with this exe: %d", s.cfg.Name.c_str(), (int)pids.size()));
        if (!pids.empty())
        {
            Log(Level::Warn, Format(L"[%s] note: %s is already running by hand (pid %lu). With AdoptExisting the "
                                    L"supervisor will adopt it, but a hand-started process shares no console with the "
                                    L"supervisor, so it can only be killed - never shut down gracefully - and it is not "
                                    L"in the supervisor's job object. Stop it and let the supervisor start it.",
                                    s.cfg.Name.c_str(), s.cfg.Exe.c_str(), (unsigned long)pids[0]));
        }

        ServiceRuntime tmp;
        tmp.cfg = s.cfg;
        LogScan scan = ScanLogFile(tmp);
        if (scan.fileSeen)
        {
            Log(Level::Info, Format(L"[%s] log size %llu bytes; heartbeat present in the last chunk: %s",
                                    s.cfg.Name.c_str(), scan.size, scan.heartbeat ? L"yes" : L"no"));
        }
        else
        {
            Log(Level::Warn, Format(L"[%s] log file not found: %s", s.cfg.Name.c_str(), s.cfg.LogFile.c_str()));
        }

        if (s.cfg.ProbePort > 0)
        {
            WinProbe p = RunProbe(s.cfg);
            Log(p.ok ? Level::Ok : Level::Error,
                Format(L"[%s] probe result: %s (%s)", s.cfg.Name.c_str(), p.ok ? L"OK" : L"FAILED", p.detail.c_str()));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------------------------
static void PrintHelp()
{
    Log(Level::Info, Format(L"acore_supervisor %s - supervisor for AzerothCore worldserver and authserver", SUPERVISOR_VERSION));
    Log(Level::Info, L"usage: acore_supervisor.exe [--config <supervisor.ini>] [--once] [--help]");
    Log(Level::Info, L"  --config <file>   configuration file (default: supervisor.ini next to the exe)");
    Log(Level::Info, L"  --once            check configuration, log file and probes, then exit");
    Log(Level::Info, L"  --help            this text");
}

int wmain(int argc, wchar_t** argv)
{
    InitializeCriticalSection(&g_logLock);

    std::wstring configPath;
    bool once = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (EqualsIgnoreCase(a, L"--help") || EqualsIgnoreCase(a, L"-h")) { PrintHelp(); return 0; }
        else if (EqualsIgnoreCase(a, L"--once")) once = true;
        else if (EqualsIgnoreCase(a, L"--config") && i + 1 < argc) configPath = argv[++i];
    }
    if (configPath.empty())
        configPath = GetExeDirectory() + L"\\supervisor.ini";
    configPath = GetFullPath(configPath);

    std::wstring error;
    std::vector<ServiceConfig> loaded;
    if (!LoadConfig(configPath, g_gen, loaded, error))
    {
        Log(Level::Error, error);
        return 2;
    }
    for (auto& cfg : loaded)
    {
        ServiceRuntime rt;
        rt.cfg = cfg;
        g_services.push_back(rt);
    }

    // guard log
    g_logPath = g_gen.GuardLog;
    g_logMaxBytes = g_gen.GuardLogMaxMB * 1024 * 1024;
    EnsureDirectory(DirName(g_logPath));
    g_logReady = true;
    RotateLogIfNeeded();

    Log(Level::Info, L"=========================================================");
    Log(Level::Info, Format(L"acore_supervisor %s starting", SUPERVISOR_VERSION));
    Log(Level::Info, Format(L"  config file : %s", configPath.c_str()));
    Log(Level::Info, Format(L"  guard log   : %s", g_logPath.c_str()));
    Log(Level::Info, Format(L"  status file : %s", g_gen.StatusFile.c_str()));
    Log(Level::Info, Format(L"  control file: %s", g_gen.ControlFile.c_str()));
    for (auto& s : g_services)
    {
        Log(Level::Info, Format(L"  service %-11s: %s", s.cfg.Name.c_str(), s.cfg.Enabled ? L"enabled" : L"disabled"));
        if (!s.cfg.Enabled) continue;
        Log(Level::Info, Format(L"    exe       : %s", s.cfg.Exe.c_str()));
        Log(Level::Info, Format(L"    workdir   : %s", s.cfg.WorkDir.c_str()));
        Log(Level::Info, Format(L"    log       : %s", s.cfg.LogFile.c_str()));
        Log(Level::Info, Format(L"    heartbeat : %s (timeout %ds)", s.cfg.HeartbeatPattern.empty()
                                ? L"<none>" : s.cfg.HeartbeatPattern.c_str(), s.cfg.HeartbeatTimeoutEffectiveSec));
        if (!s.cfg.HeartbeatPattern.empty() && s.cfg.HeartbeatCadenceKnown)
        {
            Log(Level::Info, Format(L"    cadence   : server writes it every %ds (RecordUpdateTimeDiffInterval),"
                                    L" min tick %dms (MinRecordUpdateTimeDiff)", s.cfg.HeartbeatIntervalSec,
                                    s.cfg.HeartbeatMinRecordMs));
            if (s.cfg.HeartbeatTimeoutEffectiveSec != s.cfg.HeartbeatTimeoutSeconds)
                Log(Level::Warn, Format(L"[%s] HeartbeatTimeoutSeconds = %ds is shorter than the server's own "
                                        L"heartbeat interval (%ds): the rule would fire on a HEALTHY server. "
                                        L"Using %ds for this run. Set MinRecordUpdateTimeDiff = 0 and "
                                        L"RecordUpdateTimeDiffInterval = 60000 in worldserver.conf for a tight check.",
                                        s.cfg.Name.c_str(), s.cfg.HeartbeatTimeoutSeconds, s.cfg.HeartbeatIntervalSec,
                                        s.cfg.HeartbeatTimeoutEffectiveSec));
            else if (s.cfg.HeartbeatTimeoutSeconds <= s.cfg.HeartbeatIntervalSec)
                Log(Level::Warn, Format(L"[%s] HeartbeatTimeoutSeconds = %ds is not longer than the server's own "
                                        L"heartbeat interval (%ds) and HeartbeatTimeoutMode = strict keeps it: the rule "
                                        L"would fire on a HEALTHY server. Set MinRecordUpdateTimeDiff = 0 and "
                                        L"RecordUpdateTimeDiffInterval = 60000, or remove HeartbeatTimeoutMode = strict.",
                                        s.cfg.Name.c_str(), s.cfg.HeartbeatTimeoutSeconds, s.cfg.HeartbeatIntervalSec));
            if (s.cfg.HeartbeatMinRecordMs > 0)
                Log(Level::Error, Format(L"[%s] MinRecordUpdateTimeDiff = %dms: the heartbeat line is only written "
                                         L"when a world tick was slower than that, so on a quiet server it can be "
                                         L"absent for far longer than the interval - a widened timeout cannot fix "
                                         L"that. The heartbeat is NOT used as the health rule for this run; "
                                         L"log-activity + CPU monitoring is. Set it to 0.",
                                         s.cfg.Name.c_str(), s.cfg.HeartbeatMinRecordMs));
        }
        else if (!s.cfg.HeartbeatPattern.empty())
        {
            // Was silent before: an unreadable ServerConf left the bare HeartbeatTimeoutSeconds in
            // force with nothing in the log to say so.
            Log(Level::Error, Format(L"[%s] the server config %s could not be read, so the heartbeat cadence "
                                     L"(RecordUpdateTimeDiffInterval/MinRecordUpdateTimeDiff) is UNKNOWN and the "
                                     L"configured %ds timeout is an unverified guess. The heartbeat is used only "
                                     L"as a hint this run; log-activity + CPU monitoring is the health rule.",
                                     s.cfg.Name.c_str(), s.cfg.ConfFile.c_str(), s.cfg.HeartbeatTimeoutSeconds));
        }
        if (s.cfg.HeartbeatUnreliable && !s.cfg.HeartbeatPattern.empty())
            Log(Level::Warn, Format(L"    health    : log activity + CPU (the heartbeat is only advisory: the "
                                    L"server config shows it cannot be produced often enough)"));
        Log(Level::Info, Format(L"    probe     : %s", s.cfg.ProbePort > 0
                                ? Format(L"%s %s:%d (timeout %dms)", s.cfg.ProbeMode.c_str(),
                                         s.cfg.ProbeHost.c_str(), s.cfg.ProbePort, s.cfg.ProbeTimeoutMs).c_str()
                                : L"off"));
        Log(Level::Info, Format(L"    console   : %s", s.cfg.Console.c_str()));
        Log(Level::Info, Format(L"    policy    : poll %ds, stop grace %ds, restart %ds (planned %ds, backoff max %ds)",
                                s.cfg.PollSeconds, s.cfg.StopGraceSeconds, s.cfg.RestartDelaySeconds,
                                s.cfg.PlannedRestartDelaySeconds, s.cfg.MaxBackoffSeconds));
    }
    Log(Level::Info, L"=========================================================");

    if (once) return RunOnce();

    // single instance per config file
    std::wstring mutexName = L"Global\\AcoreSupervisor_" + g_gen.InstanceName;
    HANDLE mutex = CreateMutexW(NULL, FALSE, mutexName.c_str());
    bool haveMutex = false;
    if (mutex)
    {
        haveMutex = (WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0);
        if (!haveMutex)
        {
            Log(Level::Error, Format(L"another supervisor instance is already running (mutex %s)", mutexName.c_str()));
            return 1;
        }
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    g_runStartedTick = Tick();

    // start everything
    for (size_t i = 0; i < g_services.size(); ++i)
    {
        ServiceRuntime& s = g_services[i];
        if (!s.cfg.Enabled) continue;

        if (s.cfg.AdoptExisting)
        {
            auto pids = FindProcessesByExe(s.cfg.Exe);
            if (!pids.empty() && AdoptService(s, pids[0]))
                continue;
        }
        StartService(s, (int)i);
    }

    // main loop
    ULONGLONG lastStatusTick = 0;
    while (!g_hardExit)
    {
        if (g_stopRequested)
        {
            Log(Level::Info, L"stopping supervised servers on request");
            for (auto& s : g_services)
            {
                if (!s.cfg.Enabled) continue;
                if (s.state == ServiceRuntime::State::Stopped || !s.hProcess) continue;
                std::wstring how = StopServiceProcess(s, true);
                Log(Level::Info, Format(L"[%s] stop result: %s", s.cfg.Name.c_str(), how.c_str()));
                if (IsAlive(s))
                {
                    Log(Level::Error, Format(L"[%s] pid %lu survived the stop (%s) and will keep running after "
                                             L"this supervisor exits%s", s.cfg.Name.c_str(), (unsigned long)s.pid,
                                             how.c_str(),
                                             s.canTerminate ? L"" : L" (adopted without PROCESS_TERMINATE - stop it manually)"));
                    continue;                       // keep the handle so the log stays truthful
                }
                CloseProcessHandles(s);
                s.state = ServiceRuntime::State::Stopped;
            }
            break;
        }

        ULONGLONG now = Tick();
        bool anyEnabled = false;
        bool anyRunnable = false;

        // commands from the AGMP panel (start/stop/restart/ping/shutdown)
        ProcessControlFile();

        for (auto& s : g_services)
        {
            if (!s.cfg.Enabled) continue;
            anyEnabled = true;
            if (!s.serviceStopped) anyRunnable = true;
            switch (s.state)
            {
                case ServiceRuntime::State::Stopped:
                    break;
                case ServiceRuntime::State::WaitingRestart:
                    if (now >= s.nextActionTick)
                    {
                        if (s.stopPending)
                        {
                            // the previous instance survived an earlier stop: retry that first, never
                            // start a second copy next to it
                            std::wstring how = StopServiceProcess(s, true);
                            s.lastStopHow = how;
                            Log(Level::Warn, Format(L"[%s] retried stop of pid %lu: %s",
                                                    s.cfg.Name.c_str(), (unsigned long)s.pid, how.c_str()));
                            if (IsAlive(s))
                            {
                                int retry = s.cfg.RestartDelaySeconds > 15 ? s.cfg.RestartDelaySeconds : 15;
                                s.nextActionTick = now + (ULONGLONG)retry * 1000ull;
                                break;
                            }
                            s.stopPending = false;
                            CloseProcessHandles(s);
                            if (!s.restartAfterStop)
                            {
                                // the pending stop came from a "stop" request: honour it and stay down
                                s.state = ServiceRuntime::State::Stopped;
                                s.serviceStopped = true;
                                s.stoppedByUser = true;
                                Log(Level::Info, Format(L"[%s] the pending stop finally succeeded - staying stopped "
                                                        L"(send a start command to run it again)", s.cfg.Name.c_str()));
                                break;
                            }
                        }
                        StartService(s, (int)(&s - &g_services[0]));
                    }
                    break;
                default:
                {
                    if (!IsAlive(s))
                    {
                        HandleProcessExit(s);
                        break;
                    }
                    if (now >= s.nextCheckTick)
                    {
                        s.nextCheckTick = now + (ULONGLONG)s.cfg.PollSeconds * 1000ull;
                        HealthCheck(s, now);
                    }
                    break;
                }
            }
        }

        WriteStatusFile(g_gen, g_services);

        if (!anyEnabled)
        {
            Log(Level::Warn, L"no enabled service configured - supervisor exiting");
            break;
        }
        if (!anyRunnable && g_gen.ExitWhenAllStopped)
        {
            Log(Level::Info, L"all supervised services are stopped and ExitWhenAllStopped is on - supervisor exiting");
            break;
        }
        if (g_exitAfterCommand)
        {
            Log(Level::Info, L"shutdown command processed - supervisor exiting");
            break;
        }

        if (now - lastStatusTick >= 60000)
        {
            lastStatusTick = now;
            for (auto& s : g_services)
            {
                if (!s.cfg.Enabled) continue;
                if (s.state == ServiceRuntime::State::Stopped) continue;
                ULONGLONG uptime = s.launchTick ? (now - s.launchTick) / 1000 : 0;
                std::wstring extra;
                if (s.heartbeatSeen)
                {
                    std::wstring cadence = s.observedHeartbeatSec > 0
                                               ? Format(L" (cadence %ds)", s.observedHeartbeatSec)
                                               : L"";
                    extra = Format(L"hb %llus ago%s", AgeSeconds(now, s.lastHeartbeatTick), cadence.c_str());
                }
                else if (s.cfg.ProbePort > 0)
                    extra = Format(L"probe %s", s.probeOk ? L"ok" : L"FAIL");
                else
                    extra = Format(L"log %llus ago", AgeSeconds(now, s.lastLogChangeTick ? s.lastLogChangeTick : s.launchTick));
                Log(Level::Info, Format(L"[%s] %s pid %lu up %lluh%02llum, %s, cpu %llums, ws %.0fMB, restarts %d",
                                        s.cfg.Name.c_str(), StateName(s.state).c_str(), (unsigned long)s.pid,
                                        uptime / 3600, (uptime / 60) % 60, extra.c_str(),
                                        s.cpuTotalLast / 10000ull, s.workingSetMB, s.restarts));
            }
        }

        // wait: wake instantly when a supervised process exits
        HANDLE waits[3];
        DWORD n = 0;
        for (auto& s : g_services)
        {
            if (s.cfg.Enabled && s.hProcess && s.state != ServiceRuntime::State::Stopped)
                waits[n++] = s.hProcess;
        }
        DWORD timeout = (DWORD)g_gen.TickMs;
        if (n > 0) WaitForMultipleObjects(n, waits, FALSE, timeout);
        else Sleep(timeout);
    }

    // final cleanup
    for (auto& s : g_services)
    {
        if (s.hProcess && IsAlive(s)) StopServiceProcess(s, true);
        CloseProcessHandles(s);
    }
    WriteStatusFile(g_gen, g_services);
    Log(Level::Info, L"supervisor exiting");

    if (mutex && haveMutex) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    DeleteCriticalSection(&g_logLock);
    return 0;
}
