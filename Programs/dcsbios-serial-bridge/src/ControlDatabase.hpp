#pragma once
// ControlDatabase
//   Loads DCS-BIOS control metadata from the JSON control-reference files
//   (Scripts/DCS-BIOS/doc/json/*.json next to the running exe, or beside the
//   binary in the release package).
//
//   Also knows how to convert a raw integer value to a human-readable label
//   using the optionally embedded value_map from the JSON.
//
//   Thread-safety: load() is called once at startup from the UI thread.
//   lookupByAddr() is called from the bridge thread (read-only after load).

#include "BiosProtocol.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dcsbios {

class ControlDatabase {
public:
    // Load all *.json files under the given directory.
    // Returns number of controls loaded.
    size_t load(const std::string& jsonDir, const std::string& moduleFilter = {}) {
        byAddr_.clear();
        byId_.clear();

        try {
            namespace fs = std::filesystem;
            fs::path dir(jsonDir);
            if (!fs::is_directory(dir)) return 0;
            for (auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    if (!moduleFilter.empty() && entry.path().stem().string() != moduleFilter) {
                        continue;
                    }
                    loadFile(entry.path().string());
                }
            }
        } catch (...) {}

        return byId_.size();
    }

    // Look up controls by their 16-bit word address (may be multiple).
    const std::vector<const ControlDescriptor*>* lookupByAddr(uint16_t byteAddr) const {
        auto it = byAddr_.find(byteAddr);
        if (it == byAddr_.end()) return nullptr;
        return &it->second;
    }

    const ControlDescriptor* lookupById(const std::string& id) const {
        auto it = byId_.find(id);
        if (it == byId_.end()) return nullptr;
        return &it->second;
    }

    bool empty() const { return byId_.empty(); }

