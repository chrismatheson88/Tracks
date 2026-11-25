#include "TransportComponent.h"
#include "../../Common/AppTheme.h"
#include <algorithm>
#include <cmath>

namespace tracks {

namespace {

std::vector<juce::String> computeShortTrackLabels(const std::vector<SongTrack>& tracks) {
    std::vector<juce::String> labels;
    labels.reserve(tracks.size());
    for (const auto& t : tracks) {
        labels.push_back(t.name.empty() ? "Track" : juce::String(t.name));
    }

    if (labels.size() < 2) {
        return labels;
    }

    // Find longest common prefix across all track names
    juce::String prefix = labels[0];
    for (size_t i = 1; i < labels.size(); ++i) {
        int matchLen = 0;
        int maxLen = std::min(prefix.length(), labels[i].length());
        while (matchLen < maxLen && prefix[matchLen] == labels[i][matchLen]) {
            ++matchLen;
        }
        prefix = prefix.substring(0, matchLen);
        if (prefix.isEmpty()) break;
    }

    // Trim prefix back to a clean delimiter (' ', '-', '_') so words aren't chopped mid-word
    int cutLen = prefix.length();
    while (cutLen > 0) {
        auto c = prefix[cutLen - 1];
        if (c == ' ' || c == '-' || c == '_') break;
        --cutLen;
    }

    if (cutLen > 0) {
        bool allNonEmpty = true;
        std::vector<juce::String> trimmed;
        trimmed.reserve(labels.size());
        for (const auto& lbl : labels) {
            auto s = lbl.substring(cutLen).trim();
            if (s.isEmpty()) {
                allNonEmpty = false;
                break;
            }
            trimmed.push_back(s);
        }
        if (allNonEmpty) {
            return trimmed;
        }
    }

    return labels;
}

constexpr int kStereoChannelItemBaseId = 1000;

void populateChannelCombo(juce::ComboBox& box, int numChannels) {
    int count = std::max(1, numChannels);
    box.clear(juce::dontSendNotification);
    for (int ch = 1; ch <= count; ++ch) {
        box.addItem("Ch " + juce::String(ch), ch);
    }
}

void populateTrackChannelCombo(juce::ComboBox& box, int numDeviceChannels, bool isStereoTrack) {
    int count = std::max(1, numDeviceChannels);
    box.clear(juce::dontSendNotification);

    if (isStereoTrack && count >= 2) {
        box.addSectionHeading("Stereo");
        for (int ch = 0; ch < count - 1; ++ch) {
            box.addItem("Ch " + juce::String(ch + 1) + "-" + juce::String(ch + 2),
                        kStereoChannelItemBaseId + ch);
        }
        box.addSeparator();
        box.addSectionHeading("Mono");
    }

    for (int ch = 0; ch < count; ++ch) {
        box.addItem("Ch " + juce::String(ch + 1), ch + 1);
    }
}

float linearToSliderDb(float linear) {
    if (linear <= 0.001f) return -30.0f;
    return std::clamp(20.0f * std::log10(linear), -30.0f, 6.0f);
}

float sliderDbToLinear(float db) {
    return (db <= -30.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
}

} // namespace

class TransportComponent::TrackStripComponent : public juce::Component {
public:
    TrackStripComponent(const juce::String& shortName,
                        const juce::String& fullName,
                        float initialGain,
                        int initialChannel,
                        bool initialStereoOutput,
                        bool isStereoTrack,
                        int numDeviceChannels,
                        std::function<void(float)> onGainChanged,
                        std::function<void(int, bool)> onChannelChanged)
        : isStereoTrack_(isStereoTrack),
          onGainChanged_(std::move(onGainChanged)),
          onChannelChanged_(std::move(onChannelChanged)) {
        nameLabel_.setText(shortName, juce::dontSendNotification);
        nameLabel_.setTooltip(fullName);
        nameLabel_.setFont(juce::Font(11.0f, juce::Font::bold));
        nameLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
        nameLabel_.setJustificationType(juce::Justification::centred);
        nameLabel_.setMinimumHorizontalScale(0.7f);
        addAndMakeVisible(nameLabel_);

        updateChannelOptions(numDeviceChannels, initialChannel, initialStereoOutput);
        channelBox_.onChange = [this]() {
            int id = channelBox_.getSelectedId();
            if (id >= kStereoChannelItemBaseId) {
                int ch = id - kStereoChannelItemBaseId;
                if (onChannelChanged_) onChannelChanged_(ch, true);
            } else if (id >= 1) {
                int ch = id - 1;
                if (onChannelChanged_) onChannelChanged_(ch, false);
            }
        };
        addAndMakeVisible(channelBox_);

        fader_.setSliderStyle(juce::Slider::LinearVertical);
        fader_.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 54, 18);
        fader_.setTextValueSuffix(" dB");
        fader_.setColour(juce::Slider::textBoxTextColourId, AppTheme::textMuted);
        fader_.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        fader_.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        fader_.setRange(-30.0, 6.0, 0.5);
        fader_.setValue(linearToSliderDb(initialGain), juce::dontSendNotification);
        fader_.onValueChange = [this]() {
            float linear = sliderDbToLinear(static_cast<float>(fader_.getValue()));
            if (onGainChanged_) onGainChanged_(linear);
        };
        addAndMakeVisible(fader_);
    }

