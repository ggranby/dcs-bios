// DCS-BIOS Serial Bridge
// ──────────────────────────────────────────────────────────────────────────
// Receives the DCS-BIOS export stream (UDP multicast 239.255.50.10:5010 or
// TCP 127.0.0.1:7778), parses it into an in-memory 64-KB state map, and
// dispatches delta frames to connected COM-port devices.
//
// Each device undergoes an optional handshake to negotiate:
//   – RS-485 topology role (standalone / master / slave)
//   – subscription list (which address words it wants)
//   – bidirectional flag (device can also send import commands to DCS)
//
// Import commands received from bidir devices are forwarded to DCS via UDP
// to 127.0.0.1:7778 in the standard DCS-BIOS plain-text format.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <shellapi.h>

#include "BiosProtocol.hpp"
#include "ControlDatabase.hpp"
#include "DeviceRegistry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

using namespace dcsbios;

// ─── Window message and control IDs ──────────────────────────────────────────
constexpr int kUdpPort   = 5010;
constexpr int kTcpPort   = 7778;
constexpr int kBufferSize = 4096;
constexpr int kHotkeyId  = 1;
constexpr int kLogMessage          = WM_APP + 1;
constexpr int kBridgeStoppedMessage = WM_APP + 2;
constexpr UINT_PTR kLogFlushTimer = 42;
constexpr UINT_PTR kStatusRefreshTimer = 43;
constexpr UINT_PTR kLogFlushIntervalMs = 50;
constexpr UINT_PTR kStatusRefreshIntervalMs = 250;
constexpr size_t kMaxPendingUiLines = 2000;
constexpr size_t kMaxUiDisplayLines = 400;

constexpr UINT_PTR kControlMode       = 1001;
constexpr UINT_PTR kControlPorts      = 1002;
constexpr UINT_PTR kControlAutoDetect = 1003;
constexpr UINT_PTR kControlToggle     = 1004;
constexpr UINT_PTR kControlStatus     = 1005;
constexpr UINT_PTR kControlLog        = 1006;
constexpr UINT_PTR kControlDryRun     = 1007;
constexpr UINT_PTR kControlExportLog  = 1008;
constexpr UINT_PTR kControlLogChanges = 1009;
constexpr UINT_PTR kControlSelfTest   = 1010;
constexpr UINT_PTR kControlRawKnobs   = 1011;
constexpr UINT_PTR kControlRawGauges  = 1012;

// ─── Dark-theme colours ───────────────────────────────────────────────────────
constexpr COLORREF kColorBg        = RGB(26,  28,  34);
constexpr COLORREF kColorPanel     = RGB(44,  48,  56);
constexpr COLORREF kColorText      = RGB(225, 230, 240);
constexpr COLORREF kColorTextMuted = RGB(155, 165, 180);
constexpr COLORREF kColorInputBg   = RGB(35,  38,  46);
constexpr COLORREF kColorButtonBg  = RGB(30,  33,  40);

// ─── Config / startup structs ─────────────────────────────────────────────────
struct BridgeConfig {
    bool useUdp          = true;
    bool dryRun          = false;
    bool logStateChanges = false;
    bool logRawKnobsDials = true;
    bool logRawGauges = false;
    std::vector<int> comPorts;
    std::string jsonDir;   // path to DCS-BIOS JSON files (auto-detected if empty)
};

struct StartupOptions {
    bool autoStart = false;
    bool dryRun    = false;
    std::optional<bool>         forceUdp;
    std::optional<std::vector<int>> ports;
};

// ─── Utility helpers ─────────────────────────────────────────────────────────

void PostLog(HWND hwnd, const std::wstring& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    wchar_t timestamp[32];
    wcsftime(timestamp, sizeof(timestamp) / sizeof(wchar_t), L"[%H:%M:%S]", &timeinfo);
    auto* payload = new std::wstring(std::wstring(timestamp) + L" " + message + L"\r\n");
    PostMessage(hwnd, kLogMessage, 0, reinterpret_cast<LPARAM>(payload));
}

std::wstring Trim(const std::wstring& input) {
    size_t start = 0;
    while (start < input.size() && iswspace(input[start])) ++start;
    size_t end = input.size();
    while (end > start && iswspace(input[end - 1])) --end;
    return input.substr(start, end - start);
}

std::vector<int> ParsePorts(const std::wstring& raw) {
    std::wstring s = raw;
    std::replace(s.begin(), s.end(), L';', L',');
    std::replace(s.begin(), s.end(), L' ', L',');
    std::vector<int> ports;
    std::wstringstream ss(s);
    std::wstring chunk;
    while (std::getline(ss, chunk, L',')) {
        chunk = Trim(chunk);
        if (chunk.empty()) continue;
        int v = _wtoi(chunk.c_str());
        if (v > 0 && std::find(ports.begin(), ports.end(), v) == ports.end())
            ports.push_back(v);
    }
    return ports;
}

bool StartsWithI(const std::wstring& value, const std::wstring& prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (towlower(value[i]) != towlower(prefix[i])) return false;
    return true;
}

StartupOptions ParseStartupOptions() {
    StartupOptions opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--autostart") { opt.autoStart = true; continue; }
        if (arg == L"--dry-run")   { opt.dryRun    = true; continue; }
        if (arg == L"--udp")       { opt.forceUdp  = true; continue; }
        if (arg == L"--tcp")       { opt.forceUdp  = false; continue; }
        if (StartsWithI(arg, L"--ports=")) {
            opt.ports = ParsePorts(arg.substr(8)); continue;
        }
        int p = _wtoi(arg.c_str());
        if (p > 0) opt.ports = std::vector<int>{p};
    }
    LocalFree(argv);
    return opt;
}

std::vector<int> DetectAvailableComPorts() {
    std::vector<int> ports;
    for (int i = 1; i <= 256; ++i) {
        wchar_t name[16] = {}, path[512] = {};
        swprintf_s(name, L"COM%d", i);
        if (QueryDosDeviceW(name, path, static_cast<DWORD>(std::size(path))) != 0)
            ports.push_back(i);
    }
    return ports;
}

std::wstring JoinPorts(const std::vector<int>& ports) {
    std::wstringstream out;
    for (size_t i = 0; i < ports.size(); ++i) {
        if (i > 0) out << L", ";
        out << ports[i];
    }
    return out.str();
}

bool ConfigureSerialPort(HANDLE h) {
    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) return false;
    dcb.BaudRate     = 250000;
    dcb.ByteSize     = 8;
    dcb.Parity       = NOPARITY;
    dcb.StopBits     = ONESTOPBIT;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb)) return false;
    COMMTIMEOUTS ct = {};
    ct.ReadIntervalTimeout        = MAXDWORD;
    ct.ReadTotalTimeoutMultiplier = 0;
    ct.ReadTotalTimeoutConstant   = 0;
    ct.WriteTotalTimeoutMultiplier= 0;
    ct.WriteTotalTimeoutConstant  = 50;
    return SetCommTimeouts(h, &ct) == TRUE;
}

