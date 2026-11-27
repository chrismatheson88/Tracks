#include "ControlSurfaceService.h"
#include <juce_events/juce_events.h>

namespace tracks {

namespace {

constexpr const char* kSettingKeyboardEnabled = "controls.keyboard_enabled";
constexpr const char* kSettingMidiInputDevice = "midi.input_device";
constexpr int64_t kMidiDebounceMs = 60;

std::string bindingSettingKey(ControlAction action) {
    return std::string("midi.binding.") + getControlActionMeta(action).settingKey;
}

std::string keyBindingSettingKey(ControlAction action) {
    return std::string("controls.key.") + getControlActionMeta(action).settingKey;
}

} // namespace

ControlSurfaceService::ControlSurfaceService(DatabaseService& db)
    : db_(db) {
    lastTriggerMs_.fill(0);
    reloadFromDatabase();
}

ControlSurfaceService::~ControlSurfaceService() {
    alive_->store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    closeAllInputsLocked();
}

void ControlSurfaceService::reloadFromDatabase() {
    const std::string kbVal = db_.getAppSetting(kSettingKeyboardEnabled, "1");
    keyboardShortcutsEnabled_.store(kbVal != "0" && kbVal != "false", std::memory_order_relaxed);

    const std::string savedInput = db_.getAppSetting(kSettingMidiInputDevice, kAllMidiInputsId);

    std::lock_guard<std::mutex> lock(mutex_);
    selectedInputId_ = savedInput;
    learningAction_.reset();
    learningKeyAction_.reset();

    for (const auto& meta : getAllControlActionMeta()) {
        const std::string raw = db_.getAppSetting(bindingSettingKey(meta.action), "");
        bindings_[static_cast<size_t>(meta.action)] = MidiBinding::deserialize(raw);

        const std::string rawKey = db_.getAppSetting(keyBindingSettingKey(meta.action), "");
        if (rawKey.empty()) {
            keyBindings_[static_cast<size_t>(meta.action)] = KeyBinding::defaultForAction(meta.action);
        } else if (rawKey == "none") {
            keyBindings_[static_cast<size_t>(meta.action)].reset();
        } else {
            auto parsed = KeyBinding::deserialize(rawKey);
            keyBindings_[static_cast<size_t>(meta.action)] = parsed.has_value()
                                                                 ? parsed
                                                                 : std::optional<KeyBinding>(KeyBinding::defaultForAction(meta.action));
        }
    }

    openConfiguredInputsLocked();
}

void ControlSurfaceService::setKeyboardShortcutsEnabled(bool enabled) {
    keyboardShortcutsEnabled_.store(enabled, std::memory_order_relaxed);
    db_.setAppSetting(kSettingKeyboardEnabled, enabled ? "1" : "0");
}

std::optional<KeyBinding> ControlSurfaceService::getKeyBinding(ControlAction action) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t idx = static_cast<size_t>(action);
    if (idx >= keyBindings_.size()) return std::nullopt;
    return keyBindings_[idx];
}

void ControlSurfaceService::setKeyBinding(ControlAction action, const KeyBinding& binding) {
    if (!binding.isValid()) {
        clearKeyBinding(action);
        return;
    }

    std::vector<ControlAction> clearedConflicts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t idx = static_cast<size_t>(action);
        if (idx >= keyBindings_.size()) return;

        for (size_t i = 0; i < keyBindings_.size(); ++i) {
            if (i != idx && keyBindings_[i].has_value() && *keyBindings_[i] == binding) {
                keyBindings_[i].reset();
                clearedConflicts.push_back(static_cast<ControlAction>(i));
            }
        }
        keyBindings_[idx] = binding;
        learningKeyAction_.reset();
    }

    for (auto conflict : clearedConflicts) {
        db_.setAppSetting(keyBindingSettingKey(conflict), "none");
    }
    db_.setAppSetting(keyBindingSettingKey(action), binding.serialize());
}

