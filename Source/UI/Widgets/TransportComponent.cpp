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
constexpr int kJustClickComboId = 100000;

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
        fader_.setScrollWheelEnabled(false);
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
        inner.removeFromTop(6);
        channelBox_.setBounds(inner.removeFromBottom(22));
        inner.removeFromBottom(6);
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
    enum class IconType { Previous, SkipBack, Play, Pause, Stop, FadeOut, Repeat, SkipForward, Next };

    TransportIconButton(const juce::String& name, IconType type, juce::Colour baseColour)
        : juce::Button(name), iconType_(type), baseColour_(baseColour) {
        setTooltip(name);
        setWantsKeyboardFocus(false);
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

    void setActiveHighlight(bool active) {
        if (activeHighlight_ != active) {
            activeHighlight_ = active;
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

        if (enabled && activeHighlight_) {
            g.setColour(juce::Colour(0xff38bdf8));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.6f);
        } else {
            g.setColour(enabled ? AppTheme::borderCol.brighter(0.1f) : AppTheme::borderCol.withAlpha(0.45f));
            g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
        }

        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float iconH = std::min(14.0f, bounds.getHeight() * 0.44f);
        const float halfH = iconH * 0.5f;

        g.setColour(enabled ? (activeHighlight_ ? juce::Colours::white : AppTheme::textLight)
                            : AppTheme::textMuted.withAlpha(0.45f));

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
            case IconType::FadeOut: {
                // Descending 4-bar fade-out ramp icon (tall left -> short right)
                const float totalW = iconH * 1.15f;
                const float gap = 2.0f;
                const float barW = (totalW - gap * 3.0f) * 0.25f;
                const float startX = cx - totalW * 0.5f;
                const float bottomY = cy + halfH;
                const float heights[4] = { iconH, iconH * 0.72f, iconH * 0.45f, iconH * 0.20f };
                for (int i = 0; i < 4; ++i) {
                    const float h = std::max(2.0f, heights[i]);
                    g.fillRoundedRectangle(startX + static_cast<float>(i) * (barW + gap),
                                           bottomY - h,
                                           barW,
                                           h,
                                           1.0f);
                }
                break;
            }
            case IconType::Repeat: {
                // Classic rounded loop icon: top arrow pointing right, bottom arrow pointing left
                const float halfW = iconH * 0.62f;
                const float yTop = cy - halfH * 0.62f;
                const float yBot = cy + halfH * 0.62f;
                const float xLeft = cx - halfW;
                const float xRight = cx + halfW;
                const float r = 3.0f;
                const float ah = 3.4f;

                juce::Path topArc;
                topArc.startNewSubPath(xLeft, cy - 0.5f);
                topArc.lineTo(xLeft, yTop + r);
                topArc.quadraticTo(xLeft, yTop, xLeft + r, yTop);
                topArc.lineTo(xRight - ah * 0.6f, yTop);
                g.strokePath(topArc, juce::PathStrokeType(1.85f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                juce::Path topHead;
                topHead.addTriangle(xRight - ah, yTop - ah * 0.8f,
                                    xRight - ah, yTop + ah * 0.8f,
                                    xRight + 1.0f, yTop);
                g.fillPath(topHead);

                juce::Path botArc;
                botArc.startNewSubPath(xRight, cy + 0.5f);
                botArc.lineTo(xRight, yBot - r);
                botArc.quadraticTo(xRight, yBot, xRight - r, yBot);
                botArc.lineTo(xLeft + ah * 0.6f, yBot);
                g.strokePath(botArc, juce::PathStrokeType(1.85f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                juce::Path botHead;
                botHead.addTriangle(xLeft + ah, yBot - ah * 0.8f,
                                    xLeft + ah, yBot + ah * 0.8f,
                                    xLeft - 1.0f, yBot);
                g.fillPath(botHead);
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
            case IconType::SkipBack: {
                const float triW = iconH * 0.62f;
                const float overlap = 1.0f;
                const float totalW = triW * 2.0f - overlap;
                const float leftX = cx - totalW * 0.5f;
                juce::Path p1;
                p1.addTriangle(leftX, cy, leftX + triW, cy - halfH, leftX + triW, cy + halfH);
                g.fillPath(p1.createPathWithRoundedCorners(1.0f));
                const float secondX = leftX + triW - overlap;
                juce::Path p2;
                p2.addTriangle(secondX, cy, secondX + triW, cy - halfH, secondX + triW, cy + halfH);
                g.fillPath(p2.createPathWithRoundedCorners(1.0f));
                break;
            }
            case IconType::SkipForward: {
                const float triW = iconH * 0.62f;
                const float overlap = 1.0f;
                const float totalW = triW * 2.0f - overlap;
                const float leftX = cx - totalW * 0.5f;
                juce::Path p1;
                p1.addTriangle(leftX, cy - halfH, leftX, cy + halfH, leftX + triW, cy);
                g.fillPath(p1.createPathWithRoundedCorners(1.0f));
                const float secondX = leftX + triW - overlap;
                juce::Path p2;
                p2.addTriangle(secondX, cy - halfH, secondX, cy + halfH, secondX + triW, cy);
                g.fillPath(p2.createPathWithRoundedCorners(1.0f));
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
    bool activeHighlight_ = false;
};

class TransportComponent::CrossfadeBarsStepper : public juce::Component,
                                                 public juce::SettableTooltipClient {
public:
    std::function<void(int)> onValueChanged;

    CrossfadeBarsStepper() {
        setRepaintsOnMouseActivity(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setValue(int v) {
        int clamped = clampCrossfadeBars(v);
        if (value_ != clamped) {
            value_ = clamped;
            repaint();
        }
    }

    int getValue() const { return value_; }

    void paint(juce::Graphics& g) override {
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(AppTheme::bgInput);
        g.fillRoundedRectangle(bounds, 4.0f);

        const float arrowColW = 16.0f;
        const float splitX = bounds.getRight() - arrowColW;
        const float midY = bounds.getCentreY();

        // Arrow half hover / press fills
        juce::Rectangle<float> upRect(splitX, bounds.getY(), arrowColW, midY - bounds.getY());
        juce::Rectangle<float> downRect(splitX, midY, arrowColW, bounds.getBottom() - midY);

        if (hoverZone_ == 1 && value_ < 4) {
            g.setColour((pressedZone_ == 1 ? AppTheme::primaryBlue : AppTheme::bgPanel.brighter(0.22f)).withAlpha(0.85f));
            g.fillRect(upRect.reduced(0.5f));
        } else if (hoverZone_ == -1 && value_ > 1) {
            g.setColour((pressedZone_ == -1 ? AppTheme::primaryBlue : AppTheme::bgPanel.brighter(0.22f)).withAlpha(0.85f));
            g.fillRect(downRect.reduced(0.5f));
        }

        // Dividers & outer border
        g.setColour(AppTheme::borderCol);
        g.drawVerticalLine(static_cast<int>(std::round(splitX)), bounds.getY(), bounds.getBottom());
        g.drawHorizontalLine(static_cast<int>(std::round(midY)), splitX, bounds.getRight());
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

        // Numeric value
        juce::Rectangle<float> valRect(bounds.getX(), bounds.getY(), splitX - bounds.getX(), bounds.getHeight());
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.setColour(AppTheme::textLight);
        g.drawText(juce::String(value_), valRect, juce::Justification::centred, false);

        // Tiny crisp up/down triangles
        const float triW = 3.2f;
        const float triH = 2.4f;
        {
            const float cx = upRect.getCentreX();
            const float cy = upRect.getCentreY() + 0.5f;
            juce::Path upTri;
            upTri.addTriangle(cx, cy - triH, cx - triW, cy + triH * 0.8f, cx + triW, cy + triH * 0.8f);
            g.setColour(value_ < 4 ? AppTheme::textLight : AppTheme::textMuted.withAlpha(0.32f));
            g.fillPath(upTri);
        }
        {
            const float cx = downRect.getCentreX();
            const float cy = downRect.getCentreY() - 0.5f;
            juce::Path downTri;
            downTri.addTriangle(cx, cy + triH, cx - triW, cy - triH * 0.8f, cx + triW, cy - triH * 0.8f);
            g.setColour(value_ > 1 ? AppTheme::textLight : AppTheme::textMuted.withAlpha(0.32f));
            g.fillPath(downTri);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override {
        updateHoverZone(e.position);
    }

    void mouseExit(const juce::MouseEvent&) override {
        hoverZone_ = 0;
        pressedZone_ = 0;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override {
        updateHoverZone(e.position);
        const int zone = (e.position.y < getHeight() * 0.5f) ? 1 : -1;
        pressedZone_ = zone;
        stepBy(zone);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override {
        pressedZone_ = 0;
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override {
        if (wheel.deltaY > 0.0f) stepBy(1);
        else if (wheel.deltaY < 0.0f) stepBy(-1);
    }

private:
    int value_ = 1;
    int hoverZone_ = 0;   // +1 = up, -1 = down, 0 = none
    int pressedZone_ = 0;

    void updateHoverZone(juce::Point<float> pos) {
        const float splitX = static_cast<float>(getWidth()) - 16.5f;
        int nextZone = 0;
        if (pos.x >= splitX) {
            nextZone = (pos.y < static_cast<float>(getHeight()) * 0.5f) ? 1 : -1;
        }
        if (nextZone != hoverZone_) {
            hoverZone_ = nextZone;
            repaint();
        }
    }

    void stepBy(int delta) {
        int next = clampCrossfadeBars(value_ + delta);
        if (next != value_) {
            value_ = next;
            repaint();
            if (onValueChanged) onValueChanged(value_);
        }
    }
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

    // Skip back section button (queues previous section for when current section finishes)
    skipBackButton_ = std::make_unique<TransportIconButton>("Skip Back Section", TransportIconButton::IconType::SkipBack, AppTheme::bgPanel);
    skipBackButton_->setEnabled(false);
    skipBackButton_->onClick = [this]() {
        audioEngine_.skipSection(-1);
    };
    addAndMakeVisible(skipBackButton_.get());

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

    // Fade-Out Stop button (smooth 4-second raised-cosine fade-out; click again for immediate stop)
    fadeOutButton_ = std::make_unique<TransportIconButton>("Fade Out & Stop (4s)", TransportIconButton::IconType::FadeOut, AppTheme::bgPanel);
    fadeOutButton_->setEnabled(false);
    fadeOutButton_->onClick = [this]() {
        audioEngine_.fadeOutAndStop(4.0);
    };
    addAndMakeVisible(fadeOutButton_.get());

    // Repeat current section button (with tracks) / Click Loop toggle (without tracks)
    repeatSectionButton_ = std::make_unique<TransportIconButton>("Repeat / Loop", TransportIconButton::IconType::Repeat, AppTheme::bgPanel);
    repeatSectionButton_->setEnabled(false);
    repeatSectionButton_->onClick = [this]() {
        if (audioEngine_.isStructuredTimelineMode()) {
            audioEngine_.toggleRepeatCurrentSection();
        } else {
            audioEngine_.toggleClickLoop();
        }
    };
    addAndMakeVisible(repeatSectionButton_.get());

    // Skip forward section button (queues next section for when current section finishes)
    skipForwardButton_ = std::make_unique<TransportIconButton>("Skip Forward Section", TransportIconButton::IconType::SkipForward, AppTheme::bgPanel);
    skipForwardButton_->setEnabled(false);
    skipForwardButton_->onClick = [this]() {
        audioEngine_.skipSection(1);
    };
    addAndMakeVisible(skipForwardButton_.get());

    // Next song button
    nextSongButton_ = std::make_unique<TransportIconButton>("Next Song", TransportIconButton::IconType::Next, AppTheme::bgPanel);
    nextSongButton_->setEnabled(false);
    nextSongButton_->onClick = [this]() {
        if (onNextSongClicked) onNextSongClicked();
    };
    addChildComponent(nextSongButton_.get());

    // Song end transition dropdown (shown to the right of play controls in ExpandedPanel / Open Set view)
    transitionBox_.addItem("Stay in current song", 1);
    transitionBox_.addItem("Cue next song", 2);
    transitionBox_.addItem("Autoplay next song", 3);
    transitionBox_.addItem("Crossfade", 4);
    transitionBox_.setSelectedId(1, juce::dontSendNotification);
    transitionBox_.setTooltip("Song end transition");
    transitionBox_.onChange = [this]() {
        int id = transitionBox_.getSelectedId();
        if (id >= 1 && id <= 4) {
            auto mode = clampTransitionMode(id - 1);
            resized();
            repaint();
            if (onTransitionChanged) {
                onTransitionChanged(mode, currentCrossfadeBars_);
            }
        }
    };
    addChildComponent(transitionBox_);

    crossfadeBarsStepper_ = std::make_unique<CrossfadeBarsStepper>();
    crossfadeBarsStepper_->setValue(1);
    crossfadeBarsStepper_->setTooltip("Bars to crossfade (1-4)");
    crossfadeBarsStepper_->onValueChanged = [this](int bars) {
        currentCrossfadeBars_ = clampCrossfadeBars(bars);
        crossfadeBarsLabel_.setText(currentCrossfadeBars_ == 1 ? "bar" : "bars", juce::dontSendNotification);
        if (onTransitionChanged && transitionBox_.getSelectedId() == 4) {
            onTransitionChanged(SongTransitionMode::Crossfade, currentCrossfadeBars_);
        }
    };
    addChildComponent(crossfadeBarsStepper_.get());

    crossfadeBarsLabel_.setFont(juce::Font(11.0f, juce::Font::bold));
    crossfadeBarsLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    crossfadeBarsLabel_.setJustificationType(juce::Justification::centredLeft);
    addChildComponent(crossfadeBarsLabel_);

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
    tempoDownButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    tempoDownButton_.setTooltip("Decrease tempo by 1 BPM");
    tempoDownButton_.onClick = [this]() { nudgeTempo(-1); };
    addChildComponent(tempoDownButton_);

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

    tempoUpButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    tempoUpButton_.setTooltip("Increase tempo by 1 BPM");
    tempoUpButton_.onClick = [this]() { nudgeTempo(1); };
    addChildComponent(tempoUpButton_);

    bpmSuffixLabel_.setFont(juce::Font(11.0f, juce::Font::bold));
    bpmSuffixLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    bpmSuffixLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(bpmSuffixLabel_);

    keyPrefixLabel_.setFont(juce::Font(11.0f, juce::Font::bold));
    keyPrefixLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    keyPrefixLabel_.setJustificationType(juce::Justification::centredRight);
    addChildComponent(keyPrefixLabel_);

    populateKeyBoxOptions(false, "");
    keyBox_.setSelectedId(1, juce::dontSendNotification);
    keyBox_.onChange = [this]() {
        std::string newKey = keyBox_.getText().toStdString();
        if (!newKey.empty() && newKey != currentKey_) {
            currentKey_ = newKey;
            if (isUsingTracks()) {
                arrangementTargetKey_ = newKey;
                applyCurrentKeyPitchShift();
            }
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

    audioEngine_.setUseTracks(false);
    arrangementBox_.setTextWhenNothingSelected("Just click");
    arrangementBox_.setTextWhenNoChoicesAvailable("Just click");
    arrangementBox_.addItem("Just click", kJustClickComboId);
    arrangementBox_.setSelectedId(kJustClickComboId, juce::dontSendNotification);
    arrangementBox_.onChange = [this]() {
        const int selId = arrangementBox_.getSelectedId();
        if (selId == kJustClickComboId) {
            justClickSelected_ = true;
            activeArrangementId_ = std::nullopt;
            audioEngine_.setUseTracks(false);
            if (onArrangementChanged) {
                onArrangementChanged(std::nullopt);
            } else {
                applyActiveModeSettings();
                updateTrackStripsEnabled();
                resized();
                repaint();
            }
            return;
        }
        const int idx = selId - 1;
        if (idx >= 0 && idx < static_cast<int>(currentArrangements_.size())) {
            justClickSelected_ = false;
            const auto& arr = currentArrangements_[static_cast<size_t>(idx)];
            activeArrangementId_ = arr.id;
            audioEngine_.setUseTracks(!arr.tracks.empty());
            if (arr.id.has_value() && onArrangementChanged) {
                onArrangementChanged(arr.id);
            } else {
                applyActiveModeSettings();
                updateTrackStripsEnabled();
                resized();
                repaint();
            }
        }
    };
    addAndMakeVisible(arrangementBox_);

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
        s.setScrollWheelEnabled(false);
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

    beatPlayhead_.onSectionClicked = [this](int sectionIdx) {
        audioEngine_.selectOrQueueSection(sectionIdx);
    };
    addAndMakeVisible(beatPlayhead_);
    addAndMakeVisible(outputMeter_);

    currentTimerHz_ = 10;
    startTimerHz(currentTimerHz_); // Adaptive timer: 30 Hz during playback/metering, 5 Hz when idle
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
    songArtistLabel_.setText(artist.empty() ? "-" : artist, juce::dontSendNotification);

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

int TransportComponent::getActiveOriginalTrackTempo() const {
    if (!currentArrangements_.empty()) {
        const SongArrangement* activeArr = nullptr;
        if (activeArrangementId_.has_value()) {
            for (const auto& arr : currentArrangements_) {
                if (arr.id.has_value() && *arr.id == *activeArrangementId_) {
                    activeArr = &arr;
                    break;
                }
            }
        }
        if (!activeArr) activeArr = &currentArrangements_.front();
        if (activeArr && activeArr->tempo > 0) {
            return std::clamp(activeArr->tempo, 20, 300);
        }
    }
    return std::clamp(clickOnlyTempo_ > 0 ? clickOnlyTempo_ : 120, 20, 300);
}

bool TransportComponent::isUsingTracks() const {
    if (justClickSelected_) return false;
    return !currentTracks_.empty() || audioEngine_.hasLoadedTracks();
}

std::pair<int, int> TransportComponent::getCurrentAllowedTempoRange() const {
    if (isUsingTracks()) {
        return getAllowedTrackTempoRange(getActiveOriginalTrackTempo());
    }
    return { 20, 300 };
}

void TransportComponent::updateTempoControlsUi() {
    const bool usingTracks = isUsingTracks();
    const auto [minBpm, maxBpm] = getCurrentAllowedTempoRange();
    tempoDownButton_.setEnabled(currentTempo_ > minBpm);
    tempoUpButton_.setEnabled(currentTempo_ < maxBpm);

    if (usingTracks) {
        const int origTempo = getActiveOriginalTrackTempo();
        const int delta = currentTempo_ - origTempo;
        const double pct = (origTempo > 0) ? (100.0 * static_cast<double>(currentTempo_) / static_cast<double>(origTempo)) : 100.0;
        juce::String rangeStr = " (allowed: " + juce::String(minBpm) + "-" + juce::String(maxBpm) + " BPM)";
        if (delta != 0) {
            juce::String sign = (delta > 0) ? "+" : "";
            tempoEditor_.setTooltip("Time-stretched " + sign + juce::String(delta) + " BPM (" +
                                    juce::String(pct, 1) + "%) from " + juce::String(origTempo) + " BPM" + rangeStr);
        } else {
            tempoEditor_.setTooltip("Original arrangement tempo: " + juce::String(origTempo) + " BPM" + rangeStr);
        }
        tempoDownButton_.setTooltip("Decrease tempo by 1 BPM (min " + juce::String(minBpm) + " BPM)");
        tempoUpButton_.setTooltip("Increase tempo by 1 BPM (max " + juce::String(maxBpm) + " BPM)");
    } else {
        tempoEditor_.setTooltip("Song Tempo (20-300 BPM)");
        tempoDownButton_.setTooltip("Decrease tempo by 1 BPM");
        tempoUpButton_.setTooltip("Increase tempo by 1 BPM");
    }
}

void TransportComponent::nudgeTempo(int delta) {
    const auto [minBpm, maxBpm] = getCurrentAllowedTempoRange();
    int nextTempo = std::clamp(currentTempo_ + delta, minBpm, maxBpm);
    if (nextTempo == currentTempo_) return;

    currentTempo_ = nextTempo;
    if (isUsingTracks()) {
        arrangementTargetTempo_ = currentTempo_;
    }
    tempoEditor_.setText(juce::String(currentTempo_), false);
    audioEngine_.setTempo(currentTempo_);
    updateTempoControlsUi();
    notifySongParamsChanged();
}

void TransportComponent::commitTempoEditor(bool clampAndFormat) {
    auto text = tempoEditor_.getText().trim();
    int parsed = text.getIntValue();
    const auto [minBpm, maxBpm] = getCurrentAllowedTempoRange();
    const bool usingTracks = isUsingTracks();

    if (clampAndFormat) {
        if (text.isEmpty() || parsed <= 0) {
            tempoEditor_.setText(juce::String(currentTempo_), false);
            return;
        }
        int clamped = std::clamp(parsed, minBpm, maxBpm);
        tempoEditor_.setText(juce::String(clamped), false);
        if (clamped != currentTempo_) {
            currentTempo_ = clamped;
            if (usingTracks) {
                arrangementTargetTempo_ = currentTempo_;
            }
            audioEngine_.setTempo(currentTempo_);
            updateTempoControlsUi();
            notifySongParamsChanged();
        }
    } else if (parsed >= minBpm && parsed <= maxBpm && parsed != currentTempo_) {
        currentTempo_ = parsed;
        if (usingTracks) {
            arrangementTargetTempo_ = currentTempo_;
        }
        audioEngine_.setTempo(currentTempo_);
        updateTempoControlsUi();
        notifySongParamsChanged();
    }
}

void TransportComponent::notifySongParamsChanged() {
    if (!isUsingTracks()) {
        clickOnlyTempo_ = currentTempo_;
        clickOnlyKey_ = currentKey_;
        clickOnlyTimeSig_ = currentTimeSig_;
    }
    if (onSongParamsChanged) {
        onSongParamsChanged(lastSongId_, currentTempo_, currentKey_, currentTimeSig_);
    }
}

void TransportComponent::updateSongParamEditorsEnabled() {
    const bool usingTracks = isUsingTracks();
    tempoEditor_.setEnabled(true); // Tempo can be varied live (limited to +/-10 BPM when tracks are enabled)
    keyBox_.setEnabled(true);      // Key can be transposed live via real-time pitch shifting (Drums & FX excluded)
    timeSigBox_.setEnabled(!usingTracks);
    updateTempoControlsUi();
}

void TransportComponent::applyCurrentKeyPitchShift() {
    const bool usingTracks = isUsingTracks();
    if (!usingTracks) {
        audioEngine_.setPitchSemitones(0);
        keyBox_.setTooltip("Song Key (click-only mode)");
        return;
    }

    std::string sourceKey;
    if (!currentArrangements_.empty()) {
        const SongArrangement* activeArr = nullptr;
        if (activeArrangementId_.has_value()) {
            for (const auto& arr : currentArrangements_) {
                if (arr.id.has_value() && *arr.id == *activeArrangementId_) {
                    activeArr = &arr;
                    break;
                }
            }
        }
        if (!activeArr) activeArr = &currentArrangements_.front();
        sourceKey = activeArr->key;
    }
    if (sourceKey.empty() && !currentTracks_.empty()) {
        sourceKey = currentTracks_[0].trackKey;
    }

    int semitones = sourceKey.empty() ? 0 : computeSemitoneShift(sourceKey, currentKey_);
    audioEngine_.setPitchSemitones(semitones);

    if (semitones != 0 && !sourceKey.empty()) {
        juce::String sign = (semitones > 0) ? "+" : "";
        keyBox_.setTooltip("Pitch shifted " + sign + juce::String(semitones) +
                           " semitones from " + juce::String(sourceKey) + " (Drums & FX excluded)");
    } else if (!sourceKey.empty()) {
        keyBox_.setTooltip("Original arrangement key: " + juce::String(sourceKey));
    } else {
        keyBox_.setTooltip("Song Key");
    }
}

void TransportComponent::populateKeyBoxOptions(bool filterByTrackKey, const std::string& trackSourceKey) {
    static const char* kMajorKeys[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    static const char* kMinorKeys[] = { "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm" };

    keyBox_.clear(juce::dontSendNotification);
    int kid = 1;

    if (filterByTrackKey && !trackSourceKey.empty()) {
        if (isMinorKey(trackSourceKey)) {
            for (const char* k : kMinorKeys) {
                keyBox_.addItem(k, kid++);
            }
        } else {
            for (const char* k : kMajorKeys) {
                keyBox_.addItem(k, kid++);
            }
        }
    } else {
        for (const char* k : kMajorKeys) {
            keyBox_.addItem(k, kid++);
        }
        for (const char* k : kMinorKeys) {
            keyBox_.addItem(k, kid++);
        }
    }
}

void TransportComponent::applyActiveModeSettings() {
    const bool usingTracks = isUsingTracks();
    updateSongParamEditorsEnabled();

    const SongArrangement* selectedArr = nullptr;
    if (!justClickSelected_ && !currentArrangements_.empty()) {
        if (activeArrangementId_.has_value()) {
            for (const auto& arr : currentArrangements_) {
                if (arr.id.has_value() && *arr.id == *activeArrangementId_) {
                    selectedArr = &arr;
                    break;
                }
            }
        }
        if (!selectedArr) selectedArr = &currentArrangements_.front();
    }
    const bool isTracklessArrangement = (selectedArr != nullptr && selectedArr->tracks.empty());
    const SongArrangement* activeArr = (usingTracks || isTracklessArrangement) ? selectedArr : nullptr;

    std::string trackSourceKey;
    if (usingTracks) {
        if (activeArr && !activeArr->key.empty()) {
            trackSourceKey = activeArr->key;
        } else if (!currentTracks_.empty() && !currentTracks_[0].trackKey.empty()) {
            trackSourceKey = currentTracks_[0].trackKey;
        }
    }

    populateKeyBoxOptions(usingTracks, trackSourceKey);

    int targetTempo = clickOnlyTempo_;
    if (usingTracks) {
        const int origTempo = getActiveOriginalTrackTempo();
        audioEngine_.setOriginalTrackTempo(origTempo);
        targetTempo = (arrangementTargetTempo_ > 0)
                          ? clampTrackTempoForUi(arrangementTargetTempo_, origTempo)
                          : origTempo;
        arrangementTargetTempo_ = targetTempo;
    } else if (isTracklessArrangement) {
        const int origTempo = (activeArr->tempo > 0) ? activeArr->tempo : clickOnlyTempo_;
        audioEngine_.setOriginalTrackTempo(origTempo);
        targetTempo = (arrangementTargetTempo_ > 0)
                          ? std::clamp(arrangementTargetTempo_, 20, 300)
                          : origTempo;
        arrangementTargetTempo_ = targetTempo;
    } else {
        audioEngine_.setOriginalTrackTempo(clickOnlyTempo_);
    }

    std::string targetKey = activeArr
                                ? (!arrangementTargetKey_.empty() ? arrangementTargetKey_
                                                                  : (activeArr->key.empty() ? "C" : activeArr->key))
                                : clickOnlyKey_;
    if (usingTracks && !trackSourceKey.empty() && !targetKey.empty() &&
        isMinorKey(targetKey) != isMinorKey(trackSourceKey)) {
        targetKey = trackSourceKey;
        arrangementTargetKey_ = trackSourceKey;
    }
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
    applyCurrentKeyPitchShift();
    updateTempoControlsUi();

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
                                          bool justClickMode,
                                          int clickOnlyTempo,
                                          const std::string& clickOnlyKey,
                                          const std::string& clickOnlyTimeSig,
                                          bool clickOnlyEighths,
                                          bool clickOnlySixteenths,
                                          float clickOnlyEighthsGain,
                                          float clickOnlySixteenthsGain) {
    currentArrangements_ = arrangements;
    activeArrangementId_ = activeArrangementId;
    arrangementTargetTempo_ = currentTempo_; // Preserves any setlist-overridden tempo passed via setSongInfo()
    arrangementTargetKey_ = currentKey_;     // Preserves any setlist-overridden transposed key passed via setSongInfo()
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
        arrangementBox_.addSeparator();
    }
    arrangementBox_.addItem("Just click", kJustClickComboId);

    if (justClickMode || currentArrangements_.empty()) {
        justClickSelected_ = true;
        activeArrangementId_ = std::nullopt;
        arrangementBox_.setSelectedId(kJustClickComboId, juce::dontSendNotification);
        audioEngine_.setUseTracks(false);
    } else {
        justClickSelected_ = false;
        arrangementBox_.setSelectedId(selectedIdx + 1, juce::dontSendNotification);
        if (!activeArrangementId_.has_value()) {
            activeArrangementId_ = currentArrangements_[static_cast<size_t>(selectedIdx)].id;
        }
        audioEngine_.setUseTracks(!currentArrangements_[static_cast<size_t>(selectedIdx)].tracks.empty());
    }

    applyActiveModeSettings();
    updateTrackStripsEnabled();
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

    const bool usingTracks = isUsingTracks();
    bool activeArrIsTrackless = false;
    if (!justClickSelected_ && activeArrangementId_.has_value()) {
        for (const auto& arr : currentArrangements_) {
            if (arr.id.has_value() && *arr.id == *activeArrangementId_) {
                activeArrIsTrackless = arr.tracks.empty();
                break;
            }
        }
    }
    std::optional<int64_t> targetArrId;
    if (!justClickSelected_ && (usingTracks || activeArrIsTrackless) && activeArrangementId_.has_value()) {
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
    currentSections_ = sections;
    lastSectionLeadingCount_ = audioEngine_.hasLeadingCountSection();
    currentSectionSpans_ = buildSectionSpans(currentSections_, lastSectionLeadingCount_);
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
        audioEngine_.setUseTracks(false);
    } else {
        if (isNewSong || !lastHadTracks_) {
            justClickSelected_ = false;
        }
        audioEngine_.setUseTracks(!justClickSelected_);
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
    auto layout = audioEngine_.getTimelineLayout();
    beatPlayhead_.setTimelineLayout(std::move(layout.barPositions), layout.waveformDurationFraction);
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
    const bool enabled = isUsingTracks();
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
    if (isPlaying == lastPlayingState_ && isPaused == lastPausedState_) return;
    lastPlayingState_ = isPlaying;
    lastPausedState_ = isPaused;

    if (isPlaying && !isPaused) {
        playPauseButton_->setIconType(TransportIconButton::IconType::Pause);
        playPauseButton_->setBaseColour(AppTheme::warningAmber);
        playPauseButton_->setTooltip("Pause");
        if (currentTimerHz_ != 30) {
            currentTimerHz_ = 30;
            startTimerHz(30);
        }
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

void TransportComponent::setTransitionConfig(SongTransitionMode mode, int crossfadeBars) {
    currentCrossfadeBars_ = clampCrossfadeBars(crossfadeBars);
    transitionBox_.setSelectedId(static_cast<int>(mode) + 1, juce::dontSendNotification);
    if (crossfadeBarsStepper_) {
        crossfadeBarsStepper_->setValue(currentCrossfadeBars_);
    }
    crossfadeBarsLabel_.setText(currentCrossfadeBars_ == 1 ? "bar" : "bars", juce::dontSendNotification);
    resized();
    repaint();
}

void TransportComponent::timerCallback() {
    audioEngine_.flushPendingWaveformRebuild();

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
        auto layout = audioEngine_.getTimelineLayout();
        beatPlayhead_.setTimelineLayout(std::move(layout.barPositions), layout.waveformDurationFraction);
    }

    bool showWaveform = audioEngine_.isStructuredTimelineMode();
    double totalBarsElapsed = audioEngine_.getTotalBarsElapsed();
    bool leadingCount = audioEngine_.hasLeadingCountSection();
    int queuedSec = audioEngine_.getQueuedSectionIndex();
    int selectedStartSec = audioEngine_.getSelectedStartSectionIndex();
    bool inSectionStartCountIn = audioEngine_.isInSectionStartCountIn();

    if (leadingCount != lastSectionLeadingCount_) {
        lastSectionLeadingCount_ = leadingCount;
        currentSectionSpans_ = buildSectionSpans(currentSections_, leadingCount);
    }

    bool canSkipBack = false;
    bool canSkipFwd = false;
    bool canRepeat = false;
    bool repeatActive = false;
    if (showWaveform && !currentSectionSpans_.empty()) {
        if (playing) {
            int currentBar = static_cast<int>(std::floor(std::max(0.0, totalBarsElapsed)));
            SectionContext ctx = computeSectionContext(currentSectionSpans_, currentBar);
            const int secCount = static_cast<int>(currentSectionSpans_.size());
            canSkipBack = canSkipSectionInDirection(secCount, ctx, queuedSec, -1);
            canSkipFwd = canSkipSectionInDirection(secCount, ctx, queuedSec, 1);
            canRepeat = canRepeatCurrentSection(secCount, ctx);
            repeatActive = canRepeat && (queuedSec == ctx.currentSectionIndex);
        } else {
            canSkipBack = canStepStartSection(currentSectionSpans_, selectedStartSec, -1);
            canSkipFwd = canStepStartSection(currentSectionSpans_, selectedStartSec, 1);
        }
    }

    auto setBtnStateIfChanged = [](TransportIconButton* btn, bool enabled, const juce::String& tip) {
        if (!btn) return;
        if (btn->isEnabled() != enabled) btn->setEnabled(enabled);
        if (btn->getTooltip() != tip) btn->setTooltip(tip);
    };

    setBtnStateIfChanged(skipBackButton_.get(), canSkipBack,
                         playing ? "Skip Back to Previous Section" : "Select Earlier Start Section");
    setBtnStateIfChanged(skipForwardButton_.get(), canSkipFwd,
                         playing ? "Skip Forward to Next Section" : "Select Later Start Section");

    const bool clickLoopOn = audioEngine_.isClickLoopEnabled();
    const bool fadingOut = audioEngine_.isFadingOut();
    if (fadeOutButton_) {
        fadeOutButton_->setActiveHighlight(fadingOut);
        fadeOutButton_->setBaseColour(fadingOut ? AppTheme::warningAmber.darker(0.25f) : AppTheme::bgPanel);
        setBtnStateIfChanged(fadeOutButton_.get(), (playing && !paused) || fadingOut,
                             fadingOut ? "Fading Out... Click Again to Stop Immediately" : "Fade Out & Stop (4s)");
    }
    if (repeatSectionButton_) {
        if (showWaveform) {
            repeatSectionButton_->setActiveHighlight(repeatActive);
            repeatSectionButton_->setBaseColour(repeatActive ? juce::Colour(0xff0284c7) : AppTheme::bgPanel);
            setBtnStateIfChanged(repeatSectionButton_.get(), canRepeat,
                                 repeatActive ? "Cancel Repeat Current Section" : "Repeat Current Section");
        } else {
            repeatSectionButton_->setActiveHighlight(clickLoopOn);
            repeatSectionButton_->setBaseColour(clickLoopOn ? juce::Colour(0xff0284c7) : AppTheme::bgPanel);
            juce::String tip;
            if (playing) {
                tip = clickLoopOn ? "Loop Active - Click to End After This Bar"
                                  : "Ending After This Bar - Click to Keep Looping";
            } else {
                tip = clickLoopOn ? "Loop Click Bar (Active)"
                                  : "Loop Click Bar (Off - Plays 1 Bar After Count-In)";
            }
            setBtnStateIfChanged(repeatSectionButton_.get(), true, tip);
        }
    }

    const bool loadingTracks = audioEngine_.isLoadingTracks();
    beatPlayhead_.updatePlayhead(totalBarsElapsed,
                                audioEngine_.getCurrentBeatInBar(),
                                audioEngine_.getCurrentBar(),
                                audioEngine_.getBeatsPerBar(),
                                playing,
                                paused,
                                audioEngine_.isCompoundMeter(),
                                showWaveform,
                                audioEngine_.getTrackDurationInBars(),
                                audioEngine_.getTrackBarCount(),
                                loadingTracks,
                                leadingCount,
                                queuedSec,
                                selectedStartSec,
                                inSectionStartCountIn,
                                clickLoopOn,
                                fadingOut);

    auto peaks = audioEngine_.getOutputPeaks();
    bool anyPeakActive = false;
    for (float p : peaks) {
        if (p > 0.0005f) {
            anyPeakActive = true;
            break;
        }
    }
    outputMeter_.updatePeaks(peaks);

    // Adapt GUI timer frequency: 30 Hz during active playback/metering/loading, 5 Hz when idle
    const int desiredHz = ((playing && !paused) || anyPeakActive || loadingTracks) ? 30 : 5;
    if (desiredHz != currentTimerHz_) {
        currentTimerHz_ = desiredHz;
        startTimerHz(desiredHz);
    }
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
    const bool showTrackStrips = expanded && !trackStrips_.empty() && isUsingTracks();
    trackStripsViewport_.setVisible(showTrackStrips);
    updateSubdivisionFaderVisibility();

    resized();
    repaint();
}

void TransportComponent::paint(juce::Graphics& g) {
    if (layoutMode_ == LayoutMode::ExpandedPanel) {
        g.fillAll(AppTheme::pageBgLive);
        auto panelBounds = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(AppTheme::pageBgLive.brighter(0.04f));
        g.fillRoundedRectangle(panelBounds, 6.0f);
        g.setColour(AppTheme::borderCol.interpolatedWith(AppTheme::tabLiveAccent, 0.28f));
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
        tempoDownButton_.setVisible(true);
        tempoUpButton_.setVisible(true);
        tempoDownButton_.setBounds(badgeRow.removeFromLeft(22));
        badgeRow.removeFromLeft(2);
        tempoEditor_.setBounds(badgeRow.removeFromLeft(46));
        badgeRow.removeFromLeft(2);
        tempoUpButton_.setBounds(badgeRow.removeFromLeft(22));
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
        arrangementBox_.setVisible(true);
        arrangementBox_.setBounds(badgeRow.removeFromLeft(156));
        badgeRow.removeFromLeft(10);
        eighthsToggle_.setBounds(badgeRow.removeFromLeft(62));
        badgeRow.removeFromLeft(4);
        sixteenthsToggle_.setBounds(badgeRow.removeFromLeft(68));

        area.removeFromTop(12);

        // Transport controls row: Previous Song | Skip Back | Play/Pause | Stop | Fade Out | Repeat Section | Skip Forward | Next Song   [Transition Dropdown] [Bars]
        auto transportRow = area.removeFromTop(36);
        prevSongButton_->setVisible(true);
        skipBackButton_->setVisible(true);
        fadeOutButton_->setVisible(true);
        repeatSectionButton_->setVisible(true);
        skipForwardButton_->setVisible(true);
        nextSongButton_->setVisible(true);
        transitionBox_.setVisible(true);

        const bool showCrossfadeInput = (transitionBox_.getSelectedId() == 4);
        if (crossfadeBarsStepper_) crossfadeBarsStepper_->setVisible(showCrossfadeInput);
        crossfadeBarsLabel_.setVisible(showCrossfadeInput);

        const int btnGap = 6;
        const int iconBtnW = 42;
        const int playW = 56;
        const int totalBtnW = iconBtnW * 7 + playW + btnGap * 7;
        const int xfInputW = showCrossfadeInput ? (4 + 42 + 4 + 30) : 0; // gap + stepper + gap + "bars"
        const int maxRightBlockW = std::max(140, transportRow.getWidth() - totalBtnW - 12);
        const int dropdownW = showCrossfadeInput
                                  ? std::clamp(maxRightBlockW - xfInputW, 92, 118)
                                  : std::clamp(maxRightBlockW, 136, 178);
        const int totalRightW = dropdownW + xfInputW;

        auto rightBlock = juce::Rectangle<int>(transportRow.getRight() - totalRightW,
                                               transportRow.getY() + 4,
                                               totalRightW,
                                               28);
        transitionBox_.setBounds(rightBlock.removeFromLeft(dropdownW));
        if (showCrossfadeInput) {
            rightBlock.removeFromLeft(4);
            if (crossfadeBarsStepper_) crossfadeBarsStepper_->setBounds(rightBlock.removeFromLeft(42));
            rightBlock.removeFromLeft(4);
            crossfadeBarsLabel_.setBounds(rightBlock);
        }

        const int rightEdgeForBtns = transportRow.getRight() - totalRightW - 12;
        const int centeredX = transportRow.getX() + (transportRow.getWidth() - totalBtnW) / 2;
        const int maxBtnX = rightEdgeForBtns - totalBtnW;
        const int btnX = std::max(transportRow.getX(), std::min(centeredX, maxBtnX));
        const int availBtnW = std::min(totalBtnW, std::max(0, rightEdgeForBtns - btnX));
        auto btnGroup = juce::Rectangle<int>(btnX, transportRow.getY(), availBtnW, 36);
        prevSongButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        skipBackButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        playPauseButton_->setBounds(btnGroup.removeFromLeft(playW));
        btnGroup.removeFromLeft(btnGap);
        stopButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        fadeOutButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        repeatSectionButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        skipForwardButton_->setBounds(btnGroup.removeFromLeft(iconBtnW));
        btnGroup.removeFromLeft(btnGap);
        nextSongButton_->setBounds(btnGroup);

        area.removeFromTop(12);

        // Middle row: Full-width Beat Playhead / Summed Tracks Waveform
        int playheadH = std::clamp(area.getHeight() / 3, 96, 138);
        beatPlayhead_.setBounds(area.removeFromTop(playheadH));

        area.removeFromTop(14);

        // Bottom area: Full-height Vertical Mixer Faders (Master, Click, [8th Click], [16th Click], Guide + per-track strips) + Output Peak Meters
        const bool hasTrackStrips = !trackStrips_.empty() && isUsingTracks();
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
            inner.removeFromTop(6);
            auto chArea = inner.removeFromBottom(22);
            if (chBox != nullptr) {
                chBox->setBounds(chArea);
            }
            inner.removeFromBottom(6);
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
    transitionBox_.setVisible(false);
    if (crossfadeBarsStepper_) crossfadeBarsStepper_->setVisible(false);
    crossfadeBarsLabel_.setVisible(false);
    tempoDownButton_.setVisible(false);
    tempoUpButton_.setVisible(false);
    skipBackButton_->setVisible(true);
    fadeOutButton_->setVisible(true);
    repeatSectionButton_->setVisible(true);
    skipForwardButton_->setVisible(true);
    arrangementBox_.setVisible(true);
    clickChannelBox_.setVisible(false);
    guideChannelBox_.setVisible(false);
    trackStripsViewport_.setVisible(false);
    updateSubdivisionFaderVisibility();

    auto area = getLocalBounds().reduced(12, 8);

    // Left controls: Skip Back | Play/Pause | Stop | Fade Out | Repeat Section | Skip Forward icon buttons
    auto btnArea = area.removeFromLeft(206).withSizeKeepingCentre(206, 38);
    skipBackButton_->setBounds(btnArea.removeFromLeft(28));
    btnArea.removeFromLeft(4);
    playPauseButton_->setBounds(btnArea.removeFromLeft(42));
    btnArea.removeFromLeft(4);
    stopButton_->setBounds(btnArea.removeFromLeft(30));
    btnArea.removeFromLeft(4);
    fadeOutButton_->setBounds(btnArea.removeFromLeft(30));
    btnArea.removeFromLeft(4);
    repeatSectionButton_->setBounds(btnArea.removeFromLeft(28));
    btnArea.removeFromLeft(4);
    skipForwardButton_->setBounds(btnArea);

    area.removeFromLeft(8);

    // Song info column
    auto infoArea = area.removeFromLeft(408);
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
    arrangementBox_.setBounds(badgeRow.removeFromLeft(104));
    badgeRow.removeFromLeft(2);
    eighthsToggle_.setBounds(badgeRow.removeFromLeft(54));
    badgeRow.removeFromLeft(2);
    sixteenthsToggle_.setBounds(badgeRow.removeFromLeft(56));

    area.removeFromLeft(8);

    // Center: Beat playhead
    auto playheadArea = area.removeFromLeft(132);
    beatPlayhead_.setBounds(playheadArea.withSizeKeepingCentre(128, 60));

    area.removeFromLeft(8);

    // Right: Output Peak Meter
    auto meterArea = area.removeFromRight(126);
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