// Search for DCS-BIOS JSON control-reference directory near the executable.
static std::string FindJsonDir() {
    auto hasJsonFiles = [](const std::wstring& dir) {
        WIN32_FIND_DATAW findData = {};
        std::wstring pattern = dir + L"\\*.json";
        HANDLE h = FindFirstFileW(pattern.c_str(), &findData);
        if (h == INVALID_HANDLE_VALUE) return false;
        FindClose(h);
        return true;
    };

    auto toUtf8 = [](const std::wstring& path) -> std::string {
        int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string result(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, result.data(), n, nullptr, nullptr);
        return result;
    };

    std::vector<std::wstring> candidates;

    wchar_t userProfile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH) > 0) {
        candidates.push_back(std::wstring(userProfile) + L"\\Saved Games\\DCS\\Scripts\\DCS-BIOS\\doc\\json");
        candidates.push_back(std::wstring(userProfile) + L"\\Saved Games\\DCS.openbeta\\Scripts\\DCS-BIOS\\doc\\json");
        candidates.push_back(std::wstring(userProfile) + L"\\Saved Games\\DCS.openalpha\\Scripts\\DCS-BIOS\\doc\\json");
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    auto slash = dir.rfind(L'\\');
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);

    const std::vector<std::wstring> suffixes = {
        L"\\json",
        L"\\..\\..\\Scripts\\DCS-BIOS\\doc\\json",
        L"\\..\\..\\..\\Scripts\\DCS-BIOS\\doc\\json",
        L"\\..\\..\\..\\..\\Scripts\\DCS-BIOS\\doc\\json",
    };
    for (const auto& suffix : suffixes) {
        wchar_t buf[MAX_PATH] = {};
        if (PathCanonicalizeW(buf, (dir + suffix).c_str())) {
            candidates.push_back(buf);
        }
    }

    for (const auto& candidate : candidates) {
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!hasJsonFiles(candidate)) continue;
        return toUtf8(candidate);
    }
    return {};
}

// ─── BridgeController ─────────────────────────────────────────────────────────

class BridgeController {
public:
    struct DispatchMetricsSnapshot {
        uint64_t framesObserved = 0;
        uint64_t framesWithSerialWrites = 0;
        uint64_t avgDispatchUs = 0;
        uint64_t maxDispatchUs = 0;
        uint64_t avgPortWriteUs = 0;
        uint64_t maxPortWriteUs = 0;
    };

    explicit BridgeController(HWND hwnd) : hwnd_(hwnd), exportParser_(stateMap_) {}
    ~BridgeController() { Stop(); }

    bool Start(const BridgeConfig& config) {
        if (running_) return false;

        if (!config.dryRun && config.comPorts.empty()) {
            PostLog(hwnd_, L"No COM ports specified.");
            return false;
        }

        WSADATA wd = {};
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            PostLog(hwnd_, L"WSAStartup failed.");
            return false;
        }
        winsockInitialized_ = true;
        config_ = config;

        // Load control reference JSON (optional — failure is non-fatal)
        std::string jsonDir = config.jsonDir.empty() ? FindJsonDir() : config.jsonDir;
        if (!jsonDir.empty()) {
            size_t n = controlDb_.load(jsonDir, "FA-18C_hornet");
            if (n > 0) {
                std::wstringstream m;
                m << L"Control database (FA-18C_hornet): " << n << L" controls loaded.";
                PostLog(hwnd_, m.str());
            } else {
                PostLog(hwnd_, L"Control database: no JSON found (state-change labels disabled).");
            }
        } else {
            PostLog(hwnd_, L"Control database path not found (state-change labels disabled).");
        }

        logStateChanges_ = config.logStateChanges;
        logStateChangesPrimed_ = false;
        lastLoggedValues_.clear();
        logStateChangesArmedAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        logStateChangesArmedNoticeSent_ = false;
        exportParser_.onFrameSync = [this]() { OnFrameSync(); };

        if (!OpenSerialPorts()) {
            CleanupResources();
            return false;
        }
        InitImportSocket();

        running_ = true;
        frameCounter_ = 0;
        dispatchFramesMeasured_ = 0;
        dispatchFramesWithWrites_ = 0;
        portWritesMeasured_ = 0;
        avgDispatchUs_ = 0;
        maxDispatchUs_ = 0;
        avgPortWriteUs_ = 0;
        maxPortWriteUs_ = 0;
        worker_ = std::thread(&BridgeController::RunLoop, this);
        PostLog(hwnd_, L"Bridge started.");
        return true;
    }

    void Stop() {
        bool hadWork = running_ || worker_.joinable() || !ports_.empty() || winsockInitialized_;
        running_ = false;
        if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
        if (worker_.joinable()) worker_.join();
        for (auto& sp : ports_) {
            sp->readRunning = false;
            if (sp->readThread.joinable()) sp->readThread.join();
        }
        CleanupResources();
        if (hadWork) PostLog(hwnd_, L"Bridge stopped.");
    }

    bool IsRunning() const { return running_; }

    DispatchMetricsSnapshot GetDispatchMetrics() const {
        return DispatchMetricsSnapshot{
            dispatchFramesMeasured_.load(),
            dispatchFramesWithWrites_.load(),
            avgDispatchUs_.load(),
            maxDispatchUs_.load(),
            avgPortWriteUs_.load(),
            maxPortWriteUs_.load(),
        };
    }

    // In-process self-test: exercises protocol parser, state machine, and
    // import line parser with synthetic data.
    void RunSelfTest() {
        PostLog(hwnd_, L"--- Self-Test Begin ---");

        BiosStateMap testMap;
        dcsbios::ExportParser testParser(testMap);
        int syncCount = 0;
        testParser.onFrameSync = [&]() { ++syncCount; };

        // Synthetic stream: sync + write addr=0x0100 len=4 (two words).
        // A UDP datagram contains one full frame, so flushFrame() completes it.
        const uint8_t stream[] = {
            0x55, 0x55, 0x55, 0x55,
            0x00, 0x01,  // addr = 0x0100
            0x04, 0x00,  // len  = 4 bytes
            0x01, 0x00,  // word @ 0x0100 = 1
            0x02, 0x00,  // word @ 0x0102 = 2
        };
        testParser.processBytes(stream, sizeof(stream));
        testParser.flushFrame();
        auto dirty = testMap.takeDirty();

        bool pass = true;
        auto fail = [&](const wchar_t* msg) { PostLog(hwnd_, msg); pass = false; };
        if (syncCount < 1)                      fail(L"  FAIL: no sync frame detected.");
        if (dirty.size() != 2)                  fail(L"  FAIL: expected 2 dirty words.");
        if (testMap.readWord(0x0100) != 0x0001) fail(L"  FAIL: word at 0x0100 wrong.");
        if (testMap.readWord(0x0102) != 0x0002) fail(L"  FAIL: word at 0x0102 wrong.");

        // Test import line parser
        ImportLineParser lp;
        ImportCommand got;
        bool gotCmd = false;
        lp.onCommand = [&](const ImportCommand& c) { got = c; gotCmd = true; };
        const uint8_t line[] = "UFC_COMM1_CHANNEL_SELECT SET_STATE 3\n";
        lp.processBytes(line, sizeof(line) - 1);
        if (!gotCmd || got.identifier != "UFC_COMM1_CHANNEL_SELECT"
                    || got.action     != "SET_STATE"
                    || got.value      != "3") {
            fail(L"  FAIL: import line parser produced wrong result.");
        }

        PostLog(hwnd_, pass ? L"  PASS: all checks passed." : L"  Some checks FAILED — see above.");
        PostLog(hwnd_, L"--- Self-Test End ---");
    }