void ControlSurfaceService::clearKeyBinding(ControlAction action) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t idx = static_cast<size_t>(action);
        if (idx >= keyBindings_.size()) return;
        keyBindings_[idx].reset();
        if (learningKeyAction_.has_value() && *learningKeyAction_ == action) {
            learningKeyAction_.reset();
        }
    }
    db_.setAppSetting(keyBindingSettingKey(action), "none");
}

void ControlSurfaceService::resetDefaultKeyBindings() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        learningKeyAction_.reset();
        for (const auto& meta : getAllControlActionMeta()) {
            keyBindings_[static_cast<size_t>(meta.action)] = KeyBinding::defaultForAction(meta.action);
        }
    }
    for (const auto& meta : getAllControlActionMeta()) {
        db_.setAppSetting(keyBindingSettingKey(meta.action), KeyBinding::defaultForAction(meta.action).serialize());
    }
}

void ControlSurfaceService::startKeyLearn(ControlAction action) {
    std::lock_guard<std::mutex> lock(mutex_);
    learningAction_.reset();
    learningKeyAction_ = action;
}

void ControlSurfaceService::cancelKeyLearn() {
    std::lock_guard<std::mutex> lock(mutex_);
    learningKeyAction_.reset();
}

std::optional<ControlAction> ControlSurfaceService::getLearningKeyAction() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return learningKeyAction_;
}

std::optional<ControlAction> ControlSurfaceService::matchKeyPress(const juce::KeyPress& key, bool allowSongNav) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < keyBindings_.size(); ++i) {
        const auto act = static_cast<ControlAction>(i);
        if (!allowSongNav && (act == ControlAction::PreviousSong || act == ControlAction::NextSong)) {
            continue;
        }
        if (keyBindings_[i].has_value() && keyBindings_[i]->matches(key)) {
            return act;
        }
    }
    return std::nullopt;
}

std::vector<MidiInputDeviceInfo> ControlSurfaceService::getAvailableMidiInputs() const {
    std::vector<MidiInputDeviceInfo> out;
    auto devices = juce::MidiInput::getAvailableDevices();
    out.reserve(static_cast<size_t>(devices.size()));
    for (const auto& dev : devices) {
        out.push_back({ dev.identifier.toStdString(), dev.name.toStdString() });
    }
    return out;
}

std::string ControlSurfaceService::getSelectedMidiInputIdentifier() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return selectedInputId_;
}

void ControlSurfaceService::selectMidiInput(const std::string& identifier) {
    db_.setAppSetting(kSettingMidiInputDevice, identifier);
    std::lock_guard<std::mutex> lock(mutex_);
    selectedInputId_ = identifier;
    openConfiguredInputsLocked();
}

void ControlSurfaceService::refreshMidiInputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    openConfiguredInputsLocked();
}

int ControlSurfaceService::getActiveMidiInputCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(openInputs_.size());
}

void ControlSurfaceService::closeAllInputsLocked() {
    for (auto& inp : openInputs_) {
        if (inp) {
            inp->stop();
        }
    }
    openInputs_.clear();
}

void ControlSurfaceService::openConfiguredInputsLocked() {
    closeAllInputsLocked();

    if (selectedInputId_.empty()) {
        return; // MIDI input explicitly disabled
    }

    auto devices = juce::MidiInput::getAvailableDevices();
    for (const auto& dev : devices) {
        const std::string devId = dev.identifier.toStdString();
        const std::string devName = dev.name.toStdString();
        const bool match = (selectedInputId_ == kAllMidiInputsId) ||
                           (devId == selectedInputId_) ||
                           (devName == selectedInputId_);
        if (!match) continue;

        if (auto opened = juce::MidiInput::openDevice(dev.identifier, this)) {
            opened->start();
            openInputs_.push_back(std::move(opened));
        }
    }
}

