// ============================================================================================
//  fake_acore.cs - controllable stand-in for worldserver.exe / authserver.exe
// ============================================================================================
//  Emulates exactly the behaviour the supervisor keys on:
//    * world : writes the core's startup section + "WORLD: World Initialized In ..." marker,
//              then the world-loop heartbeat line "Update time diff: Nms with N players online"
//              every FAKE_HEARTBEAT_SEC seconds
//    * auth  : listens on FAKE_PORT and answers AUTH_LOGON_CHALLENGE
//    * modes : healthy | chatty (log noise WITHOUT heartbeat = frozen world loop) | hang
//              (nothing at all) | crash | clean | planned | nolisten | silent
//    * CTRL_BREAK handling like AzerothCore: clean shutdown, exit code 0
//
//  Environment:
//    FAKE_ROLE         world | auth | child
//    FAKE_LOG          log file (world/auth)
//    FAKE_LEDGER       append-only event log used by the test driver
//    FAKE_MODE         see above
//    FAKE_STARTUP_SEC  startup chatter seconds (default 2)
//    FAKE_HEARTBEAT_SEC heartbeat interval (default 5)
//    FAKE_STOP_SEC     when the mode-specific behaviour kicks in (default 15)
//    FAKE_EXIT_CODE    crash exit code (default 7)
//    FAKE_PORT         TCP port for the auth role
//    FAKE_CHILD_EXE    executable to spawn in role=child
//    FAKE_CHILD_LIFETIME seconds the child stays alive (default 600)
// ============================================================================================