private:
    // ── Per-device record ────────────────────────────────────────────────────
    struct SerialPort {
        int            comPort    = 0;
        HANDLE         handle     = INVALID_HANDLE_VALUE;
        DeviceInfo     info;
        std::thread    readThread;
        ImportLineParser importParser;
        std::atomic<bool> readRunning{false};

        SerialPort()                             = default;
        SerialPort(const SerialPort&)            = delete;
        SerialPort& operator=(const SerialPort&) = delete;
    };

    // ── Open serial ports + handshake ────────────────────────────────────────
    bool OpenSerialPorts() {
        if (config_.dryRun) {
            PostLog(hwnd_, L"Dry-run: serial writes disabled (state machine active).");
            return true;
        }
        bool openedAny = false;
        for (int comPort : config_.comPorts) {
            wchar_t path[32] = {};
            swprintf_s(path, L"\\\\.\\COM%d", comPort);
            HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                std::wstringstream m;
                m << L"Failed to open COM" << comPort << L" (err " << GetLastError() << L").";
                PostLog(hwnd_, m.str());
                continue;
            }
            if (!ConfigureSerialPort(h)) {
                std::wstringstream m;
                m << L"Failed to configure COM" << comPort << L".";
                PostLog(hwnd_, m.str());
                CloseHandle(h);
                continue;
            }
            auto sp = std::make_unique<SerialPort>();
            sp->comPort       = comPort;
            sp->handle        = h;
            sp->info.comPort  = "COM" + std::to_string(comPort);

            PerformHandshake(*sp);

            if (sp->info.bidir) {
                sp->readRunning = true;
                SerialPort* raw = sp.get();
                sp->readThread  = std::thread(&BridgeController::ReadDeviceThread, this, raw);
            }
            openedAny = true;
            ports_.push_back(std::move(sp));
        }
        if (!openedAny) PostLog(hwnd_, L"No COM ports could be opened.");
        return openedAny;
    }

    // ── Handshake ───────────────────────────────────────────────────────────
    void PerformHandshake(SerialPort& sp) {
        auto ping = HandshakeParser::pingFrame();
        DWORD written = 0;
        WriteFile(sp.handle, ping.data(), static_cast<DWORD>(ping.size()), &written, nullptr);

        COMMTIMEOUTS ct = {};
        ct.ReadTotalTimeoutConstant = 300;
        ct.ReadIntervalTimeout      = 50;
        SetCommTimeouts(sp.handle, &ct);

        HandshakeParser parser;
        uint8_t b = 0;
        DWORD bytesRead = 0;
        bool completed = false;
        auto deadline = std::chrono::steady_clock::now() + dcsbios::kHandshakeTimeout;

        while (std::chrono::steady_clock::now() < deadline && running_) {
            bytesRead = 0;
            if (ReadFile(sp.handle, &b, 1, &bytesRead, nullptr) && bytesRead == 1) {
                auto r = parser.processByte(b);
                if (r == HandshakeParser::Result::Complete) { completed = true; break; }
                if (r == HandshakeParser::Result::Failed)   break;
            }
        }
        ConfigureSerialPort(sp.handle);  // restore normal timeouts

        if (completed) {
            parser.populateDevice(sp.info);
            auto ack = HandshakeParser::ackFrame();
            WriteFile(sp.handle, ack.data(), static_cast<DWORD>(ack.size()), &written, nullptr);

            std::wstring name(sp.info.deviceName.begin(), sp.info.deviceName.end());
            std::wstringstream m;
            m << L"COM" << sp.comPort << L": ";
            switch (sp.info.role) {
            case DeviceRole::RS485Master: m << L"RS-485 Master"; break;
            case DeviceRole::RS485Slave:  m << L"RS-485 Slave";  break;
            default:                      m << L"Standalone";     break;
            }
            if (!name.empty())    m << L" \"" << name << L"\"";
            if (sp.info.bidir)    m << L" [bidir]";
            if (sp.info.wantsAll) m << L" [all channels]";
            else                  m << L" [" << sp.info.subAddrs.size() << L" subscriptions]";
            PostLog(hwnd_, m.str());
        } else {
            sp.info.role          = DeviceRole::Legacy;
            sp.info.handshakeDone = true;
            sp.info.wantsAll      = true;
            sp.info.bidir         = false;
            std::wstringstream m;
            m << L"COM" << sp.comPort << L": Legacy (full stream, no handshake).";
            PostLog(hwnd_, m.str());
        }
    }

    void CloseSerialPorts() {
        for (auto& sp : ports_) {
            sp->readRunning = false;
            if (sp->readThread.joinable()) sp->readThread.join();
            if (sp->handle != INVALID_HANDLE_VALUE) {
                CloseHandle(sp->handle);
                sp->handle = INVALID_HANDLE_VALUE;
            }
        }
        ports_.clear();
    }

    // ── Import socket (device → DCS) ─────────────────────────────────────────
    void InitImportSocket() {
        importSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (importSocket_ == INVALID_SOCKET) {
            PostLog(hwnd_, L"Warning: import socket failed (device→DCS disabled).");
            return;
        }
        importAddr_ = {};
        importAddr_.sin_family = AF_INET;
        importAddr_.sin_port   = htons(7778);
        inet_pton(AF_INET, "127.0.0.1", &importAddr_.sin_addr);
    }

    void SendImportCommand(const ImportCommand& cmd) {
        if (importSocket_ == INVALID_SOCKET) return;
        std::string line = cmd.toLine();
        sendto(importSocket_, line.c_str(), static_cast<int>(line.size()), 0,
               reinterpret_cast<sockaddr*>(&importAddr_), sizeof(importAddr_));
    }

    // ── Per-device read thread (bidirectional) ───────────────────────────────
    void ReadDeviceThread(SerialPort* sp) {
        COMMTIMEOUTS ct = {};
        ct.ReadIntervalTimeout        = 100;
        ct.ReadTotalTimeoutMultiplier = 0;
        ct.ReadTotalTimeoutConstant   = 100;
        SetCommTimeouts(sp->handle, &ct);

        sp->importParser.onCommand = [this, sp](const ImportCommand& cmd) {
            SendImportCommand(cmd);
            std::wstring id(cmd.identifier.begin(), cmd.identifier.end());
            std::wstring act(cmd.action.begin(), cmd.action.end());
            std::wstring val(cmd.value.begin(), cmd.value.end());
            std::wstringstream m;
            m << L"COM" << sp->comPort << L" \u2192 DCS: " << id << L" " << act;
            if (!val.empty()) m << L" " << val;
            PostLog(hwnd_, m.str());
        };

        std::vector<uint8_t> buf(256);
        while (sp->readRunning) {
            DWORD br = 0;
            if (ReadFile(sp->handle, buf.data(), static_cast<DWORD>(buf.size()), &br, nullptr) && br > 0)
                sp->importParser.processBytes(buf.data(), br);
        }
    }

    void CleanupResources() {
        std::lock_guard<std::mutex> lock(resourceMutex_);
        CloseSerialPorts();
        if (importSocket_ != INVALID_SOCKET) { closesocket(importSocket_); importSocket_ = INVALID_SOCKET; }
        if (winsockInitialized_) { WSACleanup(); winsockInitialized_ = false; }
    }

    // ── Network socket setup ─────────────────────────────────────────────────
    bool InitializeUdpSocket() {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) { PostLog(hwnd_, L"Failed to create UDP socket."); return false; }

        sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(kUdpPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        BOOL reuse = TRUE;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            // Set receive timeout to 5 seconds to prevent indefinite blocking
            DWORD rcvTimeout = 5000;
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            PostLog(hwnd_, L"Failed to bind UDP socket to :5010."); return false;
        }
        ip_mreq membership = {};
        inet_pton(AF_INET, "239.255.50.10", &membership.imr_multiaddr);
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&membership), sizeof(membership)) == SOCKET_ERROR) {
            PostLog(hwnd_, L"Failed to join multicast group."); return false;
        }
        PostLog(hwnd_, L"UDP mode active on 239.255.50.10:5010.");
        return true;
    }

    bool InitializeTcpSocket() {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) { PostLog(hwnd_, L"Failed to create TCP socket."); return false; }
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(kTcpPort);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            PostLog(hwnd_, L"TCP connect to 127.0.0.1:7778 failed."); return false;
        }
        PostLog(hwnd_, L"TCP mode connected to 127.0.0.1:7778.");
        return true;
    }

    // ── Frame dispatch (called synchronously from ExportParser on sync) ───────
    void OnFrameSync() {
        ++frameCounter_;
        auto dirty = stateMap_.takeDirty();
        uint64_t fc = frameCounter_.load();
        auto dispatchStartedAt = std::chrono::steady_clock::now();
        uint64_t portWritesThisFrame = 0;

        // Dispatch delta to devices (skip in dry-run)
        if (!config_.dryRun) {
            for (auto& sp : ports_) {
                if (sp->handle == INVALID_HANDLE_VALUE) continue;
                auto frame = BuildDeltaFrame(stateMap_, dirty, sp->info);
                if (!frame.empty()) {
                    auto writeStartedAt = std::chrono::steady_clock::now();
                    DWORD written = 0;
                    BOOL ok = WriteFile(sp->handle, frame.data(),
                                        static_cast<DWORD>(frame.size()), &written, nullptr);
                    auto writeFinishedAt = std::chrono::steady_clock::now();
                    uint64_t writeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(writeFinishedAt - writeStartedAt).count());
                    ++portWritesThisFrame;
                    uint64_t measuredPortWrites = ++portWritesMeasured_;
                    uint64_t prevAvgPortWriteUs = avgPortWriteUs_.load();
                    int64_t portDeltaUs = static_cast<int64_t>(writeUs) - static_cast<int64_t>(prevAvgPortWriteUs);
                    avgPortWriteUs_ = static_cast<uint64_t>(static_cast<int64_t>(prevAvgPortWriteUs) + (portDeltaUs / static_cast<int64_t>(measuredPortWrites)));
                    maxPortWriteUs_ = std::max(maxPortWriteUs_.load(), writeUs);
                    if (!ok || written != static_cast<DWORD>(frame.size())) {
                        std::wstringstream m;
                        m << L"Write failed on COM" << sp->comPort << L".";
                        PostLog(hwnd_, m.str());
                    }
                }
            }
        }

        uint64_t frameDispatchUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - dispatchStartedAt).count());
        uint64_t dispatchFramesMeasured = ++dispatchFramesMeasured_;
        uint64_t prevAvgDispatchUs = avgDispatchUs_.load();
        int64_t dispatchDeltaUs = static_cast<int64_t>(frameDispatchUs) - static_cast<int64_t>(prevAvgDispatchUs);
        avgDispatchUs_ = static_cast<uint64_t>(static_cast<int64_t>(prevAvgDispatchUs) + (dispatchDeltaUs / static_cast<int64_t>(dispatchFramesMeasured)));
        maxDispatchUs_ = std::max(maxDispatchUs_.load(), frameDispatchUs);
        if (portWritesThisFrame > 0) {
            ++dispatchFramesWithWrites_;
        }

        // Runtime log lines are a UI view over the authoritative export stream.
        // They intentionally report only state transitions after the startup
        // settle window so the operator sees actionable changes instead of the
        // constant redraw churn generated by DCS while a cockpit initializes.
        if (logStateChanges_ && !controlDb_.empty()) {
            bool loggingArmed = std::chrono::steady_clock::now() >= logStateChangesArmedAt_;
            bool seededBaselineThisFrame = false;
            if (loggingArmed && !logStateChangesArmedNoticeSent_) {
                // Seed every eligible control from the fully-settled state map before
                // emitting runtime changes. Without this, the first activity on a shared
                // address word can dump every sibling control on that word because those
                // identifiers have no baseline yet.
                lastLoggedValues_.clear();
                controlDb_.forEachControl([&](const ControlDescriptor& desc) {
                    if (!ShouldLogStateChange(desc,
                                              config_.logRawKnobsDials,
                                              config_.logRawGauges)) return;
                    lastLoggedValues_[desc.identifier] = ReadControlValue(desc, stateMap_);
                });
                logStateChangesPrimed_ = true;
                seededBaselineThisFrame = true;
                PostLog(hwnd_, L"State-change logging armed (startup settle complete).");
                logStateChangesArmedNoticeSent_ = true;
            }

            if (!dirty.empty()) {
                std::unordered_set<std::string> seenIds;
                size_t changedCount = 0;
                size_t emittedCount = 0;
                for (uint16_t addr : dirty) {
                    const auto* list = controlDb_.lookupByAddr(addr);
                    if (list) {
                        for (const auto* desc : *list) {
                            if (!seenIds.insert(desc->identifier).second) continue;
                            if (!ShouldLogStateChange(*desc,
                                                      config_.logRawKnobsDials,
                                                      config_.logRawGauges)) continue;

                            uint32_t value = ReadControlValue(*desc, stateMap_);
                            auto [it, inserted] = lastLoggedValues_.emplace(desc->identifier, value);

                            if (!logStateChangesPrimed_) {
                                it->second = value;
                                continue;
                            }

                            // Suppress startup churn and refresh the cached baseline until
                            // logging is armed. Also skip the exact frame where we seeded the
                            // settled baseline so the arm transition itself emits no noise.
                            if (!loggingArmed || seededBaselineThisFrame) {
                                it->second = value;
                                continue;
                            }

                            if (inserted) {
                                it->second = value;
                                continue;
                            }

                            if (it->second == value) continue;

                            it->second = value;
                            ++changedCount;
                            if (emittedCount < 20) {
                                PostLog(hwnd_, FormatWireStateChange(*desc, stateMap_));
                                ++emittedCount;
                            }
                        }
                    }
                }

                if (!logStateChangesPrimed_) {
                    logStateChangesPrimed_ = true;
                } else if (changedCount > emittedCount) {
                    std::wstringstream m;
                    m << L"  ... and " << (changedCount - emittedCount) << L" more changes.";
                    PostLog(hwnd_, m.str());
                }
            }
        }

        // Progress log every 300 frames (~10 s at 30 fps)
        if (fc == 1 || fc % 300 == 0) {
            std::wstringstream m;
            m << (config_.dryRun ? L"Dry-run: " : L"Bridge: ")
              << fc << L" frames " << (config_.dryRun ? L"processed." : L"dispatched.");
            PostLog(hwnd_, m.str());
        }
    }

    // ── Main receive loop ────────────────────────────────────────────────────
    void RunLoop() {
        std::vector<char> buf(kBufferSize);
        int backoffMs = 1000;

        while (running_) {
            if (socket_ == INVALID_SOCKET) {
                bool ok = config_.useUdp ? InitializeUdpSocket() : InitializeTcpSocket();
                if (!ok) {
                    std::wstringstream m;
                    m << L"Connection init failed. Retrying in " << (backoffMs / 1000) << L"s.";
                    PostLog(hwnd_, m.str());
                    Sleep(backoffMs);
                    backoffMs = std::min(backoffMs * 2, 5000);
                    continue;
                }
                backoffMs = 1000;
                PostLog(hwnd_, L"Socket ready, waiting for data...");
            }

            int received = recv(socket_, buf.data(), static_cast<int>(buf.size()), 0);
            if (!running_) break;

            if (received == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err == WSAETIMEDOUT) {
                        // Timeout is expected, just continue waiting
                        continue;
                    }
                    std::wstringstream m;
                    m << L"Socket error " << err << L". Reconnecting.";
                PostLog(hwnd_, m.str());
                closesocket(socket_); socket_ = INVALID_SOCKET;
                continue;
            }
            
            if (received == 0) {
                PostLog(hwnd_, L"Socket closed. Reconnecting...");
                closesocket(socket_); socket_ = INVALID_SOCKET;
                continue;
            }
            // Feed raw bytes into the protocol parser.
            exportParser_.processBytes(
                reinterpret_cast<const uint8_t*>(buf.data()),
                static_cast<size_t>(received));
            if (config_.useUdp) {
                exportParser_.flushFrame();
            }
        }

        if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
        running_ = false;
        CleanupResources();
        PostMessage(hwnd_, kBridgeStoppedMessage, 0, 0);
    }

    // ── Members ──────────────────────────────────────────────────────────────
    HWND  hwnd_                 = nullptr;
    BridgeConfig                config_;
    BiosStateMap                stateMap_;
    ExportParser                exportParser_;
    ControlDatabase             controlDb_;
    std::vector<std::unique_ptr<SerialPort>> ports_;
    std::thread                 worker_;
    std::atomic<bool>           running_{false};
    SOCKET                      socket_          = INVALID_SOCKET;
    SOCKET                      importSocket_    = INVALID_SOCKET;
    sockaddr_in                 importAddr_      = {};
    std::atomic<bool>           winsockInitialized_{false};
    std::mutex                  resourceMutex_;
    std::atomic<uint64_t>       frameCounter_{0};
    std::atomic<uint64_t>       dispatchFramesMeasured_{0};
    std::atomic<uint64_t>       dispatchFramesWithWrites_{0};
    std::atomic<uint64_t>       portWritesMeasured_{0};
    std::atomic<uint64_t>       avgDispatchUs_{0};
    std::atomic<uint64_t>       maxDispatchUs_{0};
    std::atomic<uint64_t>       avgPortWriteUs_{0};
    std::atomic<uint64_t>       maxPortWriteUs_{0};
    bool                        logStateChanges_ = false;
    bool                        logStateChangesPrimed_ = false;
    std::chrono::steady_clock::time_point logStateChangesArmedAt_ = std::chrono::steady_clock::now();
    bool                        logStateChangesArmedNoticeSent_ = false;
    std::unordered_map<std::string, uint32_t> lastLoggedValues_;
};