std::optional<MidiBinding> ControlSurfaceService::getBinding(ControlAction action) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t idx = static_cast<size_t>(action);
    if (idx >= bindings_.size()) return std::nullopt;
    return bindings_[idx];
}

void ControlSurfaceService::setBinding(ControlAction action, const MidiBinding& binding) {
    if (!binding.isValid()) {
        clearBinding(action);
        return;
    }

    std::vector<ControlAction> clearedConflicts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t idx = static_cast<size_t>(action);
        if (idx >= bindings_.size()) return;

        // Avoid duplicate bindings by clearing any other action using the exact same MIDI trigger
        for (size_t i = 0; i < bindings_.size(); ++i) {
            if (i != idx && bindings_[i].has_value() && *bindings_[i] == binding) {
                bindings_[i].reset();
                clearedConflicts.push_back(static_cast<ControlAction>(i));
            }
        }
        bindings_[idx] = binding;
    }

    for (auto conflict : clearedConflicts) {
        db_.setAppSetting(bindingSettingKey(conflict), "");
    }
    db_.setAppSetting(bindingSettingKey(action), binding.serialize());
}

void ControlSurfaceService::clearBinding(ControlAction action) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t idx = static_cast<size_t>(action);
        if (idx >= bindings_.size()) return;
        bindings_[idx].reset();
    }
    db_.setAppSetting(bindingSettingKey(action), "");
}

void ControlSurfaceService::clearAllBindings() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& b : bindings_) {
            b.reset();
        }
        learningAction_.reset();
    }
    for (const auto& meta : getAllControlActionMeta()) {
        db_.setAppSetting(bindingSettingKey(meta.action), "");
    }
}

void ControlSurfaceService::startMidiLearn(ControlAction action) {
    std::lock_guard<std::mutex> lock(mutex_);
    learningKeyAction_.reset();
    learningAction_ = action;
}

void ControlSurfaceService::cancelMidiLearn() {
    std::lock_guard<std::mutex> lock(mutex_);
    learningAction_.reset();
}

std::optional<ControlAction> ControlSurfaceService::getLearningAction() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return learningAction_;
}

void ControlSurfaceService::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) {
    auto incomingOpt = MidiBinding::fromIncomingMessage(message);
    if (!incomingOpt.has_value()) {
        return;
    }
    const MidiBinding incoming = *incomingOpt;
    const std::string activityDesc = incoming.toDisplayString();

    std::optional<ControlAction> learnedAction;
    std::vector<ControlAction> triggeredActions;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (learningAction_.has_value()) {
            learnedAction = *learningAction_;
            learningAction_.reset();
        } else {
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            for (size_t i = 0; i < bindings_.size(); ++i) {
                if (bindings_[i].has_value() && bindings_[i]->matches(message)) {
                    if (nowMs - lastTriggerMs_[i] >= kMidiDebounceMs) {
                        lastTriggerMs_[i] = nowMs;
                        triggeredActions.push_back(static_cast<ControlAction>(i));
                    }
                }
            }
        }
    }

    auto alive = alive_;
    if (learnedAction.has_value()) {
        const ControlAction act = *learnedAction;
        juce::MessageManager::callAsync([this, alive, act, incoming, activityDesc]() {
            if (!alive->load(std::memory_order_acquire)) return;
            setBinding(act, incoming);
            if (onMidiActivity) onMidiActivity("Learned: " + activityDesc);
            if (onMidiLearned) onMidiLearned(act, incoming);
        });
        return;
    }

    juce::MessageManager::callAsync([this, alive, triggeredActions = std::move(triggeredActions), activityDesc]() {
        if (!alive->load(std::memory_order_acquire)) return;
        if (onMidiActivity) onMidiActivity("Last MIDI: " + activityDesc);
        if (onActionTriggered) {
            for (ControlAction act : triggeredActions) {
                onActionTriggered(act);
            }
        }
    });
}

} // namespace tracks