private:
    std::unordered_map<uint16_t, std::vector<const ControlDescriptor*>> byAddr_;
    std::unordered_map<std::string, ControlDescriptor> byId_;

    // Minimal JSON parser sufficient for the DCS-BIOS control reference format.
    // The format is a flat object of category objects, each containing control
    // objects with outputs arrays.  We only need a small subset.
    void loadFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

        // Determine module name from filename (strip directory and .json)
        std::string module;
        auto slash = path.find_last_of("\\/");
        auto dot   = path.rfind('.');
        if (slash != std::string::npos && dot != std::string::npos && dot > slash) {
            module = path.substr(slash + 1, dot - slash - 1);
        }

        // Walk through "identifier": { ... "outputs": [ { "address": N, "mask": N, "shift_by": N ... }] }
        // We do simple substring scanning — no full JSON parse needed.
        size_t pos = 0;
        auto skipWs = [&]() { while (pos < json.size() && std::isspace((unsigned char)json[pos])) ++pos; };
        auto readString = [&]() -> std::string {
            if (pos >= json.size() || json[pos] != '"') return {};
            ++pos;
            std::string s;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\') { ++pos; }
                if (pos < json.size()) s += json[pos++];
            }
            if (pos < json.size()) ++pos; // closing "
            return s;
        };
        auto readNumber = [&]() -> long long {
            skipWs();
            bool neg = false;
            if (pos < json.size() && json[pos] == '-') { neg = true; ++pos; }
            long long n = 0;
            while (pos < json.size() && std::isdigit((unsigned char)json[pos])) {
                n = n * 10 + (json[pos++] - '0');
            }
            // skip hex (0x...)
            if (pos + 1 < json.size() && json[pos] == 'x') {
                ++pos;
                n = 0;
                while (pos < json.size() && std::isxdigit((unsigned char)json[pos])) {
                    unsigned char c = (unsigned char)json[pos++];
                    n = n * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
                }
            }
            return neg ? -n : n;
        };

        // Top level: object of categories
        skipWs(); if (pos >= json.size() || json[pos] != '{') return; ++pos;
        while (pos < json.size()) {
            skipWs();
            if (json[pos] == '}') break;
            if (json[pos] != '"') { ++pos; continue; }
            std::string category = readString();
            skipWs(); if (pos >= json.size() || json[pos] != ':') { ++pos; continue; } ++pos;
            skipWs(); if (pos >= json.size() || json[pos] != '{') { ++pos; continue; } ++pos;

            // controls in this category
            while (pos < json.size()) {
                skipWs();
                if (json[pos] == '}') { ++pos; break; }
                if (json[pos] != '"') { ++pos; continue; }
                std::string identifier = readString();
                skipWs(); if (pos >= json.size() || json[pos] != ':') { ++pos; continue; } ++pos;
                skipWs(); if (pos >= json.size() || json[pos] != '{') { ++pos; continue; } ++pos;

                // fields inside control object
                ControlDescriptor desc;
                desc.identifier = identifier;
                desc.category   = category;
                desc.module     = module;
                int depth = 1;
                std::string lastKey;

                while (pos < json.size() && depth > 0) {
                    skipWs();
                    char c = json[pos];
                    if (c == '{' || c == '[') { ++depth; ++pos; continue; }
                    if (c == '}' || c == ']') { --depth; ++pos; continue; }
                    if (c == ':') { ++pos; continue; }
                    if (c == ',') { ++pos; continue; }
                    if (c == '"') {
                        std::string token = readString();
                        skipWs();
                        if (pos < json.size() && json[pos] == ':') {
                            ++pos; lastKey = token;
                        } else {
                            // value string — only capture control-level description (depth 1),
                            // not output/input descriptions nested deeper
                            if (lastKey == "description" && depth == 1)
                                desc.description = token;
                            if (lastKey == "control_type" && depth == 1)
                                desc.controlType = token;
                            if (lastKey == "interface" && token == "set_state")
                                desc.hasSetStateInput = true;
                        }
                        continue;
                    }
                    if (std::isdigit((unsigned char)c) || c == '-') {
                        long long n = readNumber();
                        if (lastKey == "address")  desc.byteAddr = static_cast<uint16_t>(n);
                        else if (lastKey == "mask") desc.mask = static_cast<uint16_t>(n);
                        else if (lastKey == "shift_by") desc.shift = static_cast<uint8_t>(n);
                        else if (lastKey == "max_value") desc.maxVal = static_cast<uint32_t>(n);
                        else if (lastKey == "max_length") { desc.strLen = static_cast<uint16_t>(n); desc.isString = true; }
                        continue;
                    }
                    // booleans / null
                    ++pos;
                }

                if (!desc.identifier.empty() && (desc.mask != 0 || desc.isString)) {
                    auto [it, ok] = byId_.emplace(desc.identifier, desc);
                    if (ok) {
                        byAddr_[desc.byteAddr].push_back(&it->second);
                    }
                }

                skipWs(); if (pos < json.size() && json[pos] == ',') ++pos;
            }
            skipWs(); if (pos < json.size() && json[pos] == ',') ++pos;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// HumanReadable
//   Converts a dirty word list + state map into human-readable change lines.
// ─────────────────────────────────────────────────────────────────────────────
inline std::wstring FormatChange(const ControlDescriptor& desc, const BiosStateMap& state) {
    auto trim = [](std::wstring value) {
        while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
        while (!value.empty() && iswspace(value.back())) value.pop_back();
        return value;
    };

    auto toWide = [](const std::string& value) {
        return std::wstring(value.begin(), value.end());
    };

    auto splitPositions = [&](const std::wstring& description) {
        std::vector<std::wstring> positions;
        auto comma = description.find(L',');
        if (comma == std::wstring::npos) return positions;

        std::wstring suffix = trim(description.substr(comma + 1));
        size_t start = 0;
        while (start < suffix.size()) {
            size_t slash = suffix.find(L'/', start);
            std::wstring token = trim(suffix.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start));
            if (!token.empty()) positions.push_back(token);
            if (slash == std::wstring::npos) break;
            start = slash + 1;
        }
        return positions;
    };

    auto displayLabel = [&](const ControlDescriptor& control) {
        std::wstring description = trim(toWide(control.description));
        auto comma = description.find(L',');
        if (comma != std::wstring::npos) {
            description = trim(description.substr(0, comma));
        }
        if (!description.empty()) return description;
        return toWide(control.identifier);
    };

    wchar_t buf[512];
    if (desc.isString) {
        std::string s;
        for (uint16_t i = 0; i < desc.strLen; ++i) {
            char ch = static_cast<char>(state.raw()[desc.byteAddr + i]);
            if (ch == 0) break;
            s += ch;
        }
        std::wstring label = displayLabel(desc);
        std::wstring id    = toWide(desc.identifier);
        std::wstring ws(s.begin(), s.end());
        // Wire format: IDENTIFIER SET_STATE "value"  (Label: "value")
        swprintf_s(buf, L"%s SET_STATE \"%s\"  (%s: \"%s\")", id.c_str(), ws.c_str(), label.c_str(), ws.c_str());
    } else {
        uint16_t val = state.readField(desc.byteAddr, desc.mask, desc.shift);
        std::wstring label = displayLabel(desc);
        std::wstring id    = toWide(desc.identifier);
        std::wstring valueText;
        auto positions = splitPositions(toWide(desc.description));

        if (desc.maxVal == 1) {
            if (positions.size() == 2) {
                valueText = (val == 0) ? positions[1] : positions[0];
            } else {
                valueText = (val == 0) ? L"OFF" : L"ON";
            }
        } else if (!positions.empty() && val < positions.size()) {
            valueText = positions[val];
        } else {
            valueText = std::to_wstring(static_cast<unsigned>(val));
        }

        // Wire format: IDENTIFIER SET_STATE N  (Human Label: State)
        swprintf_s(buf, L"%s SET_STATE %u  (%s: %s)",
                   id.c_str(), static_cast<unsigned>(val), label.c_str(), valueText.c_str());
    }
    return buf;
}

inline bool ShouldLogStateChange(const ControlDescriptor& desc,
                                 bool includeKnobDialRaw,
                                 bool includeGaugeRaw) {
    if (desc.isString) return false;

    // Pilot-actuated controls that accept SET_STATE.
    if (desc.hasSetStateInput) {
        if (desc.maxVal <= 32) return true;      // switches/selectors
        return includeKnobDialRaw;               // potentiometers/large-range dials
    }

    // Optional read-only gauge stream for debugging.
    if (!includeGaugeRaw) return false;

    std::string typeLower = desc.controlType;
    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return typeLower == "gauge";
}

} // namespace dcsbios