// ─── UI state ─────────────────────────────────────────────────────────────────

struct UiState {
    HWND modeLabel         = nullptr;
    HWND portsLabel        = nullptr;
    HWND hintLabel         = nullptr;
    HWND modeCombo         = nullptr;
    HWND portsEdit         = nullptr;
    HWND autoDetectButton  = nullptr;
    HWND toggleButton      = nullptr;
    HWND statusText        = nullptr;
    HWND dryRunCheckbox    = nullptr;
    HWND logChangesCheckbox= nullptr;
    HWND rawKnobsCheckbox  = nullptr;
    HWND rawGaugesCheckbox = nullptr;
    HWND exportLogButton   = nullptr;
    HWND selfTestButton    = nullptr;
    HWND logText           = nullptr;
    HFONT uiFont           = nullptr;
    HBRUSH bgBrush         = nullptr;
    HBRUSH panelBrush      = nullptr;
    HBRUSH inputBrush      = nullptr;
    HBRUSH buttonBrush     = nullptr;
    bool activeDryRun      = false;
    std::wstring statusPrefix = L"Stopped";
    std::wstring logBuffer;
    std::wstring pendingUiLog;
    bool logFlushScheduled = false;
    size_t pendingUiLineCount = 0;
    uint64_t droppedUiLines = 0;
    uint64_t linesReceivedWindow = 0;
    uint64_t linesRenderedWindow = 0;
    double linesReceivedPerSec = 0.0;
    double linesRenderedPerSec = 0.0;
    double avgFlushMs = 0.0;
    double maxFlushMs = 0.0;
    uint64_t flushCount = 0;
    bool queueOverloadNoticeShown = false;
    std::chrono::steady_clock::time_point metricsWindowStartedAt = std::chrono::steady_clock::now();
    std::unique_ptr<BridgeController> controller;
    StartupOptions startupOptions;
};

