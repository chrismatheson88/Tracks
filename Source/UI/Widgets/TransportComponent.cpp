#include "TransportComponent.h"
#include "../../Common/AppTheme.h"
#include <cmath>

namespace tracks {

TransportComponent::TransportComponent(AudioEngine& audioEngine)
    : audioEngine_(audioEngine) {
    setOpaque(true);

    // Play/Pause button
    playPauseButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    playPauseButton_.onClick = [this]() {
        if (!audioEngine_.isPlaying()) {
            if (onPlayClicked) onPlayClicked();
        } else if (audioEngine_.isPaused()) {
            if (onPlayClicked) onPlayClicked(); // Resume
        } else {
            if (onPauseClicked) onPauseClicked();
        }
    };
    addAndMakeVisible(playPauseButton_);

    // Stop button
    stopButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    stopButton_.onClick = [this]() {
        if (onStopClicked) onStopClicked();
    };
    addAndMakeVisible(stopButton_);

    // Song info labels
    songTitleLabel_.setFont(juce::Font(15.0f, juce::Font::bold));
    songTitleLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    songTitleLabel_.setText("No Song Selected", juce::dontSendNotification);
    addAndMakeVisible(songTitleLabel_);

    songArtistLabel_.setFont(juce::Font(12.0f, juce::Font::plain));
    songArtistLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    songArtistLabel_.setText("Select a song from library or set", juce::dontSendNotification);
    addAndMakeVisible(songArtistLabel_);

    // Badges
    auto setupBadge = [this](juce::Label& lbl) {
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::backgroundColourId, AppTheme::bgInput);
        lbl.setColour(juce::Label::textColourId, AppTheme::textLight);
        lbl.setColour(juce::Label::outlineColourId, AppTheme::borderCol);
        lbl.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(lbl);
    };

    setupBadge(tempoBadge_);
    tempoBadge_.setText("120 BPM", juce::dontSendNotification);

    setupBadge(keyBadge_);
    keyBadge_.setText("Key: C", juce::dontSendNotification);

    setupBadge(timeSigBadge_);
    timeSigBadge_.setText("4/4", juce::dontSendNotification);

    // Sliders
    auto setupSlider = [this](juce::Slider& s, juce::Label& l) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRange(-30.0, 6.0, 0.5);
        s.setValue(0.0);
        addAndMakeVisible(s);

        l.setFont(juce::Font(10.0f, juce::Font::plain));
        l.setColour(juce::Label::textColourId, AppTheme::textMuted);
        l.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(l);
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

    setupSlider(guideSlider_, guideLabel_);
    guideSlider_.onValueChange = [this]() {
        float db = static_cast<float>(guideSlider_.getValue());
        float linear = (db <= -30.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
        if (onGuideGainChanged) onGuideGainChanged(linear);
    };

    addAndMakeVisible(beatPlayhead_);
    addAndMakeVisible(outputMeter_);

    startTimerHz(30); // 30 FPS meter and playhead animation
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
    tempoBadge_.setText(juce::String(tempo) + " BPM", juce::dontSendNotification);
    keyBadge_.setText("Key: " + (key.empty() ? "C" : key), juce::dontSendNotification);
    timeSigBadge_.setText(timeSignature.empty() ? "4/4" : timeSignature, juce::dontSendNotification);
}

void TransportComponent::setPlayingState(bool isPlaying, bool isPaused) {
    if (isPlaying && !isPaused) {
        playPauseButton_.setButtonText("⏸ Pause");
        playPauseButton_.setColour(juce::TextButton::buttonColourId, AppTheme::warningAmber);
    } else {
        playPauseButton_.setButtonText("▶ Play");
        playPauseButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    }
}

void TransportComponent::timerCallback() {
    bool playing = audioEngine_.isPlaying();
    bool paused = audioEngine_.isPaused();
    setPlayingState(playing, paused);

    beatPlayhead_.updatePlayhead(audioEngine_.getCurrentBeatInBar(),
                                audioEngine_.getCurrentBar(),
                                audioEngine_.getBeatsPerBar(),
                                playing && !paused,
                                audioEngine_.getCurrentBar() < 2);

    outputMeter_.updatePeaks(audioEngine_.getOutputPeaks());
}

void TransportComponent::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgDark.brighter(0.04f));
    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
}

void TransportComponent::resized() {
    auto area = getLocalBounds().reduced(12, 8);

    // Left controls: Play and Stop buttons
    auto btnArea = area.removeFromLeft(140);
    playPauseButton_.setBounds(btnArea.removeFromTop(32));
    btnArea.removeFromTop(6);
    stopButton_.setBounds(btnArea.removeFromTop(28));

    area.removeFromLeft(16);

    // Song info column
    auto infoArea = area.removeFromLeft(220);
    songTitleLabel_.setBounds(infoArea.removeFromTop(20));
    songArtistLabel_.setBounds(infoArea.removeFromTop(16));
    infoArea.removeFromTop(6);
    auto badgeRow = infoArea.removeFromTop(22);
    tempoBadge_.setBounds(badgeRow.removeFromLeft(64));
    badgeRow.removeFromLeft(6);
    keyBadge_.setBounds(badgeRow.removeFromLeft(64));
    badgeRow.removeFromLeft(6);
    timeSigBadge_.setBounds(badgeRow.removeFromLeft(50));

    area.removeFromLeft(16);

    // Center: Beat playhead
    auto playheadArea = area.removeFromLeft(160);
    beatPlayhead_.setBounds(playheadArea.withSizeKeepingCentre(150, 60));

    area.removeFromLeft(16);

    // Right: Output Peak Meter
    auto meterArea = area.removeFromRight(150);
    outputMeter_.setBounds(meterArea);

    area.removeFromRight(16);

    // Volume sliders
    auto slidersArea = area;
    int sliderH = 18;
    int gap = 4;

    auto row1 = slidersArea.removeFromTop(sliderH);
    masterLabel_.setBounds(row1.removeFromLeft(45));
    masterSlider_.setBounds(row1);

    slidersArea.removeFromTop(gap);
    auto row2 = slidersArea.removeFromTop(sliderH);
    clickLabel_.setBounds(row2.removeFromLeft(45));
    clickSlider_.setBounds(row2);

    slidersArea.removeFromTop(gap);
    auto row3 = slidersArea.removeFromTop(sliderH);
    guideLabel_.setBounds(row3.removeFromLeft(45));
    guideSlider_.setBounds(row3);
}

} // namespace tracks
