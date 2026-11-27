#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>

namespace tracks {

enum class ControlAction : int {
    PlayPause = 0,
    Stop = 1,
    FadeOutStop = 2,
    RepeatSection = 3,
    SkipBackSection = 4,
    SkipForwardSection = 5,
    PreviousSong = 6,
    NextSong = 7,
    Count = 8
};

struct ControlActionMeta {
    ControlAction action;
    const char* settingKey;
    const char* displayName;
    const char* shortcutLabel;
};

inline const std::array<ControlActionMeta, static_cast<size_t>(ControlAction::Count)>& getAllControlActionMeta() {
    static const std::array<ControlActionMeta, static_cast<size_t>(ControlAction::Count)> kMeta = {{
        { ControlAction::PlayPause,          "play_pause",   "Play / Pause",         "Space" },
        { ControlAction::Stop,               "stop",         "Stop",                 "Esc" },
        { ControlAction::FadeOutStop,        "fade_out",     "Fade Out & Stop (4s)", "F" },
        { ControlAction::RepeatSection,      "repeat",       "Repeat / Click Loop",  "R" },
        { ControlAction::SkipBackSection,    "skip_back",    "Previous Section",     "Left" },
        { ControlAction::SkipForwardSection, "skip_forward", "Next Section",         "Right" },
        { ControlAction::PreviousSong,       "prev_song",    "Previous Song",        "[" },
        { ControlAction::NextSong,           "next_song",    "Next Song",            "]" }
    }};
    return kMeta;
}

inline const ControlActionMeta& getControlActionMeta(ControlAction action) {
    const auto& all = getAllControlActionMeta();
    const size_t idx = std::clamp<size_t>(static_cast<size_t>(action), 0, all.size() - 1);
    return all[idx];
}

enum class MidiBindingType : int {
    None = 0,
    NoteOn = 1,
    ControlChange = 2,
    ProgramChange = 3
};

struct MidiBinding {
    MidiBindingType type = MidiBindingType::None;
    int channel = 0; // 1..16, or 0 for Any
    int data1 = 0;   // Note number, CC number, or Program number (0..127)

    bool isValid() const {
        return type != MidiBindingType::None && channel >= 0 && channel <= 16 && data1 >= 0 && data1 <= 127;
    }

    bool operator==(const MidiBinding& o) const {
        return type == o.type && channel == o.channel && data1 == o.data1;
    }

    // Extract a trigger binding from an incoming MIDI message (ignores Note Off, Note On vel=0, CC release < 64)
    static std::optional<MidiBinding> fromIncomingMessage(const juce::MidiMessage& msg) {
        if (msg.isNoteOn(false)) { // velocity > 0 only
            return MidiBinding{ MidiBindingType::NoteOn, msg.getChannel(), msg.getNoteNumber() };
        }
        if (msg.isController() && msg.getControllerValue() >= 64) {
            return MidiBinding{ MidiBindingType::ControlChange, msg.getChannel(), msg.getControllerNumber() };
        }
        if (msg.isProgramChange()) {
            return MidiBinding{ MidiBindingType::ProgramChange, msg.getChannel(), msg.getProgramChangeNumber() };
        }
        return std::nullopt;
    }

    bool matches(const juce::MidiMessage& msg) const {
        if (!isValid()) return false;
        auto incoming = fromIncomingMessage(msg);
        if (!incoming.has_value()) return false;
        if (incoming->type != type || incoming->data1 != data1) return false;
        return (channel == 0 || incoming->channel == channel);
    }

    std::string serialize() const {
        if (!isValid()) return "";
        const char* prefix = "";
        switch (type) {
            case MidiBindingType::NoteOn:        prefix = "note"; break;
            case MidiBindingType::ControlChange: prefix = "cc"; break;
            case MidiBindingType::ProgramChange: prefix = "pc"; break;
            case MidiBindingType::None:
            default: return "";
        }
        return std::string(prefix) + ":" + std::to_string(channel) + ":" + std::to_string(data1);
    }

    static std::optional<MidiBinding> deserialize(const std::string& raw) {
        if (raw.empty()) return std::nullopt;
        const auto p1 = raw.find(':');
        if (p1 == std::string::npos) return std::nullopt;
        const auto p2 = raw.find(':', p1 + 1);
        if (p2 == std::string::npos) return std::nullopt;

        const std::string kind = raw.substr(0, p1);
        MidiBinding b;
        if (kind == "note") b.type = MidiBindingType::NoteOn;
        else if (kind == "cc") b.type = MidiBindingType::ControlChange;
        else if (kind == "pc") b.type = MidiBindingType::ProgramChange;
        else return std::nullopt;

        try {
            b.channel = std::stoi(raw.substr(p1 + 1, p2 - p1 - 1));
            b.data1 = std::stoi(raw.substr(p2 + 1));
        } catch (...) {
            return std::nullopt;
        }
        return b.isValid() ? std::optional<MidiBinding>(b) : std::nullopt;
    }

    std::string toDisplayString() const {
        if (!isValid()) return "--";
        std::string chPrefix = (channel > 0) ? ("Ch " + std::to_string(channel) + " ") : "Any ";
        switch (type) {
            case MidiBindingType::NoteOn: {
                static const char* kNotes[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
                const int octave = (data1 / 12) - 1;
                return chPrefix + "Note " + std::string(kNotes[data1 % 12]) + std::to_string(octave) +
                       " (" + std::to_string(data1) + ")";
            }
            case MidiBindingType::ControlChange:
                return chPrefix + "CC " + std::to_string(data1);
            case MidiBindingType::ProgramChange:
                return chPrefix + "PC " + std::to_string(data1);
            case MidiBindingType::None:
            default:
                return "--";
        }
    }
};

struct KeyBinding {
    static constexpr int kModShift = 1 << 0;
    static constexpr int kModCtrl  = 1 << 1;
    static constexpr int kModAlt   = 1 << 2;
    static constexpr int kModCmd   = 1 << 3;

