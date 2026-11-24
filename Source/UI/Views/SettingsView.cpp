#include "SettingsView.h"
#include "../../Common/AppTheme.h"
#include <cmath>

namespace tracks {

SettingsView::SettingsView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
    setOpaque(true);

    titleLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(titleLabel_);

    auto setupSection = [this](juce::Label& lbl) {
        lbl.setFont(juce::Font(12.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::primaryBlue);
        addAndMakeVisible(lbl);
    };

    auto setupField = [this](juce::Label& lbl) {
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textMuted);
        addAndMakeVisible(lbl);
    };

    // Device section
    setupSection(deviceSectionLabel_);
    setupField(deviceLabel_);
    addAndMakeVisible(deviceBox_);
    deviceBox_.onChange = [this]() {
        auto name = deviceBox_.getText().toStdString();
        if (!name.empty()) {
            audioEngine_.selectOutputDevice(name);
            saveSetting("audio.device", name);
            channelCountLabel_.setText("Available Channels: " + juce::String(audioEngine_.getCurrentOutputChannels()),
                                       juce::dontSendNotification);
        }
    };

    refreshDevicesButton_.onClick = [this]() { populateDevices(); };
    addAndMakeVisible(refreshDevicesButton_);

    channelCountLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
    channelCountLabel_.setColour(juce::Label::textColourId, AppTheme::accentGreen);
    addAndMakeVisible(channelCountLabel_);

    // Routing section
    setupSection(routingSectionLabel_);

    auto populateChannels = [](juce::ComboBox& box) {
        for (int ch = 1; ch <= 16; ++ch) {
            box.addItem("Channel " + juce::String(ch), ch);
        }
    };

    setupField(clickChannelLabel_);
    populateChannels(clickChannelBox_);
    clickChannelBox_.onChange = [this]() {
        int ch = clickChannelBox_.getSelectedId() - 1;
        audioEngine_.setClickChannel(ch);
        saveSetting("volume.channel.click_db", std::to_string(ch));
    };
    addAndMakeVisible(clickChannelBox_);

    setupField(guideChannelLabel_);
    populateChannels(guideChannelBox_);
    guideChannelBox_.onChange = [this]() {
        int ch = guideChannelBox_.getSelectedId() - 1;
        audioEngine_.setGuideChannel(ch);
        saveSetting("volume.channel.guide_db", std::to_string(ch));
    };
    addAndMakeVisible(guideChannelBox_);

    // Assets section
    setupSection(assetsSectionLabel_);

    setupField(clickSoundLabel_);
    clickSoundBox_.addItem("Embedded Tone (Accent / Normal)", 1);
    clickSoundBox_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(clickSoundBox_);

    setupField(voiceLabel_);
    voiceBox_.addItem("Embedded Female English Voice", 1);
    voiceBox_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(voiceBox_);

    // Volumes section
    setupSection(volumeSectionLabel_);

    auto setupVolSlider = [this](juce::Slider& s, const std::string& key) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        s.setRange(-30.0, 6.0, 0.5);
        s.setTextValueSuffix(" dB");
        addAndMakeVisible(s);
    };

    setupField(masterVolLabel_);
    setupVolSlider(masterVolSlider_, "volume.master_db");
    masterVolSlider_.onValueChange = [this]() {
        float dbVal = static_cast<float>(masterVolSlider_.getValue());
        float lin = (dbVal <= -30.0f) ? 0.0f : std::pow(10.0f, dbVal / 20.0f);
        audioEngine_.setMasterGain(lin);
        saveSetting("volume.master_db", std::to_string(dbVal));
    };

    setupField(clickVolLabel_);
    setupVolSlider(clickVolSlider_, "volume.click_db");
    clickVolSlider_.onValueChange = [this]() {
        float dbVal = static_cast<float>(clickVolSlider_.getValue());
        float lin = (dbVal <= -30.0f) ? 0.0f : std::pow(10.0f, dbVal / 20.0f);
        audioEngine_.setClickGain(lin);
        saveSetting("volume.click_db", std::to_string(dbVal));
    };

    setupField(guideVolLabel_);
    setupVolSlider(guideVolSlider_, "volume.guide_db");
    guideVolSlider_.onValueChange = [this]() {
        float dbVal = static_cast<float>(guideVolSlider_.getValue());
        float lin = (dbVal <= -30.0f) ? 0.0f : std::pow(10.0f, dbVal / 20.0f);
        audioEngine_.setGuideGain(lin);
        saveSetting("volume.guide_db", std::to_string(dbVal));
    };

    refreshSettings();
}

