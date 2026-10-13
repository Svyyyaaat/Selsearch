// SPDX-License-Identifier: MIT
//
// selsearch: the tiny client you bind to keys in your compositor. It sends one
// command to selsearchd over a Unix socket (Windows: a named pipe) and starts
// the daemon on demand. Deliberately free of Qt so a keypress costs ~1 ms of
// process startup. (On Windows the daemon registers its hotkeys itself; the
// CLI is for scripts and terminals.)

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <iterator>

#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern char **environ;
#endif

namespace {

const char *kUsage = R"(selsearch - local screen search and launcher

Usage:
  selsearch lens [--text|--translate|--ask]  Freeze the screen and select something
  selsearch launcher                         Toggle the launcher (bind to Alt+Space)
  selsearch launcher --show|--hide
  selsearch ask [TEXT]                       Open the launcher in AI mode
  selsearch ocr FILE                         Recognise text in an image file (JSON)
  selsearch config                           Open the settings
  selsearch reload                           Reload ~/.config/selsearch/config.toml
  selsearch status                           Show daemon status
  selsearch quit                             Stop the daemon

Hyprland 0.55+ (~/.config/hypr/hyprland.lua):
  hl.bind("ALT + Space", hl.dsp.exec_cmd("selsearch launcher"))
  hl.bind("SUPER + S", hl.dsp.exec_cmd("selsearch lens"))
Run `selsearch config` for the full snippet (autostart, other compositors).
)";

#ifndef _WIN32
std::string socketPath()
{
    if (const char *rt = std::getenv("XDG_RUNTIME_DIR"); rt && *rt)
        return std::string(rt) + "/selsearch.sock";
    return "/tmp/selsearch-" + std::to_string(getuid()) + ".sock";
}
#endif

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

#ifdef _WIN32

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring &s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// The daemon's QLocalServer("selsearch-<user>") listens on this pipe (see
// ipcSocketPath() in src/core/Ipc.cpp).
std::wstring pipePath()
{
    std::wstring user;
    if (const wchar_t *u = _wgetenv(L"USERNAME"); u && *u)
        user = u;
    std::erase(user, L'\\');
    return L"\\\\.\\pipe\\selsearch-" + (user.empty() ? std::wstring(L"user") : user);
}

HANDLE connectPipe()
{
    const std::wstring path = pipePath();
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        // All instances busy: wait for the server to offer a new one.
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(path.c_str(), 1000))
            break;
    }
    return INVALID_HANDLE_VALUE;
}