// ─── Layout ───────────────────────────────────────────────────────────────────

void LayoutControls(UiState* state, int W, int H) {
    if (!state) return;
    const int margin = 16, gap = 10, rowH = 28;
    const int labelW = 62, modeW = 320, buttonW = 110, startW = 110;

    int y = margin;
    MoveWindow(state->modeLabel, margin, y + 4, labelW, 20, TRUE);
    MoveWindow(state->modeCombo, margin + labelW + 4, y, modeW, rowH + 220, TRUE);

    y += rowH + gap;
    int portsX = margin + labelW + 4;
    int rightW  = (buttonW + gap) + startW;
    int portsW  = std::max(120, W - portsX - margin - rightW - gap);
    MoveWindow(state->portsLabel,       margin,                        y + 4, labelW + 10, 20,   TRUE);
    MoveWindow(state->portsEdit,        portsX,                        y,     portsW,      rowH,  TRUE);
    MoveWindow(state->autoDetectButton, portsX + portsW + gap,         y,     buttonW,     rowH,  TRUE);
    MoveWindow(state->toggleButton,     portsX + portsW + gap*2 + buttonW, y, startW,      rowH,  TRUE);

    y += rowH + gap;
    MoveWindow(state->dryRunCheckbox,     margin,                      y + 2, 200, 22,    TRUE);
    MoveWindow(state->logChangesCheckbox, margin + 210,                y + 2, 200, 22,    TRUE);
    MoveWindow(state->exportLogButton,    std::max(margin, W - margin - buttonW - gap - buttonW), y - 2, buttonW, rowH, TRUE);
    MoveWindow(state->selfTestButton,     std::max(margin, W - margin - buttonW),                 y - 2, buttonW, rowH, TRUE);

    y += rowH;
    MoveWindow(state->rawKnobsCheckbox,  margin,       y + 2, 220, 22, TRUE);
    MoveWindow(state->rawGaugesCheckbox, margin + 230, y + 2, 220, 22, TRUE);

    y += rowH + gap + 16;
    int hintW = 220;
    int statusW = std::max(320, W - margin * 2 - hintW - gap);
    MoveWindow(state->statusText, margin, y, statusW, 26, TRUE);
    MoveWindow(state->hintLabel,  margin + statusW + gap, y, hintW, 26, TRUE);

    y += 28 + gap + 12;
    int logH = std::max(120, H - y - margin);
    MoveWindow(state->logText, margin, y, std::max(200, W - margin * 2), logH, TRUE);
}