    int keyCode = 0;   // JUCE KeyPress keyCode (ASCII letters normalized to uppercase A-Z)
    int modifiers = 0; // Bitmask of kModShift | kModCtrl | kModAlt | kModCmd

    bool isValid() const { return keyCode != 0; }

    bool operator==(const KeyBinding& o) const {
        return keyCode == o.keyCode && modifiers == o.modifiers;
    }

    static int normalizeKeyCode(int kc) {
        if (kc >= 'a' && kc <= 'z') {
            return kc - 'a' + 'A';
        }
        return kc;
    }

    static KeyBinding defaultForAction(ControlAction action) {
        switch (action) {
            case ControlAction::PlayPause:          return { juce::KeyPress::spaceKey, 0 };
            case ControlAction::Stop:               return { juce::KeyPress::escapeKey, 0 };
            case ControlAction::FadeOutStop:        return { 'F', 0 };
            case ControlAction::RepeatSection:      return { 'R', 0 };
            case ControlAction::SkipBackSection:    return { juce::KeyPress::leftKey, 0 };
            case ControlAction::SkipForwardSection: return { juce::KeyPress::rightKey, 0 };
            case ControlAction::PreviousSong:       return { '[', 0 };
            case ControlAction::NextSong:           return { ']', 0 };
            case ControlAction::Count:
            default:                                return { 0, 0 };
        }
    }

    static std::optional<KeyBinding> fromKeyPress(const juce::KeyPress& kp) {
        int kc = normalizeKeyCode(kp.getKeyCode());
        if (kc == 0) {
            const auto ch = kp.getTextCharacter();
            if (ch > 0 && ch < 128) {
                kc = normalizeKeyCode(static_cast<int>(ch));
            }
        }
        if (kc == 0) return std::nullopt;

        const auto mods = kp.getModifiers();
        int m = 0;
        if (mods.isShiftDown())   m |= kModShift;
        if (mods.isCtrlDown())    m |= kModCtrl;
        if (mods.isAltDown())     m |= kModAlt;
        if (mods.isCommandDown()) m |= kModCmd;
        return KeyBinding{ kc, m };
    }

    bool matches(const juce::KeyPress& kp) const {
        if (!isValid()) return false;
        auto incoming = fromKeyPress(kp);
        return incoming.has_value() && (*incoming == *this);
    }

    std::string serialize() const {
        if (!isValid()) return "none";
        return std::to_string(modifiers) + ":" + std::to_string(keyCode);
    }

    static std::optional<KeyBinding> deserialize(const std::string& raw) {
        if (raw.empty() || raw == "none") return std::nullopt;
        const auto sep = raw.find(':');
        if (sep == std::string::npos) return std::nullopt;
        try {
            int m = std::stoi(raw.substr(0, sep));
            int kc = normalizeKeyCode(std::stoi(raw.substr(sep + 1)));
            if (kc == 0 || m < 0 || m > (kModShift | kModCtrl | kModAlt | kModCmd)) {
                return std::nullopt;
            }
            return KeyBinding{ kc, m };
        } catch (...) {
            return std::nullopt;
        }
    }

    std::string toDisplayString() const {
        if (!isValid()) return "--";
        std::string out;
        if (modifiers & kModCmd)   out += "Cmd+";
        if (modifiers & kModCtrl)  out += "Ctrl+";
        if (modifiers & kModAlt)   out += "Alt+";
        if (modifiers & kModShift) out += "Shift+";

        if (keyCode == juce::KeyPress::spaceKey)          out += "Space";
        else if (keyCode == juce::KeyPress::escapeKey)    out += "Esc";
        else if (keyCode == juce::KeyPress::returnKey)    out += "Enter";
        else if (keyCode == juce::KeyPress::tabKey)       out += "Tab";
        else if (keyCode == juce::KeyPress::backspaceKey) out += "Bksp";
        else if (keyCode == juce::KeyPress::deleteKey)    out += "Del";
        else if (keyCode == juce::KeyPress::leftKey)      out += "Left";
        else if (keyCode == juce::KeyPress::rightKey)     out += "Right";
        else if (keyCode == juce::KeyPress::upKey)        out += "Up";
        else if (keyCode == juce::KeyPress::downKey)      out += "Down";
        else if (keyCode == juce::KeyPress::pageUpKey)    out += "PgUp";
        else if (keyCode == juce::KeyPress::pageDownKey)  out += "PgDn";
        else if (keyCode == juce::KeyPress::homeKey)      out += "Home";
        else if (keyCode == juce::KeyPress::endKey)       out += "End";
        else if (keyCode >= juce::KeyPress::F1Key && keyCode <= juce::KeyPress::F12Key) {
            out += "F" + std::to_string(1 + (keyCode - juce::KeyPress::F1Key));
        } else if (keyCode >= 33 && keyCode <= 126) {
            out.push_back(static_cast<char>(keyCode));
        } else {
            out += "Key" + std::to_string(keyCode);
        }
        return out;
    }
};

} // namespace tracks