    void updateChannelOptions(int numDeviceChannels, int selectedChannel, bool stereoOutput) {
        int count = std::max(1, numDeviceChannels);
        populateTrackChannelCombo(channelBox_, count, isStereoTrack_);
        if (isStereoTrack_ && stereoOutput && count >= 2) {
            int clampedStart = std::clamp(selectedChannel, 0, count - 2);
            channelBox_.setSelectedId(kStereoChannelItemBaseId + clampedStart, juce::dontSendNotification);
        } else {
            int clampedMono = std::clamp(selectedChannel, 0, count - 1);
            channelBox_.setSelectedId(clampedMono + 1, juce::dontSendNotification);
        }
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(isEnabled() ? AppTheme::bgPanel : AppTheme::bgPanel.withAlpha(0.55f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(AppTheme::borderCol);
        g.drawRoundedRectangle(r, 4.0f, 1.0f);
    }

    void resized() override {
        auto inner = getLocalBounds().reduced(4, 8);
        nameLabel_.setBounds(inner.removeFromTop(18));
        inner.removeFromTop(4);
        channelBox_.setBounds(inner.removeFromTop(22));
        inner.removeFromTop(6);
        fader_.setBounds(inner);
    }

private:
    bool isStereoTrack_ = true;
    juce::Label nameLabel_;
    juce::ComboBox channelBox_;
    juce::Slider fader_;
    std::function<void(float)> onGainChanged_;
    std::function<void(int, bool)> onChannelChanged_;
};

class TransportComponent::TransportIconButton : public juce::Button {
public:
    enum class IconType { Previous, Play, Pause, Stop, Next };

    TransportIconButton(const juce::String& name, IconType type, juce::Colour baseColour)
        : juce::Button(name), iconType_(type), baseColour_(baseColour) {
        setTooltip(name);
    }

    void setIconType(IconType type) {
        if (iconType_ != type) {
            iconType_ = type;
            repaint();
        }
    }

    void setBaseColour(juce::Colour col) {
        if (baseColour_ != col) {
            baseColour_ = col;
            repaint();
        }
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        const bool enabled = isEnabled();

        juce::Colour bg = baseColour_;
        if (!enabled) {
            bg = bg.withAlpha(0.45f);
        } else if (shouldDrawButtonAsDown) {
            bg = bg.darker(0.18f);
        } else if (shouldDrawButtonAsHighlighted) {
            bg = bg.brighter(0.14f);
        }

        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 5.0f);

        g.setColour(enabled ? AppTheme::borderCol.brighter(0.1f) : AppTheme::borderCol.withAlpha(0.45f));
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);

        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float iconH = std::min(14.0f, bounds.getHeight() * 0.44f);
        const float halfH = iconH * 0.5f;

        g.setColour(enabled ? AppTheme::textLight : AppTheme::textMuted.withAlpha(0.45f));

        switch (iconType_) {
            case IconType::Play: {
                const float triW = iconH * 0.88f;
                const float x0 = cx - triW * 0.42f;
                const float x1 = x0 + triW;
                juce::Path p;
                p.addTriangle(x0, cy - halfH, x0, cy + halfH, x1, cy);
                g.fillPath(p.createPathWithRoundedCorners(1.5f));
                break;
            }
            case IconType::Pause: {
                const float barW = 4.0f;
                const float gap = 4.5f;
                g.fillRoundedRectangle(cx - gap * 0.5f - barW, cy - halfH, barW, iconH, 1.2f);
                g.fillRoundedRectangle(cx + gap * 0.5f, cy - halfH, barW, iconH, 1.2f);
                break;
            }
            case IconType::Stop: {
                const float sq = iconH * 0.88f;
                g.fillRoundedRectangle(cx - sq * 0.5f, cy - sq * 0.5f, sq, sq, 2.0f);
                break;
            }
            case IconType::Previous: {
                const float barW = 2.8f;
                const float triW = iconH * 0.72f;
                const float totalW = barW + 1.5f + triW;
                const float leftX = cx - totalW * 0.5f;
                g.fillRoundedRectangle(leftX, cy - halfH, barW, iconH, 1.0f);
                const float triLeft = leftX + barW + 1.5f;
                juce::Path p;
                p.addTriangle(triLeft, cy, triLeft + triW, cy - halfH, triLeft + triW, cy + halfH);
                g.fillPath(p.createPathWithRoundedCorners(1.2f));
                break;
            }
            case IconType::Next: {
                const float barW = 2.8f;
                const float triW = iconH * 0.72f;
                const float totalW = triW + 1.5f + barW;
                const float leftX = cx - totalW * 0.5f;
                juce::Path p;
                p.addTriangle(leftX, cy - halfH, leftX, cy + halfH, leftX + triW, cy);
                g.fillPath(p.createPathWithRoundedCorners(1.2f));
                g.fillRoundedRectangle(leftX + triW + 1.5f, cy - halfH, barW, iconH, 1.0f);
                break;
            }
        }
    }

private:
    IconType iconType_;
    juce::Colour baseColour_;
};

TransportComponent::TransportComponent(AudioEngine& audioEngine)
    : audioEngine_(audioEngine) {
    setOpaque(true);

    // Previous song button
    prevSongButton_ = std::make_unique<TransportIconButton>("Previous Song", TransportIconButton::IconType::Previous, AppTheme::bgPanel);
    prevSongButton_->setEnabled(false);
    prevSongButton_->onClick = [this]() {
        if (onPreviousSongClicked) onPreviousSongClicked();
    };
    addChildComponent(prevSongButton_.get());

    // Play/Pause button
    playPauseButton_ = std::make_unique<TransportIconButton>("Play", TransportIconButton::IconType::Play, AppTheme::primaryBlue);
    playPauseButton_->onClick = [this]() {
        if (!audioEngine_.isPlaying()) {
            if (onPlayClicked) onPlayClicked();
        } else if (audioEngine_.isPaused()) {
            if (onPlayClicked) onPlayClicked(); // Resume
        } else {
            if (onPauseClicked) onPauseClicked();
        }
    };
    addAndMakeVisible(playPauseButton_.get());

    // Stop button
    stopButton_ = std::make_unique<TransportIconButton>("Stop", TransportIconButton::IconType::Stop, AppTheme::bgPanel);
    stopButton_->onClick = [this]() {
        if (onStopClicked) onStopClicked();
    };
    addAndMakeVisible(stopButton_.get());

    // Next song button
    nextSongButton_ = std::make_unique<TransportIconButton>("Next Song", TransportIconButton::IconType::Next, AppTheme::bgPanel);
    nextSongButton_->setEnabled(false);
    nextSongButton_->onClick = [this]() {
        if (onNextSongClicked) onNextSongClicked();
    };
    addChildComponent(nextSongButton_.get());

    // Song info labels
    songTitleLabel_.setFont(juce::Font(15.0f, juce::Font::bold));
    songTitleLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    songTitleLabel_.setText("No Song Selected", juce::dontSendNotification);
    addAndMakeVisible(songTitleLabel_);

    songArtistLabel_.setFont(juce::Font(12.0f, juce::Font::plain));
    songArtistLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    songArtistLabel_.setText("Select a song from library or set", juce::dontSendNotification);
    addAndMakeVisible(songArtistLabel_);

    // Editable BPM / Key / Time Signature controls
    tempoEditor_.setFont(juce::Font(11.5f, juce::Font::bold));
    tempoEditor_.setJustification(juce::Justification::centred);
    tempoEditor_.setInputRestrictions(3, "0123456789");
    tempoEditor_.setText("120", false);
    tempoEditor_.onTextChange = [this]() { commitTempoEditor(false); };
    tempoEditor_.onReturnKey = [this]() {
        commitTempoEditor(true);
        unfocusAllComponents();
    };
    tempoEditor_.onFocusLost = [this]() { commitTempoEditor(true); };
    addAndMakeVisible(tempoEditor_);

    bpmSuffixLabel_.setFont(juce::Font(11.0f, juce::Font::bold));
    bpmSuffixLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    bpmSuffixLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(bpmSuffixLabel_);

    keyPrefixLabel_.setFont(juce::Font(11.0f, juce::Font::bold));
    keyPrefixLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    keyPrefixLabel_.setJustificationType(juce::Justification::centredRight);
    addChildComponent(keyPrefixLabel_);

    const char* keys[] = { "C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B",
                           "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm" };
    int kid = 1;
    for (const char* k : keys) {
        keyBox_.addItem(k, kid++);
    }
    keyBox_.setSelectedId(1, juce::dontSendNotification);
    keyBox_.onChange = [this]() {
        std::string newKey = keyBox_.getText().toStdString();
        if (!newKey.empty() && newKey != currentKey_) {
            currentKey_ = newKey;
            notifySongParamsChanged();
        }
    };
    addAndMakeVisible(keyBox_);

    const char* sigs[] = { "4/4", "3/4", "6/8", "12/8", "2/4", "5/4", "7/8" };
    int sid = 1;
    for (const char* s : sigs) {
        timeSigBox_.addItem(s, sid++);
    }
    timeSigBox_.setSelectedId(1, juce::dontSendNotification);
    timeSigBox_.onChange = [this]() {
        std::string newSig = timeSigBox_.getText().toStdString();
        if (!newSig.empty() && newSig != currentTimeSig_) {
            currentTimeSig_ = newSig;
            audioEngine_.setTimeSignature(currentTimeSig_);
            notifySongParamsChanged();
        }
    };
    addAndMakeVisible(timeSigBox_);

    useTracksToggle_.setToggleState(false, juce::dontSendNotification);
    useTracksToggle_.setEnabled(false);
    audioEngine_.setUseTracks(false);
    useTracksToggle_.onClick = [this]() {
        audioEngine_.setUseTracks(useTracksToggle_.getToggleState());
        applyActiveModeSettings();
        updateTrackStripsEnabled();
        resized();
        repaint();
    };
    addAndMakeVisible(useTracksToggle_);

    arrangementBox_.setTextWhenNothingSelected("Arrangement");
    arrangementBox_.setTextWhenNoChoicesAvailable("No arrangements");
    arrangementBox_.onChange = [this]() {
        int idx = arrangementBox_.getSelectedItemIndex();
        if (idx >= 0 && idx < static_cast<int>(currentArrangements_.size())) {
            const auto& arr = currentArrangements_[static_cast<size_t>(idx)];
            if (arr.id.has_value() && onArrangementChanged) {
                onArrangementChanged(*arr.id);
            }
        }
    };
    addChildComponent(arrangementBox_);

    eighthsToggle_.setToggleState(false, juce::dontSendNotification);
    eighthsToggle_.onClick = [this]() {
        audioEngine_.setClickSubdivisions(eighthsToggle_.getToggleState(), sixteenthsToggle_.getToggleState());
        updateSubdivisionFaderVisibility();
        notifyClickSubdivisionsChanged();
        resized();
        repaint();
    };
    addAndMakeVisible(eighthsToggle_);

    sixteenthsToggle_.setToggleState(false, juce::dontSendNotification);
    sixteenthsToggle_.onClick = [this]() {
        audioEngine_.setClickSubdivisions(eighthsToggle_.getToggleState(), sixteenthsToggle_.getToggleState());
        updateSubdivisionFaderVisibility();
        notifyClickSubdivisionsChanged();
        resized();
        repaint();
    };
    addAndMakeVisible(sixteenthsToggle_);

    // Sliders
    auto setupSlider = [this](juce::Slider& s, juce::Label& l, bool initiallyVisible = true) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setTextValueSuffix(" dB");
        s.setColour(juce::Slider::textBoxTextColourId, AppTheme::textMuted);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setRange(-30.0, 6.0, 0.5);
        s.setValue(0.0);
        if (initiallyVisible) addAndMakeVisible(s);
        else addChildComponent(s);

        l.setFont(juce::Font(10.0f, juce::Font::plain));
        l.setColour(juce::Label::textColourId, AppTheme::textMuted);
        l.setJustificationType(juce::Justification::centredLeft);
        l.setMinimumHorizontalScale(0.7f);
        if (initiallyVisible) addAndMakeVisible(l);
        else addChildComponent(l);
    };

