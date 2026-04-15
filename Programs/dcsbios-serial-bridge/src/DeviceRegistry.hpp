#pragma once
// DeviceRegistry
//   Manages per-device metadata for every COM port connected to the bridge.
//
// Handshake overview
// ──────────────────
// When a device opens the COM port, the bridge immediately sends the 4-byte
// probe frame and waits up to 300 ms for a response.
//
//   Bridge → Device:  0xAA 0xDE 0xAD 0x01   (DCSBIOS_PROBE v1)
//
// A DCSBIOS-aware device (Open-Hornet or compatible) replies within 300 ms:
//
//   Device → Bridge:  0xAA 0xDE 0xAD 0x02   [flags:1] [name_len:1] [name:N]
//                                             [sub_count:2] [addr:2 mask:2 shift:2] ...
//
//   flags bits:
//       0x01 = device is an RS-485 bus MASTER (has downstream slaves)
//       0x02 = device is an RS-485 bus SLAVE  (managed by another master)
//       0x04 = device requests bidirectional (can send import commands)
//
//   sub_count subscriptions follow (address/mask/shift) telling the bridge
//   which outputs to forward.  0xFFFF 0xFFFF 0xFFFF means "send everything".
//
// If no valid response is received the port is treated as a legacy dumb
// consumer: all bytes are forwarded (original behaviour).

#include "BiosProtocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace dcsbios {

// ─────────────────────────────────────────────────────────────────────────────
// Probe frame constants
// ─────────────────────────────────────────────────────────────────────────────
constexpr uint8_t kProbeHdr0 = 0xAA;
constexpr uint8_t kProbeHdr1 = 0xDE;
constexpr uint8_t kProbeHdr2 = 0xAD;
constexpr uint8_t kProbePing = 0x01;
constexpr uint8_t kProbePong = 0x02;
constexpr uint8_t kProbeAck  = 0x03;  // bridge → device: ack + negotiation complete

constexpr auto kHandshakeTimeout = std::chrono::milliseconds(300);

// ─────────────────────────────────────────────────────────────────────────────
// Subscription
//   One output field a device wants to receive.
//   addr=0xFFFF/mask=0xFFFF means "all".
// ─────────────────────────────────────────────────────────────────────────────
struct Subscription {
    uint16_t byteAddr = 0xFFFF;
    uint16_t mask     = 0xFFFF;
    uint8_t  shift    = 0;
    bool wantsAll() const { return byteAddr == 0xFFFF && mask == 0xFFFF; }
};

// ─────────────────────────────────────────────────────────────────────────────
// DeviceRole
// ─────────────────────────────────────────────────────────────────────────────
enum class DeviceRole {
    Unknown,      // not yet identified / legacy
    Legacy,       // no handshake response → raw pass-through
    Standalone,   // declared standalone panel
    RS485Master,  // has slaves downstream, aggregates their commands
    RS485Slave    // managed by a master, bridge talks to master instead
};

// ─────────────────────────────────────────────────────────────────────────────
// DeviceInfo
// ─────────────────────────────────────────────────────────────────────────────
struct DeviceInfo {
    std::string  comPort;        // e.g. "COM10"
    std::string  deviceName;     // from handshake, empty for legacy
    DeviceRole   role        = DeviceRole::Unknown;
    bool         bidir       = false;    // can send import commands
    bool         handshakeDone = false;

    std::vector<Subscription> subscriptions;

    // Set of subscribed word addresses for fast O(1) lookup (populated from subscriptions).
    std::unordered_set<uint16_t> subAddrs;
    bool wantsAll = true;  // true until handshake completes and says otherwise

    // Populate subAddrs from subscriptions after handshake.
    void buildAddrSet() {
        wantsAll = false;
        subAddrs.clear();
        for (auto& s : subscriptions) {
            if (s.wantsAll()) { wantsAll = true; subAddrs.clear(); return; }
            subAddrs.insert(s.byteAddr);
        }
    }

