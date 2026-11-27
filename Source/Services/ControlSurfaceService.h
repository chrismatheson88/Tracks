#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "../Database/DatabaseService.h"
#include "../Models/MidiBinding.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tracks {

struct MidiInputDeviceInfo {
    std::string identifier;
    std::string name;
};

class ControlSurfaceService : private juce::MidiInputCallback {
public:
    static constexpr const char* kAllMidiInputsId = "__ALL__";

    explicit ControlSurfaceService(DatabaseService& db);
    ~ControlSurfaceService() override;

    // Callbacks (always delivered on the JUCE Message Thread)
    std::function<void(ControlAction)> onActionTriggered;
    std::function<void(ControlAction, MidiBinding)> onMidiLearned;
    std::function<void(const std::string&)> onMidiActivity;

    // Keyboard shortcuts setting & bindings
    bool isKeyboardShortcutsEnabled() const { return keyboardShortcutsEnabled_.load(std::memory_order_relaxed); }
    void setKeyboardShortcutsEnabled(bool enabled);

    std::optional<KeyBinding> getKeyBinding(ControlAction action) const;
    void setKeyBinding(ControlAction action, const KeyBinding& binding);
    void clearKeyBinding(ControlAction action);
    void resetDefaultKeyBindings();
    void startKeyLearn(ControlAction action);
    void cancelKeyLearn();
    std::optional<ControlAction> getLearningKeyAction() const;
    std::optional<ControlAction> matchKeyPress(const juce::KeyPress& key, bool allowSongNav) const;

    // MIDI device management
    std::vector<MidiInputDeviceInfo> getAvailableMidiInputs() const;
    std::string getSelectedMidiInputIdentifier() const;
    void selectMidiInput(const std::string& identifier); // kAllMidiInputsId, empty (off), or device identifier
    void refreshMidiInputs();
    int getActiveMidiInputCount() const;

    // MIDI bindings & MIDI Learn
    std::optional<MidiBinding> getBinding(ControlAction action) const;
    void setBinding(ControlAction action, const MidiBinding& binding);
    void clearBinding(ControlAction action);
    void clearAllBindings();

    void startMidiLearn(ControlAction action);
    void cancelMidiLearn();
    std::optional<ControlAction> getLearningAction() const;

    void reloadFromDatabase();

private:
    DatabaseService& db_;
    mutable std::mutex mutex_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    std::atomic<bool> keyboardShortcutsEnabled_{true};
    std::string selectedInputId_ = kAllMidiInputsId;
    std::vector<std::unique_ptr<juce::MidiInput>> openInputs_;

    std::array<std::optional<MidiBinding>, static_cast<size_t>(ControlAction::Count)> bindings_;
    std::array<std::optional<KeyBinding>, static_cast<size_t>(ControlAction::Count)> keyBindings_;
    std::array<int64_t, static_cast<size_t>(ControlAction::Count)> lastTriggerMs_{};
    std::optional<ControlAction> learningAction_;
    std::optional<ControlAction> learningKeyAction_;

    void openConfiguredInputsLocked();
    void closeAllInputsLocked();
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
};

} // namespace tracks