// ─── Wnd helpers ──────────────────────────────────────────────────────────────

UiState* GetUiState(HWND hwnd) {
    return reinterpret_cast<UiState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

size_t CountLogicalLines(const std::wstring& text) {
    if (text.empty()) return 0;

    size_t lines = 0;
    for (wchar_t ch : text) {
        if (ch == L'\n') ++lines;
    }
    return lines == 0 ? 1 : lines;
}

void UpdateStatusText(UiState* s) {
    if (!s || !s->statusText) return;

    if (!s->controller || !s->controller->IsRunning()) {
        SetWindowTextW(s->statusText, s->statusPrefix.c_str());
        return;
    }

    std::wstringstream status;
    status << s->statusPrefix
           << L" | q:" << s->pendingUiLineCount
           << L" drop:" << s->droppedUiLines
           << L" rx:" << std::fixed << std::setprecision(0) << s->linesReceivedPerSec << L"/s"
           << L" ui:" << std::fixed << std::setprecision(0) << s->linesRenderedPerSec << L"/s"
           << L" flush:" << std::fixed << std::setprecision(1) << s->avgFlushMs << L"/" << s->maxFlushMs << L" ms";

    if (s->controller) {
        auto dispatch = s->controller->GetDispatchMetrics();
        status << L" tx:" << std::fixed << std::setprecision(2)
               << (static_cast<double>(dispatch.avgDispatchUs) / 1000.0)
               << L"/" << (static_cast<double>(dispatch.maxDispatchUs) / 1000.0) << L" ms"
               << L" port:" << (static_cast<double>(dispatch.avgPortWriteUs) / 1000.0)
               << L"/" << (static_cast<double>(dispatch.maxPortWriteUs) / 1000.0) << L" ms";
    }

    if (s->pendingUiLineCount >= (kMaxPendingUiLines / 2) || s->droppedUiLines > 0) {
        status << L" [UI lag]";
    }

    SetWindowTextW(s->statusText, status.str().c_str());
}

void SetStatus(UiState* s, const std::wstring& t) {
    if (!s) return;
    s->statusPrefix = t;
    UpdateStatusText(s);
}

void ResetUiMetrics(UiState* s) {
    if (!s) return;

    s->pendingUiLineCount = 0;
    s->droppedUiLines = 0;
    s->linesReceivedWindow = 0;
    s->linesRenderedWindow = 0;
    s->linesReceivedPerSec = 0.0;
    s->linesRenderedPerSec = 0.0;
    s->avgFlushMs = 0.0;
    s->maxFlushMs = 0.0;
    s->flushCount = 0;
    s->queueOverloadNoticeShown = false;
    s->metricsWindowStartedAt = std::chrono::steady_clock::now();
}

void UpdateUiRates(UiState* s) {
    if (!s) return;

    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - s->metricsWindowStartedAt;
    if (elapsed.count() < 1.0) return;

    s->linesReceivedPerSec = static_cast<double>(s->linesReceivedWindow) / elapsed.count();
    s->linesRenderedPerSec = static_cast<double>(s->linesRenderedWindow) / elapsed.count();
    s->linesReceivedWindow = 0;
    s->linesRenderedWindow = 0;
    s->metricsWindowStartedAt = now;
}

void RecordUiFlush(UiState* s, size_t renderedLines, double flushMs) {
    if (!s) return;

    s->linesRenderedWindow += renderedLines;
    ++s->flushCount;
    double sampleCount = static_cast<double>(s->flushCount);
    s->avgFlushMs += (flushMs - s->avgFlushMs) / sampleCount;
    s->maxFlushMs = std::max(s->maxFlushMs, flushMs);
}

void ApplyFont(UiState* s, HWND c) {
    if (s && c && s->uiFont) SendMessage(c, WM_SETFONT, reinterpret_cast<WPARAM>(s->uiFont), TRUE);
}

void ScrollLogControlToBottom(UiState* s) {
    if (!s || !s->logText) return;

    int end = GetWindowTextLengthW(s->logText);
    SendMessage(s->logText, EM_SETSEL, end, end);
    SendMessage(s->logText, EM_SCROLLCARET, 0, 0);
    SendMessage(s->logText, WM_VSCROLL, SB_BOTTOM, 0);
}

void TrimLogControlToMaxLines(UiState* s) {
    if (!s || !s->logText) return;

    int n = GetWindowTextLengthW(s->logText);
    if (n <= 0) return;

    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(s->logText, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));

    size_t lineCount = CountLogicalLines(text);
    if (lineCount <= kMaxUiDisplayLines) return;

    size_t linesToDrop = lineCount - kMaxUiDisplayLines;
    size_t cutPos = 0;
    while (cutPos < text.size() && linesToDrop > 0) {
        if (text[cutPos] == L'\n') {
            --linesToDrop;
        }
        ++cutPos;
    }

    if (cutPos > 0) {
        SendMessage(s->logText, EM_SETSEL, 0, static_cast<LPARAM>(cutPos));
        SendMessage(s->logText, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        ScrollLogControlToBottom(s);
    }
}

void AppendToLogControl(UiState* s, const std::wstring& text) {
    if (!s || !s->logText) return;

    // Update the edit control as one visual transaction. We do briefly select
    // the oldest text while trimming the ring buffer, but suppress redraw until
    // the control is re-anchored at the newest line so the user never sees the
    // thumb or caret jump to the top mid-update.
    SendMessage(s->logText, WM_SETREDRAW, FALSE, 0);
    int n = GetWindowTextLengthW(s->logText);
    SendMessage(s->logText, EM_SETSEL, n, n);
    SendMessage(s->logText, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    TrimLogControlToMaxLines(s);
    ScrollLogControlToBottom(s);
    SendMessage(s->logText, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s->logText, nullptr, TRUE);
    UpdateWindow(s->logText);
}

void AppendToLog(UiState* s, const std::wstring& text) {
    s->logBuffer += text;
    AppendToLogControl(s, text);
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& content) {
    int n = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    std::string utf8(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return out.good();
}

void ExportLogToFile(HWND hwnd) {
    UiState* s = GetUiState(hwnd);
    if (!s) return;
    if (s->logBuffer.empty()) { AppendToLog(s, L"No log content to export.\r\n"); return; }

    wchar_t fp[MAX_PATH] = L"dcsbios-serial-bridge-log.txt";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = fp;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"txt";
    if (!GetSaveFileNameW(&ofn)) return;

    if (WriteUtf8File(fp, s->logBuffer)) {
        std::wstringstream m; m << L"Log exported to: " << fp;
        AppendToLog(s, m.str() + L"\r\n");
    } else {
        AppendToLog(s, L"Failed to export log.\r\n");
    }
}

std::wstring GetWindowTextContent(HWND hwnd) {
    int n = GetWindowTextLengthW(hwnd);
    std::wstring t(static_cast<size_t>(n) + 1, L'\0');
    if (n > 0) GetWindowTextW(hwnd, t.data(), n + 1);
    t.resize(static_cast<size_t>(n));
    return t;
}

// ─── Bridge toggle ───────────────────────────────────────────────────────────

void ToggleBridge(HWND hwnd) {
    UiState* s = GetUiState(hwnd);
    if (!s || !s->controller) return;

    if (s->controller->IsRunning()) {
        s->controller->Stop();
        SetWindowTextW(s->toggleButton, L"Start");
        SetStatus(s, L"Stopped");
        EnableWindow(s->modeCombo, TRUE);
        EnableWindow(s->portsEdit, TRUE);
        EnableWindow(s->autoDetectButton, TRUE);
        return;
    }

    int modeIndex = static_cast<int>(SendMessage(s->modeCombo, CB_GETCURSEL, 0, 0));
    auto parsedPorts = ParsePorts(GetWindowTextContent(s->portsEdit));
    if (parsedPorts.empty()) {
        parsedPorts = DetectAvailableComPorts();
        if (!parsedPorts.empty())
            SetWindowTextW(s->portsEdit, JoinPorts(parsedPorts).c_str());
    }

    BridgeConfig cfg;
    cfg.useUdp          = (modeIndex != 1);
    cfg.dryRun          = (SendMessage(s->dryRunCheckbox,     BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.logStateChanges = (SendMessage(s->logChangesCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.logRawKnobsDials = (SendMessage(s->rawKnobsCheckbox,  BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.logRawGauges     = (SendMessage(s->rawGaugesCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.comPorts        = std::move(parsedPorts);
    s->activeDryRun     = cfg.dryRun;

    if (!s->controller->Start(cfg)) { SetStatus(s, L"Start failed"); return; }

    ResetUiMetrics(s);
    SetWindowTextW(s->toggleButton, L"Stop");
    std::wstring status = cfg.useUdp ? L"Running (UDP" : L"Running (TCP";
    if (cfg.dryRun) status += L", Dry-Run";
    status += L")";
    SetStatus(s, status);
    EnableWindow(s->modeCombo, FALSE);
    EnableWindow(s->portsEdit, FALSE);
    EnableWindow(s->autoDetectButton, FALSE);
}

// ─── Window proc ─────────────────────────────────────────────────────────────

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {

    case WM_CREATE: {
        auto* s = new UiState();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->controller  = std::make_unique<BridgeController>(hwnd);
        s->bgBrush     = CreateSolidBrush(kColorBg);
        s->panelBrush  = CreateSolidBrush(kColorPanel);
        s->inputBrush  = CreateSolidBrush(kColorInputBg);
        s->buttonBrush = CreateSolidBrush(kColorButtonBg);
        s->uiFont      = CreateFontW(-18, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        auto cw = [&](LPCWSTR cls, LPCWSTR txt, DWORD style, UINT_PTR id = 0) {
            return CreateWindowW(cls, txt, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 100, 28, hwnd, reinterpret_cast<HMENU>(id), nullptr, nullptr);
        };

        s->modeLabel        = cw(L"STATIC",   L"Mode:",                    0);
        s->modeCombo        = cw(L"COMBOBOX", L"",          CBS_DROPDOWNLIST,  kControlMode);
        SendMessage(s->modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"UDP (Multicast 239.255.50.10:5010)"));
        SendMessage(s->modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"TCP (127.0.0.1:7778)"));
        SendMessage(s->modeCombo, CB_SETCURSEL, 0, 0);

        s->portsLabel       = cw(L"STATIC",   L"COM Ports:",               0);
        s->portsEdit        = cw(L"EDIT",      L"",    WS_BORDER | ES_AUTOHSCROLL, kControlPorts);
        s->autoDetectButton = cw(L"BUTTON",    L"Auto Detect",  BS_PUSHBUTTON, kControlAutoDetect);
        s->toggleButton     = cw(L"BUTTON",    L"Start",        BS_PUSHBUTTON, kControlToggle);
        s->dryRunCheckbox   = cw(L"BUTTON",    L"Dry Run (no serial write)", BS_AUTOCHECKBOX, kControlDryRun);
        s->logChangesCheckbox = cw(L"BUTTON",  L"Log State Changes",        BS_AUTOCHECKBOX, kControlLogChanges);
        s->rawKnobsCheckbox = cw(L"BUTTON",    L"Raw Knobs/Dials",          BS_AUTOCHECKBOX, kControlRawKnobs);
        s->rawGaugesCheckbox = cw(L"BUTTON",   L"Raw Gauges",               BS_AUTOCHECKBOX, kControlRawGauges);
        s->exportLogButton  = cw(L"BUTTON",    L"Export Log",   BS_PUSHBUTTON, kControlExportLog);
        s->selfTestButton   = cw(L"BUTTON",    L"Self Test",    BS_PUSHBUTTON, kControlSelfTest);
        s->statusText       = cw(L"STATIC",    L"Stopped",                  0, kControlStatus);
        s->hintLabel        = cw(L"STATIC",    L"F8 toggles Start/Stop",    0);
        s->logText          = cw(L"EDIT",       L"",
                                  WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                  kControlLog);

        // Apply font to all controls
        for (HWND c : { s->modeLabel, s->portsLabel, s->modeCombo, s->portsEdit,
                         s->autoDetectButton, s->toggleButton, s->dryRunCheckbox,
                         s->logChangesCheckbox, s->rawKnobsCheckbox, s->rawGaugesCheckbox,
                         s->exportLogButton, s->selfTestButton,
                         s->statusText, s->hintLabel, s->logText }) {
            ApplyFont(s, c);
        }

        SendMessage(s->rawKnobsCheckbox, BM_SETCHECK, BST_CHECKED, 0);
        SendMessage(s->rawGaugesCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);

        auto detected = DetectAvailableComPorts();
        if (!detected.empty()) SetWindowTextW(s->portsEdit, JoinPorts(detected).c_str());

        RECT rc = {};
        GetClientRect(hwnd, &rc);
        LayoutControls(s, rc.right, rc.bottom);

        RegisterHotKey(hwnd, kHotkeyId, MOD_NOREPEAT, VK_F8);
        SetTimer(hwnd, kStatusRefreshTimer, kStatusRefreshIntervalMs, nullptr);
        return 0;
    }

    case WM_SIZE: {
        UiState* s = GetUiState(hwnd);
        if (s && wParam != SIZE_MINIMIZED) LayoutControls(s, LOWORD(lParam), HIWORD(lParam));
        return 0;
    }

    case WM_ERASEBKGND: {
        UiState* s = GetUiState(hwnd);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc,
                 s && s->bgBrush ? s->bgBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        UiState* s = GetUiState(hwnd);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, kColorText);
        SetBkColor(hdc, kColorBg);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(s && s->bgBrush ? s->bgBrush : GetStockObject(NULL_BRUSH));
    }

    case WM_CTLCOLOREDIT: {
        UiState* s = GetUiState(hwnd);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, kColorText);
        SetBkColor(hdc, kColorInputBg);
        return reinterpret_cast<INT_PTR>(s && s->inputBrush ? s->inputBrush : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLORBTN: {
        UiState* s   = GetUiState(hwnd);
        HWND ctrl    = reinterpret_cast<HWND>(lParam);
        HDC  hdc     = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, IsWindowEnabled(ctrl) ? kColorText : kColorTextMuted);
        SetBkColor(hdc, kColorButtonBg);
        return reinterpret_cast<INT_PTR>(s && s->buttonBrush ? s->buttonBrush : GetStockObject(GRAY_BRUSH));
    }

    case WM_HOTKEY:
        if (wParam == kHotkeyId) ToggleBridge(hwnd);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        UiState* s = GetUiState(hwnd);
        if (!s) return 0;
        switch (id) {
        case kControlAutoDetect: {
            auto detected = DetectAvailableComPorts();
            SetWindowTextW(s->portsEdit, JoinPorts(detected).c_str());
            AppendToLog(s, detected.empty() ? L"No COM ports detected.\r\n"
                                             : L"Detected: " + JoinPorts(detected) + L"\r\n");
            break;
        }
        case kControlToggle:
            ToggleBridge(hwnd);
            break;
        case kControlDryRun:
            if (s->controller && s->controller->IsRunning()) {
                SendMessage(s->dryRunCheckbox, BM_SETCHECK,
                            s->activeDryRun ? BST_CHECKED : BST_UNCHECKED, 0);
                AppendToLog(s, L"Dry Run can only be changed while stopped.\r\n");
            }
            break;
        case kControlLogChanges:
            // Allow toggling live — change takes effect next Start()
            break;
        case kControlRawKnobs:
        case kControlRawGauges:
            // Allow toggling live — change takes effect next Start()
            break;
        case kControlExportLog:
            ExportLogToFile(hwnd);
            break;
        case kControlSelfTest:
            if (s->controller) s->controller->RunSelfTest();
            break;
        }
        return 0;
    }

    case WM_TIMER: {
        UiState* s = GetUiState(hwnd);
        if (!s) return 0;
        if (wParam == kLogFlushTimer) {
            KillTimer(hwnd, kLogFlushTimer);
            s->logFlushScheduled = false;
            if (!s->pendingUiLog.empty()) {
                size_t renderedLines = s->pendingUiLineCount;
                auto flushStart = std::chrono::steady_clock::now();
                AppendToLogControl(s, s->pendingUiLog);
                auto flushEnd = std::chrono::steady_clock::now();
                double flushMs = std::chrono::duration<double, std::milli>(flushEnd - flushStart).count();
                RecordUiFlush(s, renderedLines, flushMs);
                s->pendingUiLog.clear();
                s->pendingUiLineCount = 0;
            }
            UpdateUiRates(s);
            UpdateStatusText(s);
        } else if (wParam == kStatusRefreshTimer) {
            UpdateUiRates(s);
            UpdateStatusText(s);
        }
        return 0;
    }

    case kLogMessage: {
        UiState* s = GetUiState(hwnd);
        if (s) {
            auto* payload = reinterpret_cast<std::wstring*>(lParam);
            if (payload) {
                size_t incomingLines = CountLogicalLines(*payload);
                s->logBuffer += *payload;
                s->linesReceivedWindow += incomingLines;

                if (s->pendingUiLineCount + incomingLines > kMaxPendingUiLines) {
                    s->droppedUiLines += incomingLines;

                    // Preserve the full in-memory capture while dropping only the
                    // queued UI work. This keeps overload visible without implying
                    // the serial/export pipeline itself lost data.
                    if (!s->queueOverloadNoticeShown) {
                        AppendToLogControl(s, L"[ui] Pending render queue exceeded limit; dropping queued UI lines until it recovers.\r\n");
                        s->queueOverloadNoticeShown = true;
                    }
                } else {
                    s->pendingUiLog += *payload;
                    s->pendingUiLineCount += incomingLines;
                    if (!s->logFlushScheduled) {
                        SetTimer(hwnd, kLogFlushTimer, kLogFlushIntervalMs, nullptr);
                        s->logFlushScheduled = true;
                    }
                }

                if (s->pendingUiLineCount == 0) {
                    s->queueOverloadNoticeShown = false;
                }

                UpdateUiRates(s);
                UpdateStatusText(s);
                delete payload;
            }
        }
        return 0;
    }

    case kBridgeStoppedMessage: {
        UiState* s = GetUiState(hwnd);
        if (s) {
            SetWindowTextW(s->toggleButton, L"Start");
            SetStatus(s, L"Stopped");
            EnableWindow(s->modeCombo, TRUE);
            EnableWindow(s->portsEdit, TRUE);
            EnableWindow(s->autoDetectButton, TRUE);
        }
        return 0;
    }

    case WM_DESTROY: {
        UiState* s = GetUiState(hwnd);
        if (s) {
            if (s->logFlushScheduled) KillTimer(hwnd, kLogFlushTimer);
            KillTimer(hwnd, kStatusRefreshTimer);
            if (!s->pendingUiLog.empty()) {
                AppendToLogControl(s, s->pendingUiLog);
                s->pendingUiLog.clear();
                s->pendingUiLineCount = 0;
            }
            if (s->controller) s->controller->Stop();
            UnregisterHotKey(hwnd, kHotkeyId);
            if (s->uiFont)      DeleteObject(s->uiFont);
            if (s->bgBrush)     DeleteObject(s->bgBrush);
            if (s->panelBrush)  DeleteObject(s->panelBrush);
            if (s->inputBrush)  DeleteObject(s->inputBrush);
            if (s->buttonBrush) DeleteObject(s->buttonBrush);
            delete s;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    }

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

} // namespace

// ─── Entry point ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    StartupOptions opts = ParseStartupOptions();

    const wchar_t kClass[] = L"DcsBiosSerialBridgeWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_COMPOSITED, kClass, L"DCS-BIOS Serial Bridge",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 580,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);

    UiState* s = GetUiState(hwnd);
    if (s) {
        s->startupOptions = std::move(opts);
        if (s->startupOptions.forceUdp.has_value())
            SendMessage(s->modeCombo, CB_SETCURSEL,
                        s->startupOptions.forceUdp.value() ? 0 : 1, 0);
        if (s->startupOptions.ports.has_value())
            SetWindowTextW(s->portsEdit, JoinPorts(s->startupOptions.ports.value()).c_str());
        if (s->startupOptions.dryRun)
            SendMessage(s->dryRunCheckbox, BM_SETCHECK, BST_CHECKED, 0);
        if (s->startupOptions.autoStart)
            ToggleBridge(hwnd);
    }

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}
