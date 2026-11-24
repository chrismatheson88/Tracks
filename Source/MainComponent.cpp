#include "MainComponent.h"
#include "Services/AudioAttachmentService.h"

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
    addAndMakeVisible(transport_.get());

    switchView(ActiveView::Library);
    setSize(1080, 720);
}

void MainComponent::playSong(const Song& song) {
    audioEngine_->loadSong(song);
    transport_->setSongInfo(song.title, song.artist, song.tempo, song.key, song.timeSignature);
}

void MainComponent::switchView(ActiveView view) {
    activeView_ = view;

    libraryView_->setVisible(activeView_ == ActiveView::Library);
    openSetView_->setVisible(activeView_ == ActiveView::OpenSet);
    settingsView_->setVisible(activeView_ == ActiveView::Settings);

    if (activeView_ == ActiveView::Library) {
        libraryView_->refreshSongList();
    } else if (activeView_ == ActiveView::OpenSet) {
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
    brandLabel_.setBounds(topBar.removeFromLeft(90));
    topBar.removeFromLeft(20);

    navLibraryButton_.setBounds(topBar.removeFromLeft(100));
    topBar.removeFromLeft(8);
    navOpenSetButton_.setBounds(topBar.removeFromLeft(110));
    topBar.removeFromLeft(8);
    navSettingsButton_.setBounds(topBar.removeFromLeft(100));

    // Bottom Transport Bar (height 84)
    auto transportArea = area.removeFromBottom(84);
    transport_->setBounds(transportArea);

    // Main view content area
    if (libraryView_->isVisible()) libraryView_->setBounds(area);
    if (openSetView_->isVisible()) openSetView_->setBounds(area);
    if (settingsView_->isVisible()) settingsView_->setBounds(area);
}

} // namespace tracks