using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class FakeAcore
{
    private static string logPath;
    private static string ledgerPath;
    private static string role = "world";
    private static string mode = "healthy";
    private static int startupSec = 2;
    private static int heartbeatSec = 5;
    private static int stopSec = 15;
    private static int exitCode = 7;
    private static int port;
    private static string childExe;
    private static int childLifetime = 600;

    private static FileStream logStream;
    private static readonly object LogLock = new object();
    private static volatile bool shuttingDown;
    private static TcpListener listener;

    private delegate bool HandlerRoutine(uint ctrlType);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleCtrlHandler(HandlerRoutine handler, bool add);
    private static HandlerRoutine handlerRef;

    private static void Main(string[] args)
    {
        // optional "--config <file>" with the same keys as the environment variables, so a test
        // can run the fake directly (tracked process = the fake, like the real server)
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], "--config", StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    foreach (string raw in File.ReadAllLines(args[i + 1]))
                    {
                        string line = raw.Trim();
                        if (line.Length == 0 || line[0] == '#' || line[0] == ';') continue;
                        int eq = line.IndexOf('=');
                        if (eq < 1) continue;
                        fileCfg[line.Substring(0, eq).Trim().ToUpperInvariant()] = line.Substring(eq + 1).Trim();
                    }
                }
                catch { }
            }
        }

        logPath = Env("FAKE_LOG", null);
        ledgerPath = Env("FAKE_LEDGER", null);
        role = Env("FAKE_ROLE", "world").ToLowerInvariant();
        mode = Env("FAKE_MODE", "healthy").ToLowerInvariant();
        startupSec = IntEnv("FAKE_STARTUP_SEC", 2);
        heartbeatSec = IntEnv("FAKE_HEARTBEAT_SEC", 5);
        stopSec = IntEnv("FAKE_STOP_SEC", 15);
        exitCode = IntEnv("FAKE_EXIT_CODE", 7);
        port = IntEnv("FAKE_PORT", 0);
        childExe = Env("FAKE_CHILD_EXE", null);
        childLifetime = IntEnv("FAKE_CHILD_LIFETIME", 600);

        Ledger("START pid=" + Process.GetCurrentProcess().Id + " role=" + role + " mode=" + mode);

        handlerRef = OnConsoleEvent;
        SetConsoleCtrlHandler(handlerRef, true);

        Console.WriteLine("[fake:{0}] pid={1} mode={2}", role, Process.GetCurrentProcess().Id, mode);

        if (role == "child")
        {
            Ledger("CHILD pid=" + Process.GetCurrentProcess().Id + " lifetime=" + childLifetime);
            Thread.Sleep(childLifetime * 1000);
            Ledger("CHILD-END pid=" + Process.GetCurrentProcess().Id);
            return;
        }

        if (role == "auth")
        {
            RunAuth();
            return;
        }

        RunWorld();
    }

    // ------------------------------------------------------------------ worldserver stand-in
    private static void RunWorld()
    {
        if (mode != "nolog" && !string.IsNullOrEmpty(logPath))
        {
            string dir = Path.GetDirectoryName(Path.GetFullPath(logPath));
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            logStream = new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite);
            Write("AzerothCore rev. fake-build (Win64, Release, Static) (worldserver-daemon)");
            Write("<Ctrl-C> to stop.");
            Write(" ");
            Write("Using configuration file       configs/worldserver.conf");
            for (int i = 0; i < Math.Max(1, startupSec); i++)
            {
                Write(">> Loading All Grids For Map " + (i + 1));
                Thread.Sleep(1000);
            }
            Write(" ");
            Write("WORLD: World Initialized In 0 Minutes " + Math.Max(1, startupSec) + " Seconds");
            Write("AzerothCore rev. fake-build (Win64, Release, Static) (worldserver-daemon) ready...");
        }

        if (mode == "crash" || mode == "clean" || mode == "planned")
        {
            Thread.Sleep(Math.Max(0, stopSec) * 1000);
            int code = mode == "clean" ? 0 : (mode == "planned" ? 2 : exitCode);
            Write("fake: exiting with code " + code);
            Ledger("EXIT pid=" + Process.GetCurrentProcess().Id + " code=" + code);
            Environment.Exit(code);
        }

        // a long lived child, to prove the job object kills the whole tree
        if (!string.IsNullOrEmpty(childExe) && File.Exists(childExe))
        {
            var psi = new ProcessStartInfo(childExe);
            psi.UseShellExecute = false;
            psi.EnvironmentVariables["FAKE_ROLE"] = "child";
            psi.EnvironmentVariables["FAKE_LEDGER"] = ledgerPath;
            psi.EnvironmentVariables["FAKE_CHILD_LIFETIME"] = childLifetime.ToString();
            var child = Process.Start(psi);
            Write("fake: spawned child pid " + child.Id);
            Ledger("PARENT-SPAWNED-CHILD pid=" + child.Id);
        }

        Stopwatch sw = Stopwatch.StartNew();
        int beat = 0;
        while (true)
        {
            Thread.Sleep(Math.Max(1, heartbeatSec) * 1000);
            beat++;
            double elapsed = sw.Elapsed.TotalSeconds;
            bool past = elapsed >= stopSec;

            if (mode == "hang" && past)
            {
                // frozen world loop: process alive, nothing written any more
                while (true) Thread.Sleep(1000);
            }

            if (mode == "chatty" && past)
            {
                // frozen world loop but a chatty background logger keeps writing to the file:
                // only the world-loop heartbeat rule can detect this
                while (true)
                {
                    Write("fake: background logger noise, heartbeat stopped");
                    Thread.Sleep(2000);
                }
            }

            Write("Update time diff: " + (8 + beat) + "ms with 0 players online");
            Write("Last 500 diffs summary:");
            Write("|- Mean: 9ms");
            Write("|- Median: 2ms");
            Write("|- Percentiles (95, 99, max): 21ms, 34ms, " + (40 + beat) + "ms");
        }
    }

    // ------------------------------------------------------------------ authserver stand-in
    private static void RunAuth()
    {
        if (mode == "crash" || mode == "clean" || mode == "planned")
        {
            Thread.Sleep(Math.Max(0, stopSec) * 1000);
            int code = mode == "clean" ? 0 : (mode == "planned" ? 2 : exitCode);
            Ledger("EXIT pid=" + Process.GetCurrentProcess().Id + " code=" + code);
            Environment.Exit(code);
        }

        if (mode != "nolisten")
        {
            try
            {
                listener = new TcpListener(IPAddress.Loopback, port);
                listener.Start();
                Ledger("LISTENING port=" + port);
                Console.WriteLine("[fake:auth] listening on 127.0.0.1:" + port);
                listener.BeginAcceptTcpClient(OnAccept, null);
            }
            catch (Exception ex)
            {
                Ledger("LISTEN-FAILED " + ex.Message);
            }
        }

        // quiet server: nothing is logged, exactly like the real authserver
        while (true) Thread.Sleep(1000);
    }

    private static void OnAccept(IAsyncResult ar)
    {
        TcpClient client = null;
        try { client = listener.EndAcceptTcpClient(ar); }
        catch { return; }
        try { listener.BeginAcceptTcpClient(OnAccept, null); } catch { }

        bool answer = (mode != "silent");
        Ledger("ACCEPT " + (answer ? "answer" : "silent"));
        ThreadPool.QueueUserWorkItem(_ =>
        {
            try
            {
                using (client)
                {
                    client.ReceiveTimeout = 5000;
                    var stream = client.GetStream();
                    byte[] header = new byte[4];
                    int got = 0;
                    while (got < header.Length)
                    {
                        int n = stream.Read(header, got, header.Length - got);
                        if (n <= 0) return;
                        got += n;
                    }
                    int size = (header[2] << 8) | header[3];
                    byte[] rest = new byte[size];
                    int have = 0;
                    while (have < size)
                    {
                        int n = stream.Read(rest, have, size - have);
                        if (n <= 0) break;
                        have += n;
                    }
                    if (!answer)
                    {
                        Thread.Sleep(30000);     // accept but never answer = wedged handler
                        return;
                    }
                    // AUTH_LOGON_CHALLENGE + WOW_FAIL_UNKNOWN_ACCOUNT (silent for the real server too)
                    byte[] resp = new byte[] { 0x00, 0x04 };
                    stream.Write(resp, 0, resp.Length);
                    stream.Flush();
                    Ledger("ANSWERED pid=" + Process.GetCurrentProcess().Id);
                }
            }
            catch { }
        });
    }

    // ------------------------------------------------------------------ helpers
    private static bool OnConsoleEvent(uint ctrlType)
    {
        if (shuttingDown) return true;
        shuttingDown = true;
        Write("fake: console control event " + ctrlType + " -> graceful shutdown");
        Ledger("GRACEFUL pid=" + Process.GetCurrentProcess().Id + " ctrl=" + ctrlType);
        Thread.Sleep(150);
        Environment.Exit(0);
        return true;
    }

    private static void Write(string line)
    {
        if (logStream == null) return;
        try
        {
            byte[] bytes = Encoding.UTF8.GetBytes(line + "\r\n");
            lock (LogLock)
            {
                logStream.Write(bytes, 0, bytes.Length);
                logStream.Flush();
            }
        }
        catch { }
    }

    private static void Ledger(string line)
    {
        if (string.IsNullOrEmpty(ledgerPath)) return;
        try
        {
            string dir = Path.GetDirectoryName(Path.GetFullPath(ledgerPath));
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.AppendAllText(ledgerPath, DateTime.Now.ToString("HH:mm:ss.fff") + " [" + role + "] " + line + Environment.NewLine, Encoding.UTF8);
        }
        catch { }
    }

    private static string Env(string name, string fallback)
    {
        string v = Environment.GetEnvironmentVariable(name);
        if (!string.IsNullOrEmpty(v)) return v;
        string fromFile;
        if (fileCfg.TryGetValue(name.ToUpperInvariant(), out fromFile) && !string.IsNullOrEmpty(fromFile))
            return fromFile;
        return fallback;
    }

    private static readonly System.Collections.Generic.Dictionary<string, string> fileCfg =
        new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

    private static int IntEnv(string name, int fallback)
    {
        int parsed;
        string v = Env(name, null);
        return (!string.IsNullOrEmpty(v) && int.TryParse(v, out parsed)) ? parsed : fallback;
    }
}