    bool wantsAddress(uint16_t byteAddr) const {
        if (wantsAll) return true;
        return subAddrs.count(byteAddr) > 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// HandshakeParser
//   Feed bytes from the device into processByte().
//   Call isComplete()/getDevice() when done.
// ─────────────────────────────────────────────────────────────────────────────
class HandshakeParser {
public:
    enum class Result { Pending, Complete, Failed };

    HandshakeParser() = default;

    Result processByte(uint8_t b) {
        if (done_) return Result::Complete;
        switch (step_) {
        case 0: step_ = (b == kProbeHdr0) ? 1 : 0; break;
        case 1: step_ = (b == kProbeHdr1) ? 2 : 0; break;
        case 2: step_ = (b == kProbeHdr2) ? 3 : 0; break;
        case 3:
            if (b != kProbePong) { return Result::Failed; }
            step_ = 4; break;
        case 4: flags_ = b; step_ = 5; break;
        case 5: nameLen_ = b; step_ = 6; nameRead_ = 0; break;
        case 6:
            name_ += static_cast<char>(b);
            if (++nameRead_ == nameLen_) step_ = 7;
            break;
        case 7: subCount_ = b;         step_ = 8; break;
        case 8: subCount_ |= b << 8;   step_ = (subCount_ == 0) ? 99 : 9; break;
        case 9: curSub_.byteAddr = b;  step_ = 10; break;
        case 10: curSub_.byteAddr |= static_cast<uint16_t>(b) << 8; step_ = 11; break;
        case 11: curSub_.mask = b;     step_ = 12; break;
        case 12: curSub_.mask |= static_cast<uint16_t>(b) << 8; curSub_.shift = 0; step_ = 13; break;
        case 13:
            curSub_.shift = b;
            subs_.push_back(curSub_);
            if ((int)subs_.size() == subCount_) step_ = 99;
            else step_ = 9;
            break;
        case 99:
            done_ = true;
            return Result::Complete;
        }
        return Result::Pending;
    }

    bool complete() const { return done_; }

    // Build a DeviceInfo from the parsed fields.
    void populateDevice(DeviceInfo& d) const {
        d.deviceName  = name_;
        d.bidir       = (flags_ & 0x04) != 0;
        d.subscriptions = subs_;
        if (flags_ & 0x01) d.role = DeviceRole::RS485Master;
        else if (flags_ & 0x02) d.role = DeviceRole::RS485Slave;
        else d.role = DeviceRole::Standalone;
        d.handshakeDone = true;
        d.buildAddrSet();
    }

    // Build the 4-byte ping frame to send to the device.
    static std::array<uint8_t, 4> pingFrame() {
        return { kProbeHdr0, kProbeHdr1, kProbeHdr2, kProbePing };
    }

    // Build the 1-byte ACK frame to send after successful handshake.
    static std::array<uint8_t, 4> ackFrame() {
        return { kProbeHdr0, kProbeHdr1, kProbeHdr2, kProbeAck };
    }

private:
    bool     done_      = false;
    int      step_      = 0;
    uint8_t  flags_     = 0;
    uint8_t  nameLen_   = 0;
    std::string name_;
    uint8_t  nameRead_  = 0;
    int      subCount_  = 0;
    std::vector<Subscription> subs_;
    Subscription curSub_;
};

// ─────────────────────────────────────────────────────────────────────────────
// DeltaPacket
//   One "write record" in DCS-BIOS format to send downstream, but only for
//   a single word that changed.  We pack consecutive dirty addresses into
//   the minimum number of write records.
// ─────────────────────────────────────────────────────────────────────────────
inline std::vector<uint8_t> BuildDeltaFrame(
    const BiosStateMap&           state,
    const std::vector<uint16_t>&  dirtyAddrs,
    const DeviceInfo&             dev)
{
    // Collect addresses this device cares about, sorted.
    std::vector<uint16_t> relevant;
    relevant.reserve(dirtyAddrs.size());
    for (uint16_t a : dirtyAddrs) {
        if (dev.wantsAddress(a)) relevant.push_back(a);
    }
    if (relevant.empty()) return {};
    std::sort(relevant.begin(), relevant.end());

    // Build DCS-BIOS wire format:
    // [sync: 0x55 0x55 0x55 0x55]
    // [addr_lo addr_hi len_lo len_hi data…] per run of consecutive addresses
    std::vector<uint8_t> frame;
    frame.reserve(4 + relevant.size() * 6);
    frame.push_back(0x55); frame.push_back(0x55); frame.push_back(0x55); frame.push_back(0x55);

    size_t i = 0;
    while (i < relevant.size()) {
        // Find consecutive run
        size_t j = i;
        while (j + 1 < relevant.size() && relevant[j + 1] == relevant[j] + 2) ++j;
        uint16_t runStart = relevant[i];
        uint16_t runWords = static_cast<uint16_t>(j - i + 1);
        uint16_t lenBytes = runWords * 2;

        frame.push_back(runStart & 0xFF);
        frame.push_back((runStart >> 8) & 0xFF);
        frame.push_back(lenBytes & 0xFF);
        frame.push_back((lenBytes >> 8) & 0xFF);
        for (uint16_t w = 0; w < runWords; ++w) {
            uint16_t a = runStart + w * 2;
            frame.push_back(state.raw()[a]);
            frame.push_back(state.raw()[a + 1]);
        }
        i = j + 1;
    }
    return frame;
}

// ─────────────────────────────────────────────────────────────────────────────
// ImportLineParser
//   Collects bytes from a device serial port and assembles ImportCommand lines.
//   Fire onCommand when a complete \n-terminated line arrives.
// ─────────────────────────────────────────────────────────────────────────────
class ImportLineParser {
public:
    std::function<void(const ImportCommand&)> onCommand;

    void processBytes(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            char c = static_cast<char>(data[i]);
            if (c == '\n' || c == '\r') {
                if (!line_.empty()) parseLine();
                line_.clear();
            } else if (line_.size() < 256) {
                line_ += c;
            }
        }
    }

private:
    std::string line_;

    void parseLine() {
        // Format: IDENTIFIER ACTION [VALUE]
        auto sp1 = line_.find(' ');
        if (sp1 == std::string::npos) return;
        ImportCommand cmd;
        cmd.identifier = line_.substr(0, sp1);
        auto rest = line_.substr(sp1 + 1);
        auto sp2 = rest.find(' ');
        if (sp2 == std::string::npos) {
            cmd.action = rest;
        } else {
            cmd.action = rest.substr(0, sp2);
            cmd.value  = rest.substr(sp2 + 1);
        }
        if (!cmd.identifier.empty() && !cmd.action.empty() && onCommand) {
            onCommand(cmd);
        }
    }
};

} // namespace dcsbios
