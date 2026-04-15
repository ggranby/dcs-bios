#pragma once
// DCS-BIOS Export Protocol Parser
//
// Wire format (big picture):
//   sync frame  : 0x55 0x55 0x55 0x55   (marks start of one update tick)
//   write record: [addr_lo][addr_hi][len_lo][len_hi][data bytes …]
//
// All integers little-endian.  addr and len are always even.
// The sync sequence cannot appear in normal data (DCS splits it if needed).
//
// Import protocol (device → DCS): plain-text newline-terminated
//   IDENTIFIER SET_STATE value\n
//   IDENTIFIER TOGGLE\n  etc.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dcsbios {

// ─────────────────────────────────────────────────────────────────────────────
// BiosStateMap
//   Flat 64 KB mirror of the DCS-BIOS address space.
//   Tracks which 16-bit words changed since the last call to takeDirty().
// ─────────────────────────────────────────────────────────────────────────────
class BiosStateMap {
public:
    static constexpr size_t kStateBytes = 65536;

    BiosStateMap() {
        state_.fill(0);
        dirty_.fill(false);
    }

    // Apply a write record from the export stream.
    // addr is byte-aligned (always even per protocol).
    void write(uint16_t addr, const uint8_t* data, uint16_t len) {
        for (uint16_t i = 0; i < len; ++i) {
            if (state_[addr + i] != data[i]) {
                state_[addr + i] = data[i];
                // mark entire containing 16-bit word dirty
                dirty_[(addr + i) & 0xFFFEu] = true;
            }
        }
    }

    // Read a 16-bit word from the current state.
    uint16_t readWord(uint16_t byteAddr) const {
        uint16_t v = 0;
        v |= static_cast<uint16_t>(state_[byteAddr]);
        v |= static_cast<uint16_t>(state_[byteAddr + 1]) << 8;
        return v;
    }

    // Extract a field value using address/mask/shift (DCS-BIOS descriptor).
    uint16_t readField(uint16_t byteAddr, uint16_t mask, uint8_t shift) const {
        return (readWord(byteAddr) & mask) >> shift;
    }

    // Returns addresses (even, byte) of all dirty words and clears dirty flags.
    std::vector<uint16_t> takeDirty() {
        std::vector<uint16_t> result;
        for (size_t i = 0; i < kStateBytes; i += 2) {
            if (dirty_[i]) {
                result.push_back(static_cast<uint16_t>(i));
                dirty_[i] = false;
            }
        }
        return result;
    }

    const uint8_t* raw() const { return state_.data(); }

private:
    std::array<uint8_t, kStateBytes> state_;
    std::array<bool, kStateBytes>    dirty_;  // indexed by byte addr, only even indices used
};

// ─────────────────────────────────────────────────────────────────────────────
// ExportParser
//   Feed raw bytes from the UDP/TCP stream into processBytes().
//   DCS-BIOS frames begin with 0x55 0x55 0x55 0x55, followed by write records.
//   onFrameSync fires after all writes for one frame are applied.
// ─────────────────────────────────────────────────────────────────────────────
class ExportParser {
public:
    explicit ExportParser(BiosStateMap& map) : map_(map) {}

    // Callback fired once per completed frame (after all writes for that frame).
    std::function<void()> onFrameSync;

    // Process a chunk of raw bytes from the network stream.
    void processBytes(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            processByte(data[i]);
        }
    }

    // UDP export uses one datagram per frame. Flush the current frame after
    // the datagram has been fully consumed so changes are visible immediately.
    void flushFrame() {
        if (!inFrame_) return;
        if (state_ != State::AddrLo) return;
        if (onFrameSync) onFrameSync();
        inFrame_ = false;
        state_ = State::Sync0;
    }

private:
    enum class State { Sync0, Sync1, Sync2, Sync3, AddrLo, AddrHi, LenLo, LenHi, Data };

    BiosStateMap& map_;
    State         state_  = State::Sync0;
    uint16_t      addr_   = 0;
    uint16_t      len_    = 0;
    uint16_t      count_  = 0;
    uint8_t       buf_[4] = {};  // sync candidate buffer
    uint8_t       syncCnt_= 0;
    bool          inFrame_ = false;

    void processByte(uint8_t b) {
        switch (state_) {
        case State::Sync0:
            if (b == 0x55) { state_ = State::Sync1; } break;
        case State::Sync1:
            state_ = (b == 0x55) ? State::Sync2 : State::Sync0; break;
        case State::Sync2:
            state_ = (b == 0x55) ? State::Sync3 : State::Sync0; break;
        case State::Sync3:
            if (b == 0x55) {
                if (inFrame_ && onFrameSync) onFrameSync();
                inFrame_ = true;
                state_ = State::AddrLo;
            } else {
                state_ = State::Sync0;
            }
            break;
        case State::AddrLo:
            addr_ = b;
            state_ = State::AddrHi;
            break;
        case State::AddrHi:
            addr_ |= static_cast<uint16_t>(b) << 8;
            state_ = State::LenLo;
            break;
        case State::LenLo:
            len_ = b;
            state_ = State::LenHi;
            break;
        case State::LenHi:
            len_ |= static_cast<uint16_t>(b) << 8;
            count_ = 0;
            if (len_ == 0) { state_ = State::AddrLo; break; }
            state_ = State::Data;
            // Inline short records to avoid heap allocation
            dataBuf_.resize(len_);
            break;
        case State::Data:
            if (count_ < dataBuf_.size()) {
                dataBuf_[count_++] = b;
            }
            if (count_ == len_) {
                map_.write(addr_, dataBuf_.data(), len_);
                state_ = State::AddrLo;
            }
            break;
        }
    }

    std::vector<uint8_t> dataBuf_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ControlDescriptor
//   Describes one addressable DCS-BIOS output control.
//   Loaded from the JSON control-reference at startup (or embedded for offline).
// ─────────────────────────────────────────────────────────────────────────────
struct ControlDescriptor {
    std::string  identifier;   // e.g. "UFC_OPTION1"
    std::string  category;     // e.g. "UFC"
    std::string  description;  // human-readable
    std::string  controlType;  // e.g. selector, potentiometer, gauge, led, metadata
    std::string  module;       // e.g. "FA-18C_hornet"
    uint16_t     byteAddr  = 0;
    uint16_t     mask      = 0xFFFF;
    uint8_t      shift     = 0;
    uint32_t     maxVal    = 0;
    bool         isString  = false;
    uint16_t     strLen    = 0;
    bool         hasSetStateInput = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// ImportCommand
//   One text command to send to DCS-BIOS (UDP 7778 / TCP 7778).
// ─────────────────────────────────────────────────────────────────────────────
struct ImportCommand {
    std::string identifier;
    std::string action;   // SET_STATE, TOGGLE, INC, DEC, etc.
    std::string value;    // empty for action-only commands

    std::string toLine() const {
        if (value.empty()) {
            return identifier + " " + action + "\n";
        }
        return identifier + " " + action + " " + value + "\n";
    }
};

} // namespace dcsbios
