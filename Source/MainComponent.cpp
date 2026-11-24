#include "MainComponent.h"
#include "Services/AudioAttachmentService.h"
#include <cstdlib>

namespace tracks {

MainComponent::MainComponent() {
    juce::LookAndFeel::setDefaultLookAndFeel(&theme_);
    setOpaque(true);

    // Database path: locate tracks_library.sqlite3 in project root
    auto dbPath = (AudioAttachmentService::getProjectRoot() / "tracks_library.sqlite3").string();
    db_ = std::make_unique<DatabaseService>(dbPath);

    // Audio Engine
    audioEngine_ = std::make_unique<AudioEngine>();

    // Brand Label
    brandLabel_.setFont(juce::Font(16.0f, juce::Font::bold));
    brandLabel_.setColour(juce::Label::textColourId, AppTheme::primaryBlue);
    addAndMakeVisible(brandLabel_);

    // Navigation buttons
    auto setupNavBtn = [this](juce::TextButton& btn, ActiveView view) {
        btn.onClick = [this, view]() { switchView(view); };
        addAndMakeVisible(btn);
    };

    setupNavBtn(navLibraryButton_, ActiveView::Library);
    setupNavBtn(navOpenSetButton_, ActiveView::OpenSet);
    setupNavBtn(navSettingsButton_, ActiveView::Settings);

    relaunchButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    relaunchButton_.onClick = [this]() { relaunchApp(); };
    addAndMakeVisible(relaunchButton_);

    // Initialize views
    libraryView_ = std::make_unique<LibraryView>(*db_, *audioEngine_);
    libraryView_->onSongLoadedForPlayback = [this](const Song& s) {
        playSong(s);
    };
    addChildComponent(libraryView_.get());

    openSetView_ = std::make_unique<OpenSetView>(*db_, *audioEngine_);
    openSetView_->onSongSelectedForPlayback = [this](const Song& s) {
        playSong(s);
    };
    addChildComponent(openSetView_.get());

    settingsView_ = std::make_unique<SettingsView>(*db_, *audioEngine_);
    addChildComponent(settingsView_.get());

    // Initialize transport bar
    transport_ = std::make_unique<TransportComponent>(*audioEngine_);
    transport_->onPlayClicked = [this]() {
        if (!audioEngine_->isPlaying()) {
            audioEngine_->play();
        } else {
            audioEngine_->resume();
        }
    };
    transport_->onPauseClicked = [this]() {
        audioEngine_->pause();
    };
    transport_->onStopClicked = [this]() {
        audioEngine_->stop();
    };
    transport_->onMasterGainChanged = [this](float g) {
        audioEngine_->setMasterGain(g);
    };
    transport_->onClickGainChanged = [this](float g) {
        audioEngine_->setClickGain(g);
    };
    transport_->onGuideGainChanged = [this](float g) {
        audioEngine_->setGuideGain(g);
    };
    transport_->onClickChannelChanged = [this](int ch) {
        db_->setAppSetting("volume.channel.click_db", std::to_string(ch));
    };
    transport_->onGuideChannelChanged = [this](int ch) {
        db_->setAppSetting("volume.channel.guide_db", std::to_string(ch));
    };
    transport_->onTrackMixChanged = [this](int64_t trackId, float gain, int outputChannel, bool stereoOutput) {
        db_->updateTrackMix(trackId, gain, outputChannel, stereoOutput);
    };
    transport_->onSongClickSubdivisionsChanged = [this](int64_t songId, bool eighths, bool sixteenths, float eighthsGain, float sixteenthsGain) {
        db_->updateSongClickSubdivisions(songId, eighths, sixteenths, eighthsGain, sixteenthsGain);
        if (openSetView_) {
            openSetView_->syncSongClickSubdivisions(songId, eighths, sixteenths, eighthsGain, sixteenthsGain);
        }
    };
    transport_->syncClickAndGuideChannels(audioEngine_->getClickChannel(), audioEngine_->getGuideChannel());
    addAndMakeVisible(transport_.get());

    switchView(ActiveView::Library);
    setSize(1080, 720);
}

void MainComponent::playSong(const Song& song) {
    audioEngine_->loadSong(song);
    transport_->setSongInfo(song.title, song.artist, song.tempo, song.key, song.timeSignature);
    transport_->syncTracksAvailability(song.id, song.tracks, audioEngine_->hasLoadedTracks(), song.sections);
    transport_->syncClickSubdivisions(song.id, song.clickEighths, song.clickSixteenths, song.clickEighthsGain, song.clickSixteenthsGain);
}

void MainComponent::switchView(ActiveView view) {
    activeView_ = view;

    libraryView_->setVisible(activeView_ == ActiveView::Library);
    openSetView_->setVisible(activeView_ == ActiveView::OpenSet);
    settingsView_->setVisible(activeView_ == ActiveView::Settings);

    if (activeView_ == ActiveView::Library) {
        libraryView_->refreshSongList();
    } else if (activeView_ == ActiveView::OpenSet) {
        transport_->syncClickAndGuideChannels(audioEngine_->getClickChannel(), audioEngine_->getGuideChannel());
        openSetView_->refreshSetlists();
    } else if (activeView_ == ActiveView::Settings) {
        settingsView_->refreshSettings();
    }

    updateNavButtonStyles();
    resized();
}

void MainComponent::updateNavButtonStyles() {
    auto setBtnActive = [](juce::TextButton& btn, bool active) {
        btn.setColour(juce::TextButton::buttonColourId,
                      active ? AppTheme::primaryBlue : AppTheme::bgPanel);
    };

    setBtnActive(navLibraryButton_, activeView_ == ActiveView::Library);
    setBtnActive(navOpenSetButton_, activeView_ == ActiveView::OpenSet);
    setBtnActive(navSettingsButton_, activeView_ == ActiveView::Settings);
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgDark);

