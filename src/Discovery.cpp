// Discovery: while the WebSocket server listens, %LOCALAPPDATA%\BeDD\run\<pid>.json (the root is
// env BEDD_HOME when set) says where and with which token:
//   { "protocol": 2, "kind": "client|server", "pid", "url": "ws://127.0.0.1:<port>/v1", "token",
//     "instance", "instanceDir", "label", "modVersion", "startedAt" }
// It is written atomically (a temporary file renamed over it) and deleted at shutdown; readers
// ignore a file whose process is gone (a game that crashed leaves it behind).
#include "Discovery.h"
#include "BeDD.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>

#include <nlohmann/json.hpp>

#include "ll/api/mod/NativeMod.h"

namespace bedd {

namespace {

namespace fs = std::filesystem;

fs::path gFile; // written, to delete at shutdown

std::string utf8(std::wstring const& w) {
    if (w.empty()) return {};
    int         n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring env(wchar_t const* name) {
    wchar_t buf[1024]{};
    DWORD   n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

// The folder of the process's exe.
fs::path exeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

// Empty when neither BEDD_HOME nor LOCALAPPDATA is set (nowhere sensible to write).
fs::path runDir() {
    std::wstring home = env(L"BEDD_HOME");
    if (home.empty()) {
        std::wstring local = env(L"LOCALAPPDATA");
        if (local.empty()) return {};
        home = local + L"\\BeDD";
    }
    return fs::path(home) / L"run";
}

std::string nowRfc3339() {
    auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

} // namespace

std::string instanceName() { return utf8(exeDir().filename().wstring()); }

std::string label() { return utf8(env(L"BEDD_LABEL")); }

std::string modVersion() {
    auto self = ll::mod::NativeMod::current();
    if (!self) return {};
    auto const& v = self->getManifest().version;
    return v ? v->to_string() : std::string();
}

bool writeDiscovery(int port, std::string const& token) {
    nlohmann::json j{
        {"protocol",    2                                              },
#ifdef LL_PLAT_S
        {"kind",        "server"                                       },
#else
        {"kind",        "client"                                       },
#endif
        {"pid",         GetCurrentProcessId()                          },
        {"url",         std::format("ws://127.0.0.1:{}/v1", port)      },
        {"token",       token                                          },
        {"instance",    instanceName()                                 },
        {"instanceDir", utf8(exeDir().wstring())                       },
        {"label",       label()                                        },
        {"modVersion",  modVersion()                                   },
        {"startedAt",   nowRfc3339()                                   },
    };
    std::error_code ec;
    fs::path        dir = runDir();
    if (dir.empty()) {
        log().error("discovery: neither BEDD_HOME nor LOCALAPPDATA is set; no run file written");
        return false;
    }
    fs::create_directories(dir, ec);
    fs::path file = dir / std::format(L"{}.json", GetCurrentProcessId());
    fs::path tmp  = dir / std::format(L"{}.json.tmp", GetCurrentProcessId());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2);
        if (!out.flush()) {
            log().error("discovery: could not write {}", utf8(tmp.wstring()));
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log().error("discovery: could not rename {} ({})", utf8(tmp.wstring()), GetLastError());
        fs::remove(tmp, ec);
        return false;
    }
    gFile = file;
    log().info("discovery: {}", utf8(file.wstring()));
    return true;
}

void deleteDiscovery() {
    if (gFile.empty()) return;
    std::error_code ec;
    fs::remove(gFile, ec);
    gFile.clear();
}

} // namespace bedd