    setupSlider(masterSlider_, masterLabel_);
    masterSlider_.onValueChange = [this]() {
        float db = static_cast<float>(masterSlider_.getValue());
        float linear = (db <= -30.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
        if (onMasterGainChanged) onMasterGainChanged(linear);
    };

    setupSlider(clickSlider_, clickLabel_);
    clickSlider_.onValueChange = [this]() {
        float db = static_cast<float>(clickSlider_.getValue());
        float linear = (db <= -30.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
        if (onClickGainChanged) onClickGainChanged(linear);
    };

    setupSlider(eighthClickSlider_, eighthClickLabel_, false);
    eighthClickSlider_.onValueChange = [this]() {
        float linear = sliderDbToLinear(static_cast<float>(eighthClickSlider_.getValue()));
        audioEngine_.setEighthClickGain(linear);
        notifyClickSubdivisionsChanged();
    };

    setupSlider(sixteenthClickSlider_, sixteenthClickLabel_, false);
    sixteenthClickSlider_.onValueChange = [this]() {
        float linear = sliderDbToLinear(static_cast<float>(sixteenthClickSlider_.getValue()));
        audioEngine_.setSixteenthClickGain(linear);
        notifyClickSubdivisionsChanged();
    };

    setupSlider(guideSlider_, guideLabel_);
    guideSlider_.onValueChange = [this]() {
        float db = static_cast<float>(guideSlider_.getValue());
        float linear = (db <= -30.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
        if (onGuideGainChanged) onGuideGainChanged(linear);
    };

    lastDeviceChannelCount_ = std::max(1, audioEngine_.getCurrentOutputChannels());
    populateChannelCombo(clickChannelBox_, lastDeviceChannelCount_);
    clickChannelBox_.setSelectedId(std::clamp(audioEngine_.getClickChannel() + 1, 1, lastDeviceChannelCount_), juce::dontSendNotification);
    clickChannelBox_.onChange = [this]() {
        int ch = std::max(0, clickChannelBox_.getSelectedId() - 1);
        audioEngine_.setClickChannel(ch);
        if (onClickChannelChanged) onClickChannelChanged(ch);
    };
    addChildComponent(clickChannelBox_);

    populateChannelCombo(guideChannelBox_, lastDeviceChannelCount_);
    guideChannelBox_.setSelectedId(std::clamp(audioEngine_.getGuideChannel() + 1, 1, lastDeviceChannelCount_), juce::dontSendNotification);
    guideChannelBox_.onChange = [this]() {
        int ch = std::max(0, guideChannelBox_.getSelectedId() - 1);
        audioEngine_.setGuideChannel(ch);
        if (onGuideChannelChanged) onGuideChannelChanged(ch);
    };
    addChildComponent(guideChannelBox_);

    trackStripsViewport_.setScrollBarsShown(false, true);
    trackStripsViewport_.setScrollBarThickness(8);
    trackStripsContainer_ = std::make_unique<juce::Component>();
    trackStripsViewport_.setViewedComponent(trackStripsContainer_.get(), false);
    addChildComponent(trackStripsViewport_);

    addAndMakeVisible(beatPlayhead_);
    addAndMakeVisible(outputMeter_);

    startTimerHz(60); // 60 FPS smooth playbar and meter animation
}

TransportComponent::~TransportComponent() {
    stopTimer();
}

void TransportComponent::setSongInfo(const std::string& title,
                                     const std::string& artist,
                                     int tempo,
                                     const std::string& key,
                                     const std::string& timeSignature) {
    songTitleLabel_.setText(title.empty() ? "No Song Selected" : title, juce::dontSendNotification);
    songArtistLabel_.setText(artist.empty() ? "—" : artist, juce::dontSendNotification);

    currentTempo_ = std::clamp(tempo > 0 ? tempo : 120, 20, 300);
    currentKey_ = key.empty() ? "C" : key;
    currentTimeSig_ = timeSignature.empty() ? "4/4" : timeSignature;

    tempoEditor_.setText(juce::String(currentTempo_), false);

    auto selectOrAddComboItem = [](juce::ComboBox& box, const std::string& value) {
        juce::String target(value);
        for (int i = 0; i < box.getNumItems(); ++i) {
            if (box.getItemText(i) == target) {
                box.setSelectedItemIndex(i, juce::dontSendNotification);
                return;
            }
        }
        int newId = box.getNumItems() + 1;
        box.addItem(target, newId);
        box.setSelectedId(newId, juce::dontSendNotification);
    };

    selectOrAddComboItem(keyBox_, currentKey_);
    selectOrAddComboItem(timeSigBox_, currentTimeSig_);
}

void TransportComponent::commitTempoEditor(bool clampAndFormat) {
    auto text = tempoEditor_.getText().trim();
    int parsed = text.getIntValue();

    if (clampAndFormat) {
        if (text.isEmpty() || parsed <= 0) {
            tempoEditor_.setText(juce::String(currentTempo_), false);
            return;
        }
        int clamped = std::clamp(parsed, 20, 300);
        tempoEditor_.setText(juce::String(clamped), false);
        if (clamped != currentTempo_) {
            currentTempo_ = clamped;
            audioEngine_.setTempo(currentTempo_);
            notifySongParamsChanged();
        }
    } else if (parsed >= 20 && parsed <= 300 && parsed != currentTempo_) {
        currentTempo_ = parsed;
        audioEngine_.setTempo(currentTempo_);
        notifySongParamsChanged();
    }
}

void TransportComponent::notifySongParamsChanged() {
    const bool usingTracks = useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
    if (!usingTracks) {
        clickOnlyTempo_ = currentTempo_;
        clickOnlyKey_ = currentKey_;
        clickOnlyTimeSig_ = currentTimeSig_;
    }
    if (onSongParamsChanged) {
        onSongParamsChanged(lastSongId_, currentTempo_, currentKey_, currentTimeSig_);
    }
}

void TransportComponent::updateSongParamEditorsEnabled() {
    const bool usingTracks = useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
    tempoEditor_.setEnabled(!usingTracks);
    keyBox_.setEnabled(!usingTracks);
    timeSigBox_.setEnabled(!usingTracks);
}

void TransportComponent::applyActiveModeSettings() {
    const bool usingTracks = useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
    updateSongParamEditorsEnabled();

    const SongArrangement* activeArr = nullptr;
    if (usingTracks && !currentArrangements_.empty()) {
        if (activeArrangementId_.has_value()) {
            for (const auto& arr : currentArrangements_) {
                if (arr.id.has_value() && *arr.id == *activeArrangementId_) {
                    activeArr = &arr;
                    break;
                }
            }
        }
        if (!activeArr) activeArr = &currentArrangements_.front();
    }

    int targetTempo = activeArr ? activeArr->tempo : clickOnlyTempo_;
    std::string targetKey = activeArr ? (activeArr->key.empty() ? "C" : activeArr->key) : clickOnlyKey_;
    std::string targetSig = activeArr ? (activeArr->timeSignature.empty() ? "4/4" : activeArr->timeSignature) : clickOnlyTimeSig_;
    bool target8 = activeArr ? activeArr->clickEighths : clickOnlyEighths_;
    bool target16 = activeArr ? activeArr->clickSixteenths : clickOnlySixteenths_;
    float target8Gain = activeArr ? activeArr->clickEighthsGain : clickOnlyEighthsGain_;
    float target16Gain = activeArr ? activeArr->clickSixteenthsGain : clickOnlySixteenthsGain_;

    currentTempo_ = std::clamp(targetTempo > 0 ? targetTempo : 120, 20, 300);
    currentKey_ = targetKey.empty() ? "C" : targetKey;
    currentTimeSig_ = targetSig.empty() ? "4/4" : targetSig;

    tempoEditor_.setText(juce::String(currentTempo_), false);
    auto selectOrAdd = [](juce::ComboBox& box, const std::string& value) {
        juce::String target(value);
        for (int i = 0; i < box.getNumItems(); ++i) {
            if (box.getItemText(i) == target) {
                box.setSelectedItemIndex(i, juce::dontSendNotification);
                return;
            }
        }
        int newId = box.getNumItems() + 1;
        box.addItem(target, newId);
        box.setSelectedId(newId, juce::dontSendNotification);
    };
    selectOrAdd(keyBox_, currentKey_);
    selectOrAdd(timeSigBox_, currentTimeSig_);

    audioEngine_.setTempo(currentTempo_);
    audioEngine_.setTimeSignature(currentTimeSig_);

    eighthsToggle_.setToggleState(target8, juce::dontSendNotification);
    sixteenthsToggle_.setToggleState(target16, juce::dontSendNotification);
    eighthClickSlider_.setValue(linearToSliderDb(target8Gain), juce::dontSendNotification);
    sixteenthClickSlider_.setValue(linearToSliderDb(target16Gain), juce::dontSendNotification);
    audioEngine_.setClickSubdivisions(target8, target16);
    audioEngine_.setEighthClickGain(target8Gain);
    audioEngine_.setSixteenthClickGain(target16Gain);
    updateSubdivisionFaderVisibility();
}

void TransportComponent::syncArrangements(const std::vector<SongArrangement>& arrangements,
                                          std::optional<int64_t> activeArrangementId,
                                          int clickOnlyTempo,
                                          const std::string& clickOnlyKey,
                                          const std::string& clickOnlyTimeSig,
                                          bool clickOnlyEighths,
                                          bool clickOnlySixteenths,
                                          float clickOnlyEighthsGain,
                                          float clickOnlySixteenthsGain) {
    currentArrangements_ = arrangements;
    activeArrangementId_ = activeArrangementId;
    clickOnlyTempo_ = (clickOnlyTempo > 0) ? clickOnlyTempo : 120;
    clickOnlyKey_ = clickOnlyKey.empty() ? "C" : clickOnlyKey;
    clickOnlyTimeSig_ = clickOnlyTimeSig.empty() ? "4/4" : clickOnlyTimeSig;
    clickOnlyEighths_ = clickOnlyEighths;
    clickOnlySixteenths_ = clickOnlySixteenths;
    clickOnlyEighthsGain_ = clickOnlyEighthsGain;
    clickOnlySixteenthsGain_ = clickOnlySixteenthsGain;

    arrangementBox_.clear(juce::dontSendNotification);
    int selectedIdx = 0;
    for (size_t i = 0; i < currentArrangements_.size(); ++i) {
        const auto& arr = currentArrangements_[i];
        arrangementBox_.addItem(arr.name.empty() ? "Main" : arr.name, static_cast<int>(i + 1));
        if (activeArrangementId_.has_value() && arr.id.has_value() && *arr.id == *activeArrangementId_) {
            selectedIdx = static_cast<int>(i);
        }
    }
    if (!currentArrangements_.empty()) {
        arrangementBox_.setSelectedId(selectedIdx + 1, juce::dontSendNotification);
        if (!activeArrangementId_.has_value()) {
            activeArrangementId_ = currentArrangements_[static_cast<size_t>(selectedIdx)].id;
        }
    }

    applyActiveModeSettings();
    resized();
    repaint();
}

void TransportComponent::refreshAvailableChannels(int numChannels) {
    int count = std::max(1, numChannels);
    lastDeviceChannelCount_ = count;

    populateChannelCombo(clickChannelBox_, count);
    clickChannelBox_.setSelectedId(std::clamp(audioEngine_.getClickChannel() + 1, 1, count), juce::dontSendNotification);

    populateChannelCombo(guideChannelBox_, count);
    guideChannelBox_.setSelectedId(std::clamp(audioEngine_.getGuideChannel() + 1, 1, count), juce::dontSendNotification);

    for (size_t i = 0; i < trackStrips_.size() && i < currentTracks_.size(); ++i) {
        trackStrips_[i]->updateChannelOptions(count,
                                              currentTracks_[i].outputChannel,
                                              currentTracks_[i].stereoOutput);
    }
}

void TransportComponent::syncClickSubdivisions(std::optional<int64_t> songId,
                                               bool enableEighths,
                                               bool enableSixteenths,
                                               float eighthsGain,
                                               float sixteenthsGain) {
    lastSongId_ = songId;
    eighthsToggle_.setToggleState(enableEighths, juce::dontSendNotification);
    sixteenthsToggle_.setToggleState(enableSixteenths, juce::dontSendNotification);
    eighthClickSlider_.setValue(linearToSliderDb(eighthsGain), juce::dontSendNotification);
    sixteenthClickSlider_.setValue(linearToSliderDb(sixteenthsGain), juce::dontSendNotification);

    audioEngine_.setClickSubdivisions(enableEighths, enableSixteenths);
    audioEngine_.setEighthClickGain(eighthsGain);
    audioEngine_.setSixteenthClickGain(sixteenthsGain);

    updateSubdivisionFaderVisibility();
    resized();
    repaint();
}

void TransportComponent::notifyClickSubdivisionsChanged() {
    const bool e8 = eighthsToggle_.getToggleState();
    const bool e16 = sixteenthsToggle_.getToggleState();
    const float g8 = sliderDbToLinear(static_cast<float>(eighthClickSlider_.getValue()));
    const float g16 = sliderDbToLinear(static_cast<float>(sixteenthClickSlider_.getValue()));

    const bool usingTracks = useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
    std::optional<int64_t> targetArrId;
    if (usingTracks && activeArrangementId_.has_value()) {
        targetArrId = activeArrangementId_;
        for (auto& arr : currentArrangements_) {
            if (arr.id.has_value() && *arr.id == *targetArrId) {
                arr.clickEighths = e8;
                arr.clickSixteenths = e16;
                arr.clickEighthsGain = g8;
                arr.clickSixteenthsGain = g16;
                break;
            }
        }
    } else {
        clickOnlyEighths_ = e8;
        clickOnlySixteenths_ = e16;
        clickOnlyEighthsGain_ = g8;
        clickOnlySixteenthsGain_ = g16;
    }

    if (!lastSongId_.has_value() || !onSongClickSubdivisionsChanged) return;
    onSongClickSubdivisionsChanged(*lastSongId_, targetArrId, e8, e16, g8, g16);
}

void TransportComponent::updateSubdivisionFaderVisibility() {
    const bool showEighths = eighthsToggle_.getToggleState();
    const bool showSixteenths = sixteenthsToggle_.getToggleState();
    eighthClickSlider_.setVisible(showEighths);
    eighthClickLabel_.setVisible(showEighths);
    sixteenthClickSlider_.setVisible(showSixteenths);
    sixteenthClickLabel_.setVisible(showSixteenths);
}

void TransportComponent::syncClickAndGuideChannels(int clickChannel, int guideChannel) {
    int count = std::max(1, audioEngine_.getCurrentOutputChannels());
    if (count != lastDeviceChannelCount_) {
        refreshAvailableChannels(count);
    }
    clickChannelBox_.setSelectedId(std::clamp(clickChannel + 1, 1, count), juce::dontSendNotification);
    guideChannelBox_.setSelectedId(std::clamp(guideChannel + 1, 1, count), juce::dontSendNotification);
}

void TransportComponent::syncTracksAvailability(std::optional<int64_t> songId,
                                                const std::vector<SongTrack>& tracks,
                                                bool hasLoadedTracks,
                                                const std::vector<SongSection>& sections) {
    beatPlayhead_.setSections(sections);
    std::vector<std::string> newPaths;
    newPaths.reserve(tracks.size());
    for (const auto& t : tracks) {
        if (!t.filePath.empty()) newPaths.push_back(t.filePath);
    }

    const bool hasTracks = hasLoadedTracks || !newPaths.empty();
    const bool isNewSong = (!songId.has_value() || songId != lastSongId_);
    const bool trackListChanged = isNewSong || (newPaths != lastTrackPaths_);

    if (!hasTracks) {
        useTracksToggle_.setToggleState(false, juce::dontSendNotification);
        useTracksToggle_.setEnabled(false);
        audioEngine_.setUseTracks(false);
    } else {
        useTracksToggle_.setEnabled(true);
        if (isNewSong || !lastHadTracks_) {
            useTracksToggle_.setToggleState(true, juce::dontSendNotification);
        }
        audioEngine_.setUseTracks(useTracksToggle_.getToggleState());
    }

    lastSongId_ = songId;
    lastHadTracks_ = hasTracks;

    if (trackListChanged) {
        lastTrackPaths_ = std::move(newPaths);
        currentTracks_.clear();
        for (const auto& t : tracks) {
            if (!t.filePath.empty()) currentTracks_.push_back(t);
        }
        rebuildTrackStrips();
    } else {
        updateTrackStripsEnabled();
        resized();
        repaint();
    }

    lastWaveformVersion_ = audioEngine_.getWaveformVersion();
    beatPlayhead_.setWaveformData(audioEngine_.getWaveformPeaks());
}

void TransportComponent::rebuildTrackStrips() {
    trackStrips_.clear();
    trackStripsContainer_ = std::make_unique<juce::Component>();

    const int numDeviceChannels = std::max(1, audioEngine_.getCurrentOutputChannels());
    lastDeviceChannelCount_ = numDeviceChannels;

    auto shortLabels = computeShortTrackLabels(currentTracks_);
    for (size_t i = 0; i < currentTracks_.size(); ++i) {
        const auto& trk = currentTracks_[i];
        int trackIdx = static_cast<int>(i);
        bool isStereoTrack = (trk.numChannels != 1); // 2+ channels (or unknown legacy 0) can route as stereo pair
        auto strip = std::make_unique<TrackStripComponent>(
            shortLabels[i],
            juce::String(trk.name),
            trk.gain,
            trk.outputChannel,
            trk.stereoOutput,
            isStereoTrack,
            numDeviceChannels,
            [this, trackIdx](float linearGain) {
                audioEngine_.setTrackGain(trackIdx, linearGain);
                if (trackIdx >= 0 && trackIdx < static_cast<int>(currentTracks_.size())) {
                    auto& t = currentTracks_[static_cast<size_t>(trackIdx)];
                    t.gain = linearGain;
                    if (t.id.has_value() && onTrackMixChanged) {
                        onTrackMixChanged(*t.id, linearGain, t.outputChannel, t.stereoOutput);
                    }
                }
            },
            [this, trackIdx](int ch, bool stereoOut) {
                audioEngine_.setTrackOutputChannel(trackIdx, ch, stereoOut);
                if (trackIdx >= 0 && trackIdx < static_cast<int>(currentTracks_.size())) {
                    auto& t = currentTracks_[static_cast<size_t>(trackIdx)];
                    t.outputChannel = ch;
                    t.stereoOutput = stereoOut;
                    if (t.id.has_value() && onTrackMixChanged) {
                        onTrackMixChanged(*t.id, t.gain, ch, stereoOut);
                    }
                }
            });
        trackStripsContainer_->addAndMakeVisible(strip.get());
        trackStrips_.push_back(std::move(strip));
    }

    trackStripsViewport_.setViewedComponent(trackStripsContainer_.get(), false);
    updateTrackStripsEnabled();
    resized();
    repaint();
}

void TransportComponent::updateTrackStripsEnabled() {
    const bool enabled = useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
    for (auto& strip : trackStrips_) {
        strip->setEnabled(enabled);
        strip->repaint();
    }
}

void TransportComponent::layoutTrackStrips(int viewportWidth, int viewportHeight) {
    if (!trackStripsContainer_ || trackStrips_.empty()) return;

    const int gap = 6;
    const int count = static_cast<int>(trackStrips_.size());
    const int minStripW = 76;
    const int maxStripW = 114;

    int fittedW = (viewportWidth - gap * (count - 1)) / count;
    bool needsScroll = (fittedW < minStripW);
    int stripW = needsScroll ? minStripW : std::min(maxStripW, fittedW);
    int contentW = std::max(viewportWidth, count * stripW + gap * (count - 1));
    int stripH = needsScroll ? std::max(40, viewportHeight - trackStripsViewport_.getScrollBarThickness() - 2)
                             : viewportHeight;

    trackStripsContainer_->setSize(contentW, stripH);

    int x = 0;
    for (auto& strip : trackStrips_) {
        strip->setBounds(x, 0, stripW, stripH);
        x += stripW + gap;
    }
}

void TransportComponent::setPlayingState(bool isPlaying, bool isPaused) {
    if (!playPauseButton_) return;
    if (isPlaying && !isPaused) {
        playPauseButton_->setIconType(TransportIconButton::IconType::Pause);
        playPauseButton_->setBaseColour(AppTheme::warningAmber);
        playPauseButton_->setTooltip("Pause");
    } else {
        playPauseButton_->setIconType(TransportIconButton::IconType::Play);
        playPauseButton_->setBaseColour(AppTheme::primaryBlue);
        playPauseButton_->setTooltip("Play");
    }
}

void TransportComponent::setSongNavigationState(bool canGoPrev, bool canGoNext) {
    if (prevSongButton_) prevSongButton_->setEnabled(canGoPrev);
    if (nextSongButton_) nextSongButton_->setEnabled(canGoNext);
}

void TransportComponent::timerCallback() {
    int deviceChannels = std::max(1, audioEngine_.getCurrentOutputChannels());
    if (deviceChannels != lastDeviceChannelCount_) {
        refreshAvailableChannels(deviceChannels);
    }

    bool playing = audioEngine_.isPlaying();
    bool paused = audioEngine_.isPaused();
    setPlayingState(playing, paused);

    uint64_t wfVer = audioEngine_.getWaveformVersion();
    if (wfVer != lastWaveformVersion_) {
        lastWaveformVersion_ = wfVer;
        beatPlayhead_.setWaveformData(audioEngine_.getWaveformPeaks());
    }

    bool hasTracks = audioEngine_.hasLoadedTracks();
    bool showWaveform = hasTracks && useTracksToggle_.getToggleState();

    beatPlayhead_.updatePlayhead(audioEngine_.getTotalBarsElapsed(),
                                audioEngine_.getCurrentBeatInBar(),
                                audioEngine_.getCurrentBar(),
                                audioEngine_.getBeatsPerBar(),
                                playing,
                                paused,
                                audioEngine_.isCompoundMeter(),
                                showWaveform,
                                audioEngine_.getTrackDurationInBars(),
                                audioEngine_.getTrackBarCount(),
                                audioEngine_.isLoadingTracks(),
                                audioEngine_.hasLeadingCountSection());

    outputMeter_.updatePeaks(audioEngine_.getOutputPeaks());
}

void TransportComponent::setLayoutMode(LayoutMode mode) {
    if (layoutMode_ == mode) return;
    layoutMode_ = mode;

    const bool expanded = (layoutMode_ == LayoutMode::ExpandedPanel);

    songTitleLabel_.setFont(juce::Font(expanded ? 19.0f : 15.0f, juce::Font::bold));
    songArtistLabel_.setFont(juce::Font(expanded ? 13.0f : 12.0f, juce::Font::plain));

    auto configureFader = [expanded](juce::Slider& s, juce::Label& l) {
        if (expanded) {
            s.setSliderStyle(juce::Slider::LinearVertical);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow, true, 58, 20);
            l.setFont(juce::Font(12.0f, juce::Font::bold));
            l.setColour(juce::Label::textColourId, AppTheme::textLight);
            l.setJustificationType(juce::Justification::centred);
        } else {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            l.setFont(juce::Font(10.0f, juce::Font::plain));
            l.setColour(juce::Label::textColourId, AppTheme::textMuted);
            l.setJustificationType(juce::Justification::centredLeft);
        }
    };

    configureFader(masterSlider_, masterLabel_);
    configureFader(clickSlider_, clickLabel_);
    configureFader(eighthClickSlider_, eighthClickLabel_);
    configureFader(sixteenthClickSlider_, sixteenthClickLabel_);
    configureFader(guideSlider_, guideLabel_);

    eighthClickLabel_.setText(expanded ? "8th Click" : "8th", juce::dontSendNotification);
    sixteenthClickLabel_.setText(expanded ? "16th Click" : "16th", juce::dontSendNotification);

    clickChannelBox_.setVisible(expanded);
    guideChannelBox_.setVisible(expanded);
    const bool showTrackStrips = expanded && !trackStrips_.empty() && useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
    trackStripsViewport_.setVisible(showTrackStrips);
    updateSubdivisionFaderVisibility();

    resized();
    repaint();
}

void TransportComponent::paint(juce::Graphics& g) {
    if (layoutMode_ == LayoutMode::ExpandedPanel) {
        g.fillAll(AppTheme::bgDark);
        auto panelBounds = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(AppTheme::bgDark.brighter(0.03f));
        g.fillRoundedRectangle(panelBounds, 6.0f);
        g.setColour(AppTheme::borderCol);
        g.drawRoundedRectangle(panelBounds, 6.0f, 1.0f);

        for (const auto& strip : faderStripBounds_) {
            if (!strip.isEmpty()) {
                auto r = strip.toFloat().reduced(0.5f);
                g.setColour(AppTheme::bgPanel);
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(AppTheme::borderCol);
                g.drawRoundedRectangle(r, 4.0f, 1.0f);
            }
        }
        return;
    }

    g.fillAll(AppTheme::bgDark.brighter(0.04f));
    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
}

void TransportComponent::resized() {
    if (layoutMode_ == LayoutMode::ExpandedPanel) {
        auto area = getLocalBounds().reduced(16);

        // Top row: Full-width song title, artist, and editable BPM/Key/Time Signature + click/track toggles
        auto headerRow = area.removeFromTop(74);
        songTitleLabel_.setBounds(headerRow.removeFromTop(24));
        songArtistLabel_.setBounds(headerRow.removeFromTop(18));
        headerRow.removeFromTop(8);
        auto badgeRow = headerRow.removeFromTop(24);
        tempoEditor_.setBounds(badgeRow.removeFromLeft(48));
        badgeRow.removeFromLeft(4);
        bpmSuffixLabel_.setBounds(badgeRow.removeFromLeft(32));
        badgeRow.removeFromLeft(10);
        keyPrefixLabel_.setVisible(true);
        keyPrefixLabel_.setBounds(badgeRow.removeFromLeft(30));
        badgeRow.removeFromLeft(4);
        keyBox_.setBounds(badgeRow.removeFromLeft(70));
        badgeRow.removeFromLeft(10);
        timeSigBox_.setBounds(badgeRow.removeFromLeft(68));
        badgeRow.removeFromLeft(12);
        useTracksToggle_.setBounds(badgeRow.removeFromLeft(96));
        const bool showArrBox = useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState() && !currentArrangements_.empty();
        arrangementBox_.setVisible(showArrBox);
        if (showArrBox) {
            badgeRow.removeFromLeft(4);
            arrangementBox_.setBounds(badgeRow.removeFromLeft(140));
        }
        badgeRow.removeFromLeft(8);
        eighthsToggle_.setBounds(badgeRow.removeFromLeft(62));
        badgeRow.removeFromLeft(4);
        sixteenthsToggle_.setBounds(badgeRow.removeFromLeft(68));

        area.removeFromTop(12);

        // Transport controls row: Previous | Play/Pause | Stop | Next icon buttons
        auto transportRow = area.removeFromTop(36);
        prevSongButton_->setVisible(true);
        nextSongButton_->setVisible(true);
        const int btnGap = 10;
        const int iconBtnW = 52;
        const int playW = 68;
        const int totalBtnW = iconBtnW + btnGap + playW + btnGap + iconBtnW + btnGap + iconBtnW;
        auto btnGroup = transportRow.withSizeKeepingCentre(std::min(transportRow.getWidth(), totalBtnW), 36);
        prevSongButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        playPauseButton_->setBounds(btnGroup.removeFromLeft(playW));
        btnGroup.removeFromLeft(btnGap);
        stopButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        nextSongButton_->setBounds(btnGroup);

        area.removeFromTop(12);

        // Middle row: Full-width Beat Playhead / Summed Tracks Waveform
        int playheadH = std::clamp(area.getHeight() / 3, 96, 138);
        beatPlayhead_.setBounds(area.removeFromTop(playheadH));

        area.removeFromTop(14);

        // Bottom area: Shorter Vertical Mixer Faders (Master, Click, [8th Click], [16th Click], Guide + per-track strips) + Output Peak Meters
        const int maxMixerH = 260;
        if (area.getHeight() > maxMixerH) {
            area = area.removeFromTop(maxMixerH);
        }
        const bool hasTrackStrips = !trackStrips_.empty() && useTracksToggle_.isEnabled() && useTracksToggle_.getToggleState();
        const bool showEighths = eighthsToggle_.getToggleState();
        const bool showSixteenths = sixteenthsToggle_.getToggleState();
        clickChannelBox_.setVisible(true);
        guideChannelBox_.setVisible(true);
        trackStripsViewport_.setVisible(hasTrackStrips);
        updateSubdivisionFaderVisibility();

        int meterWidth = hasTrackStrips
                             ? std::clamp(area.getWidth() / 5, 116, 160)
                             : std::max(180, area.getWidth() / 2 - 8);
        auto meterArea = area.removeFromRight(meterWidth);
        outputMeter_.setBounds(meterArea);
        area.removeFromRight(12);

        auto mixerArea = area;
        const int stripGap = 6;

        auto layoutBaseStrip = [](juce::Rectangle<int> strip, juce::Label& lbl, juce::ComboBox* chBox, juce::Slider& slider) {
            auto inner = strip.reduced(4, 8);
            lbl.setBounds(inner.removeFromTop(18));
            inner.removeFromTop(4);
            auto chArea = inner.removeFromTop(22);
            if (chBox != nullptr) {
                chBox->setBounds(chArea);
            }
            inner.removeFromTop(6);
            slider.setBounds(inner);
        };

        struct FixedStripDef {
            juce::Label* label;
            juce::ComboBox* chBox;
            juce::Slider* slider;
        };
        std::vector<FixedStripDef> fixedStrips;
        fixedStrips.push_back({ &masterLabel_, nullptr, &masterSlider_ });
        fixedStrips.push_back({ &clickLabel_, &clickChannelBox_, &clickSlider_ });
        if (showEighths) {
            fixedStrips.push_back({ &eighthClickLabel_, nullptr, &eighthClickSlider_ });
        }
        if (showSixteenths) {
            fixedStrips.push_back({ &sixteenthClickLabel_, nullptr, &sixteenthClickSlider_ });
        }
        fixedStrips.push_back({ &guideLabel_, &guideChannelBox_, &guideSlider_ });

        const int fixedCount = static_cast<int>(fixedStrips.size());
        faderStripBounds_.clear();
        faderStripBounds_.reserve(fixedStrips.size());

        if (!hasTrackStrips) {
            int stripW = (mixerArea.getWidth() - stripGap * (fixedCount - 1)) / fixedCount;
            for (int i = 0; i < fixedCount; ++i) {
                auto rect = (i == fixedCount - 1) ? mixerArea : mixerArea.removeFromLeft(stripW);
                if (i < fixedCount - 1) mixerArea.removeFromLeft(stripGap);
                faderStripBounds_.push_back(rect);
                layoutBaseStrip(rect, *fixedStrips[static_cast<size_t>(i)].label,
                                fixedStrips[static_cast<size_t>(i)].chBox,
                                *fixedStrips[static_cast<size_t>(i)].slider);
            }
        } else {
            const int totalStrips = fixedCount + static_cast<int>(trackStrips_.size());
            const int evenW = (mixerArea.getWidth() - stripGap * (totalStrips - 1)) / totalStrips;
            const int baseStripW = std::clamp(evenW, 66, 94);

            for (int i = 0; i < fixedCount; ++i) {
                auto rect = mixerArea.removeFromLeft(baseStripW);
                mixerArea.removeFromLeft(stripGap);
                faderStripBounds_.push_back(rect);
                layoutBaseStrip(rect, *fixedStrips[static_cast<size_t>(i)].label,
                                fixedStrips[static_cast<size_t>(i)].chBox,
                                *fixedStrips[static_cast<size_t>(i)].slider);
            }

            trackStripsViewport_.setBounds(mixerArea);
            layoutTrackStrips(mixerArea.getWidth(), mixerArea.getHeight());
        }
        return;
    }

    faderStripBounds_.clear();
    prevSongButton_->setVisible(false);
    nextSongButton_->setVisible(false);
    arrangementBox_.setVisible(false);
    clickChannelBox_.setVisible(false);
    guideChannelBox_.setVisible(false);
    trackStripsViewport_.setVisible(false);
    updateSubdivisionFaderVisibility();

    auto area = getLocalBounds().reduced(12, 8);

    // Left controls: Play/Pause and Stop icon buttons side-by-side
    auto btnArea = area.removeFromLeft(116).withSizeKeepingCentre(116, 38);
    playPauseButton_->setBounds(btnArea.removeFromLeft(60));
    btnArea.removeFromLeft(8);
    stopButton_->setBounds(btnArea);

    area.removeFromLeft(12);

    // Song info column
    auto infoArea = area.removeFromLeft(392);
    songTitleLabel_.setBounds(infoArea.removeFromTop(20));
    songArtistLabel_.setBounds(infoArea.removeFromTop(16));
    infoArea.removeFromTop(6);
    auto badgeRow = infoArea.removeFromTop(22);
    keyPrefixLabel_.setVisible(false);
    tempoEditor_.setBounds(badgeRow.removeFromLeft(38));
    badgeRow.removeFromLeft(2);
    bpmSuffixLabel_.setBounds(badgeRow.removeFromLeft(28));
    badgeRow.removeFromLeft(4);
    keyBox_.setBounds(badgeRow.removeFromLeft(54));
    badgeRow.removeFromLeft(4);
    timeSigBox_.setBounds(badgeRow.removeFromLeft(54));
    badgeRow.removeFromLeft(6);
    useTracksToggle_.setBounds(badgeRow.removeFromLeft(88));
    badgeRow.removeFromLeft(2);
    eighthsToggle_.setBounds(badgeRow.removeFromLeft(54));
    badgeRow.removeFromLeft(2);
    sixteenthsToggle_.setBounds(badgeRow.removeFromLeft(56));

    area.removeFromLeft(12);

    // Center: Beat playhead
    auto playheadArea = area.removeFromLeft(140);
    beatPlayhead_.setBounds(playheadArea.withSizeKeepingCentre(134, 60));

    area.removeFromLeft(12);

    // Right: Output Peak Meter
    auto meterArea = area.removeFromRight(136);
    outputMeter_.setBounds(meterArea);

    area.removeFromRight(12);

    // Volume sliders
    auto slidersArea = area;
    int sliderH = 18;
    int gap = 4;

    auto row1 = slidersArea.removeFromTop(sliderH);
    masterLabel_.setBounds(row1.removeFromLeft(45));
    masterSlider_.setBounds(row1);

    slidersArea.removeFromTop(gap);
    auto row2 = slidersArea.removeFromTop(sliderH);
    const bool showEighths = eighthsToggle_.getToggleState();
    const bool showSixteenths = sixteenthsToggle_.getToggleState();
    const int clickCols = 1 + (showEighths ? 1 : 0) + (showSixteenths ? 1 : 0);
    if (clickCols == 1) {
        clickLabel_.setBounds(row2.removeFromLeft(45));
        clickSlider_.setBounds(row2);
    } else {
        int colW = (row2.getWidth() - 6 * (clickCols - 1)) / clickCols;
        auto c0 = row2.removeFromLeft(colW);
        clickLabel_.setBounds(c0.removeFromLeft(34));
        clickSlider_.setBounds(c0);
        if (showEighths) {
            row2.removeFromLeft(6);
            auto c1 = showSixteenths ? row2.removeFromLeft(colW) : row2;
            eighthClickLabel_.setBounds(c1.removeFromLeft(26));
            eighthClickSlider_.setBounds(c1);
        }
        if (showSixteenths) {
            row2.removeFromLeft(6);
            sixteenthClickLabel_.setBounds(row2.removeFromLeft(30));
            sixteenthClickSlider_.setBounds(row2);
        }
    }

    slidersArea.removeFromTop(gap);
    auto row3 = slidersArea.removeFromTop(sliderH);
    guideLabel_.setBounds(row3.removeFromLeft(45));
    guideSlider_.setBounds(row3);
}

} // namespace tracks