std::wstring daemonPath()
{
    // Prefer the selsearchd.exe next to this binary.
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    if (n > 0 && n < std::size(self)) {
        std::wstring dir(self, n);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        const std::wstring candidate = dir + L"\\selsearchd.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return L"selsearchd.exe";
}

bool startDaemon()
{
    const std::wstring path = daemonPath();
    std::wstring cmdLine = L"\"" + path + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    // Detached: no console, not tied to this terminal or its Ctrl+C.
    const bool qualified = path.find(L'\\') != std::wstring::npos;
    if (!CreateProcessW(qualified ? path.c_str() : nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return false;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int sendCommand(const std::string &json, bool autostart, int timeoutMs)
{
    // We were started by the user (a terminal, a shortcut): let the daemon
    // bring its window to the front, which Windows otherwise refuses.
    AllowSetForegroundWindow(ASFW_ANY);
    HANDLE pipe = connectPipe();
    if (pipe == INVALID_HANDLE_VALUE && autostart) {
        if (!startDaemon()) {
            std::fprintf(stderr, "selsearch: could not start selsearchd\n");
            return 2;
        }
        // The daemon needs a moment to load Qt and create the pipe.
        for (int i = 0; i < 100 && pipe == INVALID_HANDLE_VALUE; ++i) {
            Sleep(50);
            pipe = connectPipe();
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "selsearch: selsearchd is not running\n");
        return 2;
    }
    const std::string line = json + "\n";
    DWORD written = 0;
    if (!WriteFile(pipe, line.data(), DWORD(line.size()), &written, nullptr) || written != line.size()) {
        CloseHandle(pipe);
        return 2;
    }
    std::string reply;
    char buf[4096];
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(timeoutMs);
    while (reply.find('\n') == std::string::npos) {
        // Poll so the timeout holds without overlapped I/O.
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            break;  // closed
        if (available == 0) {
            if (GetTickCount64() > deadline)
                break;
            Sleep(5);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(pipe, buf, available < sizeof buf ? available : DWORD(sizeof buf), &n, nullptr) || n == 0)
            break;
        reply.append(buf, size_t(n));
    }
    CloseHandle(pipe);
    if (reply.empty()) {
        std::fprintf(stderr, "selsearch: no reply from selsearchd\n");
        return 2;
    }
    const bool ok = reply.find("\"ok\":true") != std::string::npos;
    // Print replies that carry data (status, ocr) or errors.
    if (!ok || reply.size() > 12)
        std::fputs(reply.c_str(), ok ? stdout : stderr);
    return ok ? 0 : 1;
}

#else  // POSIX

int connectSocket()
{
    const std::string path = socketPath();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

std::string daemonPath()
{
    // Prefer the selsearchd next to this binary (build tree, /usr/bin, ...).
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        std::string dir(self);
        dir = dir.substr(0, dir.rfind('/'));
        std::string candidate = dir + "/selsearchd";
        if (access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return "selsearchd";
}

bool startDaemon()
{
    std::string path = daemonPath();
    // Detach: new session, stdio to /dev/null.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    char *argv[] = {path.data(), nullptr};
    pid_t pid;
    int rc = path.find('/') == std::string::npos ? posix_spawnp(&pid, path.c_str(), &fa, &attr, argv, environ)
                                                 : posix_spawn(&pid, path.c_str(), &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc == 0;
}

int sendCommand(const std::string &json, bool autostart, int timeoutMs)
{
    int fd = connectSocket();
    if (fd < 0 && autostart) {
        if (!startDaemon()) {
            std::fprintf(stderr, "selsearch: could not start selsearchd\n");
            return 2;
        }
        // The daemon needs a moment to load Qt and bind the socket.
        for (int i = 0; i < 100 && fd < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            fd = connectSocket();
        }
    }
    if (fd < 0) {
        std::fprintf(stderr, "selsearch: selsearchd is not running\n");
        return 2;
    }
    std::string line = json + "\n";
    if (write(fd, line.data(), line.size()) != ssize_t(line.size())) {
        close(fd);
        return 2;
    }
    std::string reply;
    char buf[4096];
    pollfd pfd{fd, POLLIN, 0};
    while (reply.find('\n') == std::string::npos) {
        int r = poll(&pfd, 1, timeoutMs);
        if (r <= 0)
            break;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        reply.append(buf, size_t(n));
    }
    close(fd);
    if (reply.empty()) {
        std::fprintf(stderr, "selsearch: no reply from selsearchd\n");
        return 2;
    }
    const bool ok = reply.find("\"ok\":true") != std::string::npos;
    // Print replies that carry data (status, ocr) or errors.
    if (!ok || reply.size() > 12)
        std::fputs(reply.c_str(), ok ? stdout : stderr);
    return ok ? 0 : 1;
}

#endif

}  // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    // argv is in the ANSI code page; take the real (UTF-16) command line.
    std::vector<std::string> args;
    int wargc = 0;
    if (wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 1; i < wargc; ++i)
            args.push_back(narrow(wargv[i]));
        LocalFree(wargv);
    }
    SetConsoleOutputCP(CP_UTF8);  // replies are UTF-8 JSON
    (void)argc;
    (void)argv;
#else
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        std::fputs(kUsage, stdout);
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "--version" || args[0] == "version") {
        std::puts("selsearch " SELSEARCH_VERSION);
        return 0;
    }
    const std::string &cmd = args[0];
    std::string json;
    bool autostart = true;
    int timeout = 5000;

    if (cmd == "lens") {
        std::string mode = "region";
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--text") mode = "text";
            else if (args[i] == "--translate") mode = "translate";
            else if (args[i] == "--ask") mode = "ask";
        }
        json = R"({"cmd":"lens","mode":")" + mode + "\"}";
    } else if (cmd == "launcher" || cmd == "toggle") {
        std::string action = "toggle";
        if (args.size() > 1 && args[1] == "--show") action = "show";
        if (args.size() > 1 && args[1] == "--hide") action = "hide";
        json = R"({"cmd":"launcher","action":")" + action + "\"}";
    } else if (cmd == "ask") {
        std::string text;
        for (size_t i = 1; i < args.size(); ++i)
            text += (i > 1 ? " " : "") + args[i];
        json = R"({"cmd":"ask","text":")" + jsonEscape(text) + "\"}";
    } else if (cmd == "ocr") {
        if (args.size() < 2) {
            std::fputs("usage: selsearch ocr FILE [LANG...]\n", stderr);
            return 1;
        }
#ifdef _WIN32
        // The daemon has another working directory: send an absolute path.
        wchar_t full[MAX_PATH * 4];
        const std::wstring given = widen(args[1]);
        const DWORD len = GetFullPathNameW(given.c_str(), DWORD(std::size(full)), full, nullptr);
        if (len == 0 || len >= std::size(full) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
            std::fprintf(stderr, "selsearch: %s: No such file or directory\n", args[1].c_str());
            return 1;
        }
        const std::string real = narrow(std::wstring(full, len));
#else
        char real[PATH_MAX];
        if (!realpath(args[1].c_str(), real)) {
            std::fprintf(stderr, "selsearch: %s: %s\n", args[1].c_str(), std::strerror(errno));
            return 1;
        }
#endif
        std::string langs;
        for (size_t i = 2; i < args.size(); ++i)
            langs += std::string(i > 2 ? "," : "") + "\"" + jsonEscape(args[i]) + "\"";
        json = R"({"cmd":"ocr","path":")" + jsonEscape(real) + R"(","languages":[)" + langs + "]}";
        timeout = 60000;
    } else if (cmd == "config" || cmd == "settings") {
        json = R"({"cmd":"settings"})";
    } else if (cmd == "reload") {
        json = R"({"cmd":"reload"})";
        autostart = false;
    } else if (cmd == "status") {
        json = R"({"cmd":"status"})";
        autostart = false;
    } else if (cmd == "quit") {
        json = R"({"cmd":"quit"})";
        autostart = false;
    } else {
        std::fprintf(stderr, "selsearch: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
        return 1;
    }
    return sendCommand(json, autostart, timeout);
}