void SettingsView::populateDevices() {
    deviceBox_.clear();
    auto devices = audioEngine_.getAvailableOutputDevices();
    int selId = 1;
    for (size_t i = 0; i < devices.size(); ++i) {
        deviceBox_.addItem(devices[i].name, static_cast<int>(i + 1));
        if (devices[i].isDefault) {
            selId = static_cast<int>(i + 1);
        }
    }
    deviceBox_.setSelectedId(selId, juce::dontSendNotification);
    channelCountLabel_.setText("Available Channels: " + juce::String(audioEngine_.getCurrentOutputChannels()),
                               juce::dontSendNotification);
}

void SettingsView::refreshSettings() {
    populateDevices();

    int clickCh = 0;
    try { clickCh = std::stoi(db_.getAppSetting("volume.channel.click_db", "0")); } catch (...) {}
    clickChannelBox_.setSelectedId(clickCh + 1, juce::dontSendNotification);
    audioEngine_.setClickChannel(clickCh);

    int guideCh = 0;
    try { guideCh = std::stoi(db_.getAppSetting("volume.channel.guide_db", "0")); } catch (...) {}
    guideChannelBox_.setSelectedId(guideCh + 1, juce::dontSendNotification);
    audioEngine_.setGuideChannel(guideCh);

    float masterDb = 0.0f;
    try { masterDb = std::stof(db_.getAppSetting("volume.master_db", "0.0")); } catch (...) {}
    masterVolSlider_.setValue(masterDb, juce::dontSendNotification);

    float clickDb = 0.0f;
    try { clickDb = std::stof(db_.getAppSetting("volume.click_db", "0.0")); } catch (...) {}
    clickVolSlider_.setValue(clickDb, juce::dontSendNotification);

    float guideDb = 0.0f;
    try { guideDb = std::stof(db_.getAppSetting("volume.guide_db", "0.0")); } catch (...) {}
    guideVolSlider_.setValue(guideDb, juce::dontSendNotification);
}

void SettingsView::saveSetting(const std::string& key, const std::string& val) {
    db_.setAppSetting(key, val);
}

void SettingsView::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgDark);
}

void SettingsView::resized() {
    auto area = getLocalBounds().reduced(24);

    titleLabel_.setBounds(area.removeFromTop(32));
    area.removeFromTop(16);

    int rowH = 26;
    int gap = 8;
    int sectionGap = 20;

    // Device
    deviceSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto dRow = area.removeFromTop(rowH);
    deviceLabel_.setBounds(dRow.removeFromLeft(120));
    deviceBox_.setBounds(dRow.removeFromLeft(280));
    dRow.removeFromLeft(8);
    refreshDevicesButton_.setBounds(dRow.removeFromLeft(90));
    area.removeFromTop(4);
    channelCountLabel_.setBounds(area.removeFromTop(18));

    area.removeFromTop(sectionGap);

    // Routing
    routingSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto cRow = area.removeFromTop(rowH);
    clickChannelLabel_.setBounds(cRow.removeFromLeft(140));
    clickChannelBox_.setBounds(cRow.removeFromLeft(180));

    area.removeFromTop(gap);
    auto gRow = area.removeFromTop(rowH);
    guideChannelLabel_.setBounds(gRow.removeFromLeft(140));
    guideChannelBox_.setBounds(gRow.removeFromLeft(180));

    area.removeFromTop(sectionGap);

    // Assets
    assetsSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto asRow1 = area.removeFromTop(rowH);
    clickSoundLabel_.setBounds(asRow1.removeFromLeft(140));
    clickSoundBox_.setBounds(asRow1.removeFromLeft(240));

    area.removeFromTop(gap);
    auto asRow2 = area.removeFromTop(rowH);
    voiceLabel_.setBounds(asRow2.removeFromLeft(140));
    voiceBox_.setBounds(asRow2.removeFromLeft(240));

    area.removeFromTop(sectionGap);

    // Volumes
    volumeSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto vRow1 = area.removeFromTop(rowH);
    masterVolLabel_.setBounds(vRow1.removeFromLeft(140));
    masterVolSlider_.setBounds(vRow1.removeFromLeft(300));

    area.removeFromTop(gap);
    auto vRow2 = area.removeFromTop(rowH);
    clickVolLabel_.setBounds(vRow2.removeFromLeft(140));
    clickVolSlider_.setBounds(vRow2.removeFromLeft(300));

    area.removeFromTop(gap);
    auto vRow3 = area.removeFromTop(rowH);
    guideVolLabel_.setBounds(vRow3.removeFromLeft(140));
    guideVolSlider_.setBounds(vRow3.removeFromLeft(300));
}

} // namespace tracks