    // Divider line below top nav bar
    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(48, 0.0f, static_cast<float>(getWidth()));
}

void MainComponent::resized() {
    auto area = getLocalBounds();

    // Top Navigation Bar (height 48)
    auto topBar = area.removeFromTop(48).reduced(12, 8);
    relaunchButton_.setBounds(topBar.removeFromRight(85));
    topBar.removeFromRight(12);

    brandLabel_.setBounds(topBar.removeFromLeft(90));
    topBar.removeFromLeft(20);

    navLibraryButton_.setBounds(topBar.removeFromLeft(100));
    topBar.removeFromLeft(8);
    navOpenSetButton_.setBounds(topBar.removeFromLeft(110));
    topBar.removeFromLeft(8);
    navSettingsButton_.setBounds(topBar.removeFromLeft(100));

    if (activeView_ == ActiveView::OpenSet) {
        openSetView_->setBounds(area);
        transport_->setLayoutMode(TransportComponent::LayoutMode::ExpandedPanel);
        auto transportBounds = openSetView_->getTransportArea().translated(openSetView_->getX(), openSetView_->getY());
        transport_->setBounds(transportBounds);
        transport_->toFront(false);
    } else {
        // Bottom Transport Bar (height 84)
        auto transportArea = area.removeFromBottom(84);
        transport_->setLayoutMode(TransportComponent::LayoutMode::DockedBar);
        transport_->setBounds(transportArea);

        // Main view content area
        if (libraryView_->isVisible()) libraryView_->setBounds(area);
        if (settingsView_->isVisible()) settingsView_->setBounds(area);
    }
}

void MainComponent::relaunchApp() {
    auto appFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto bundle = appFile.getParentDirectory().getParentDirectory().getParentDirectory();
    juce::String targetPath = (bundle.hasFileExtension(".app") && bundle.isDirectory())
                                  ? bundle.getFullPathName()
                                  : appFile.getFullPathName();

    juce::String cmd = "/bin/sh -c 'sleep 0.2; open -n \"" + targetPath + "\"' &";
    std::system(cmd.toRawUTF8());

    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace tracks
