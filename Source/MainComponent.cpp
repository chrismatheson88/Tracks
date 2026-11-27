#include "MainComponent.h"
#include "Services/AudioAttachmentService.h"
#include <BinaryData.h>
#include <cstdlib>

namespace tracks {

namespace {

juce::Path makeTopRoundedTabPath(juce::Rectangle<float> r, float cornerRadius, bool closeBottom) {
    juce::Path p;
    const float rad = std::min({ cornerRadius, r.getWidth() * 0.5f, r.getHeight() * 0.5f });
    p.startNewSubPath(r.getX(), r.getBottom());
    p.lineTo(r.getX(), r.getY() + rad);
    p.addCentredArc(r.getX() + rad, r.getY() + rad, rad, rad, 0.0f,
                    -juce::MathConstants<float>::halfPi, 0.0f, false);
    p.lineTo(r.getRight() - rad, r.getY());
    p.addCentredArc(r.getRight() - rad, r.getY() + rad, rad, rad, 0.0f,
                    0.0f, juce::MathConstants<float>::halfPi, false);
    p.lineTo(r.getRight(), r.getBottom());
    if (closeBottom) {
        p.closeSubPath();
    }
    return p;
}

} // namespace

class MainComponent::NavTabButton : public juce::Button {
public:
    NavTabButton(const juce::String& label, juce::Colour accent, juce::Colour pageBg)
        : juce::Button(label), accent_(accent), pageBg_(pageBg) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus(false);
    }

    void setActive(bool active) {
        if (active_ != active) {
            active_ = active;
            repaint();
        }
    }

    bool isActive() const { return active_; }
    juce::Colour getAccentColour() const { return accent_; }
    juce::Colour getPageBackgroundColour() const { return pageBg_; }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
        const float w = static_cast<float>(getWidth());
        const float h = static_cast<float>(getHeight());
        if (w <= 2.0f || h <= 2.0f) return;

        if (active_) {
            juce::Rectangle<float> r(0.5f, 0.5f, w - 1.0f, h - 0.5f);
            auto fillPath = makeTopRoundedTabPath(r, 7.0f, true);
            auto strokePath = makeTopRoundedTabPath(r, 7.0f, false);

            // Base page background so the active tab merges seamlessly into the page below
            g.setColour(pageBg_);
            g.fillPath(fillPath);

            // Subtle top-down accent wash ending at 0.07f to match the page's top glow seamlessly
            juce::ColourGradient tabWash(accent_.withAlpha(0.18f), 0.0f, r.getY(),
                                         accent_.withAlpha(0.07f), 0.0f, r.getBottom(), false);
            g.setGradientFill(tabWash);
            g.fillPath(fillPath);

            // Crisp top accent bar clipped to the tab's rounded top corners
            {
                juce::Graphics::ScopedSaveState state(g);
                g.reduceClipRegion(fillPath);
                g.setColour(accent_);
                g.fillRect(r.getX(), r.getY(), r.getWidth(), 3.0f);
            }

            // Open-bottom border around left, top, and right edges
            g.setColour(AppTheme::borderCol.interpolatedWith(accent_, 0.45f));
            g.strokePath(strokePath, juce::PathStrokeType(1.0f));

            // Label
            g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.setColour(AppTheme::textLight);
            g.drawText(getButtonText(),
                       r.withTrimmedTop(3.0f).toNearestInt(),
                       juce::Justification::centred,
                       false);
        } else {
            juce::Rectangle<float> r(0.5f, 5.5f, w - 1.0f, h - 6.5f);
            auto fillPath = makeTopRoundedTabPath(r, 6.0f, true);
            auto strokePath = makeTopRoundedTabPath(r, 6.0f, false);

            juce::Colour bg = AppTheme::headerBg.brighter(0.06f);
            if (down) {
                bg = pageBg_.darker(0.08f);
            } else if (highlighted) {
                bg = bg.interpolatedWith(pageBg_, 0.65f).interpolatedWith(accent_, 0.08f);
            }
            g.setColour(bg);
            g.fillPath(fillPath);

            // Subtle top accent hairline so each tab's logo hue is gently visible even when inactive
            {
                juce::Graphics::ScopedSaveState state(g);
                g.reduceClipRegion(fillPath);
                g.setColour(accent_.withAlpha(highlighted ? 0.78f : 0.36f));
                g.fillRect(r.getX(), r.getY(), r.getWidth(), 2.0f);
            }

            g.setColour(AppTheme::borderCol.withAlpha(highlighted ? 0.80f : 0.42f));
            g.strokePath(strokePath, juce::PathStrokeType(1.0f));

            g.setFont(juce::Font(12.5f, juce::Font::bold));
            g.setColour(highlighted ? AppTheme::textLight : AppTheme::textMuted);
            g.drawText(getButtonText(),
                       r.withTrimmedTop(2.0f).toNearestInt(),
                       juce::Justification::centred,
                       false);
        }
    }

private:
    juce::Colour accent_;
    juce::Colour pageBg_;
    bool active_ = false;
};

MainComponent::MainComponent() {
    juce::LookAndFeel::setDefaultLookAndFeel(&theme_);
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addKeyListener(this);

    // Database path: locate tracks_library.sqlite3 in project root
    auto dbPath = (AudioAttachmentService::getProjectRoot() / "tracks_library.sqlite3").string();
    db_ = std::make_unique<DatabaseService>(dbPath);

    // Audio Engine
    audioEngine_ = std::make_unique<AudioEngine>();
    audioEngine_->onSongTransitionTriggered = [this](AudioEngine::TransitionTrigger trigger) {
        if (activeView_ == ActiveView::OpenSet && openSetView_) {
            openSetView_->executeSongTransition(trigger);
        }
    };

    // Keyboard & MIDI Controller Service
    controlSurface_ = std::make_unique<ControlSurfaceService>(*db_);
    controlSurface_->onActionTriggered = [this](ControlAction action) {
        executeControlAction(action);
    };

    // Brand Logo & Label
    brandIcon_ = juce::ImageCache::getFromMemory(BinaryData::TracksLogo_png, BinaryData::TracksLogo_pngSize);
    brandLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    brandLabel_.setColour(juce::Label::textColourId, AppTheme::primaryBlue);
    addAndMakeVisible(brandLabel_);

    // Navigation tabs (Blue / Purple / Pink from Tracks logo)
    auto setupNavTab = [this](std::unique_ptr<NavTabButton>& btn,
                              const juce::String& label,
                              juce::Colour accent,
                              juce::Colour pageBg,
                              ActiveView view) {
        btn = std::make_unique<NavTabButton>(label, accent, pageBg);
        btn->onClick = [this, view]() { switchView(view); };
        addAndMakeVisible(btn.get());
    };

    setupNavTab(navLibraryButton_, "Library", AppTheme::tabLibraryAccent, AppTheme::pageBgLibrary, ActiveView::Library);
    setupNavTab(navOpenSetButton_, "Live", AppTheme::tabLiveAccent, AppTheme::pageBgLive, ActiveView::OpenSet);
    setupNavTab(navSettingsButton_, "Settings", AppTheme::tabSettingsAccent, AppTheme::pageBgSettings, ActiveView::Settings);

    relaunchButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    relaunchButton_.setWantsKeyboardFocus(false);
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
    openSetView_->onSongNavigationStateChanged = [this](bool canPrev, bool canNext) {
        if (transport_) {
            transport_->setSongNavigationState(canPrev, canNext);
        }
    };
    addChildComponent(openSetView_.get());

    settingsView_ = std::make_unique<SettingsView>(*db_, *audioEngine_, *controlSurface_);
    const std::string announceSetting = db_->getAppSetting("audio.announce_song_name", "1");
    announceSongName_ = (announceSetting != "0" && announceSetting != "false");
    settingsView_->onAnnounceSongNameChanged = [this](bool enabled) {
        announceSongName_ = enabled;
        if (!enabled && audioEngine_) {
            audioEngine_->stopGuideAnnouncement();
        }
    };
    settingsView_->onLibraryImported = [this]() {
        suppressSongAnnouncement_ = true;
        lastAnnouncedSongKey_.clear();
        if (controlSurface_) controlSurface_->reloadFromDatabase();
        const std::string restoredAnnounce = db_->getAppSetting("audio.announce_song_name", "1");
        announceSongName_ = (restoredAnnounce != "0" && restoredAnnounce != "false");
        if (libraryView_) libraryView_->reloadAfterImport();
        if (openSetView_) openSetView_->refreshSetlists();
        if (settingsView_) settingsView_->refreshSettings();
        if (transport_) {
            transport_->setSongInfo("", "", 120, "C", "4/4");
            transport_->syncTracksAvailability(std::nullopt, {}, false, {});
            transport_->syncArrangements({}, std::nullopt, true, 120, "C", "4/4", false, false, 1.0f, 1.0f);
            transport_->syncClickAndGuideChannels(audioEngine_->getClickChannel(), audioEngine_->getGuideChannel());
            if (openSetView_) {
                transport_->setSongNavigationState(openSetView_->canSelectPreviousSong(), openSetView_->canSelectNextSong());
            }
        }
        suppressSongAnnouncement_ = false;
    };
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
    transport_->onPreviousSongClicked = [this]() {
        if (openSetView_) openSetView_->selectPreviousSong();
    };
    transport_->onNextSongClicked = [this]() {
        if (openSetView_) openSetView_->selectNextSong();
    };
    transport_->onTransitionChanged = [this](SongTransitionMode mode, int crossfadeBars) {
        if (activeView_ == ActiveView::OpenSet && openSetView_) {
            openSetView_->setCurrentSongTransition(mode, crossfadeBars);
        }
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
    transport_->onSongClickSubdivisionsChanged = [this](int64_t songId,
                                                        std::optional<int64_t> arrangementId,
                                                        bool eighths,
                                                        bool sixteenths,
                                                        float eighthsGain,
                                                        float sixteenthsGain) {
        if (arrangementId.has_value()) {
            db_->updateArrangementClickSubdivisions(*arrangementId, eighths, sixteenths, eighthsGain, sixteenthsGain);
        } else {
            db_->updateSongClickSubdivisions(songId, eighths, sixteenths, eighthsGain, sixteenthsGain);
        }
        if (openSetView_) {
            openSetView_->syncSongClickSubdivisions(songId, arrangementId, eighths, sixteenths, eighthsGain, sixteenthsGain);
        }
    };
    transport_->onArrangementChanged = [this](std::optional<int64_t> arrangementId) {
        if (activeView_ == ActiveView::OpenSet && openSetView_) {
            openSetView_->selectArrangementForCurrentSong(arrangementId);
        }
    };
    transport_->onSongParamsChanged = [this](std::optional<int64_t> songId, int tempo, const std::string& key, const std::string& timeSignature) {
        audioEngine_->setTempo(tempo);
        audioEngine_->setTimeSignature(timeSignature);
        if (activeView_ == ActiveView::OpenSet && openSetView_) {
            openSetView_->applySongParameters(tempo, key, timeSignature);
        } else if (songId.has_value()) {
            if (auto loaded = db_->loadSong(*songId); loaded.has_value()) {
                loaded->tempo = tempo;
                loaded->key = key;
                loaded->timeSignature = timeSignature;
                db_->saveSong(*loaded);
                if (activeView_ == ActiveView::Library && libraryView_) {
                    libraryView_->refreshSongList();
                }
            }
        }
    };
    transport_->syncClickAndGuideChannels(audioEngine_->getClickChannel(), audioEngine_->getGuideChannel());
    addAndMakeVisible(transport_.get());

    switchView(ActiveView::Library);
    suppressSongAnnouncement_ = false;
    setSize(1080, 720);
}

void MainComponent::playSong(const Song& song) {
    audioEngine_->loadSong(song);
    const std::string songKey = song.id.has_value() ? std::to_string(*song.id) : song.title;
    if (!suppressSongAnnouncement_ && announceSongName_ && !song.title.empty() && songKey != lastAnnouncedSongKey_) {
        audioEngine_->speakGuideText(song.title);
    }
    lastAnnouncedSongKey_ = songKey;
    transport_->setSongInfo(song.title, song.artist, song.tempo, song.key, song.timeSignature);
    transport_->syncTracksAvailability(song.id, song.tracks, audioEngine_->hasLoadedTracks(), song.sections);
    transport_->syncClickSubdivisions(song.id, song.clickEighths, song.clickSixteenths, song.clickEighthsGain, song.clickSixteenthsGain);

    int clickOnlyTempo = song.tempo;
    std::string clickOnlyKey = song.key;
    std::string clickOnlySig = song.timeSignature;
    bool clickOnly8 = song.clickEighths;
    bool clickOnly16 = song.clickSixteenths;
    float clickOnly8Gain = song.clickEighthsGain;
    float clickOnly16Gain = song.clickSixteenthsGain;
    if (activeView_ == ActiveView::OpenSet && openSetView_) {
        if (auto base = openSetView_->getClickOnlyBaselineForCurrentSong(); base.has_value()) {
            clickOnlyTempo = base->tempo;
            clickOnlyKey = base->key;
            clickOnlySig = base->timeSignature;
            clickOnly8 = base->clickEighths;
            clickOnly16 = base->clickSixteenths;
            clickOnly8Gain = base->clickEighthsGain;
            clickOnly16Gain = base->clickSixteenthsGain;
        }
    }
    transport_->syncArrangements(song.arrangements,
                                 song.activeArrangementId,
                                 song.justClickMode,
                                 clickOnlyTempo,
                                 clickOnlyKey,
                                 clickOnlySig,
                                 clickOnly8,
                                 clickOnly16,
                                 clickOnly8Gain,
                                 clickOnly16Gain);

    if (activeView_ == ActiveView::OpenSet && openSetView_) {
        const auto mode = openSetView_->getCurrentSongTransitionMode();
        const int xfBars = openSetView_->getCurrentSongCrossfadeBars();
        transport_->setTransitionConfig(mode, xfBars);
        audioEngine_->setTransitionConfig(mode, xfBars, openSetView_->canSelectNextSong());
    } else {
        transport_->setTransitionConfig(SongTransitionMode::StayInCurrentSong, 1);
        audioEngine_->setTransitionConfig(SongTransitionMode::StayInCurrentSong, 1, false);
    }
}

void MainComponent::switchView(ActiveView view) {
    if (activeView_ == ActiveView::Library && view != ActiveView::Library && libraryView_) {
        libraryView_->flushPendingAutoSave();
    }
    activeView_ = view;

    libraryView_->setVisible(activeView_ == ActiveView::Library);
    openSetView_->setVisible(activeView_ == ActiveView::OpenSet);
    settingsView_->setVisible(activeView_ == ActiveView::Settings);

    if (activeView_ == ActiveView::Library) {
        libraryView_->refreshSongList();
        audioEngine_->setTransitionConfig(SongTransitionMode::StayInCurrentSong, 1, false);
    } else if (activeView_ == ActiveView::OpenSet) {
        transport_->syncClickAndGuideChannels(audioEngine_->getClickChannel(), audioEngine_->getGuideChannel());
        openSetView_->refreshSetlists();
        transport_->setSongNavigationState(openSetView_->canSelectPreviousSong(), openSetView_->canSelectNextSong());
        const auto mode = openSetView_->getCurrentSongTransitionMode();
        const int xfBars = openSetView_->getCurrentSongCrossfadeBars();
        transport_->setTransitionConfig(mode, xfBars);
        audioEngine_->setTransitionConfig(mode, xfBars, openSetView_->canSelectNextSong());
    } else if (activeView_ == ActiveView::Settings) {
        settingsView_->refreshSettings();
    }

    if (activeView_ != ActiveView::Settings && controlSurface_) {
        controlSurface_->cancelMidiLearn();
        controlSurface_->cancelKeyLearn();
    }

    updateNavButtonStyles();
    resized();
}

MainComponent::~MainComponent() {
    removeKeyListener(this);
}

void MainComponent::executeControlAction(ControlAction action) {
    if (!audioEngine_) return;
    switch (action) {
        case ControlAction::PlayPause:
            if (!audioEngine_->isPlaying()) {
                audioEngine_->play();
            } else if (audioEngine_->isPaused()) {
                audioEngine_->resume();
            } else {
                audioEngine_->pause();
            }
            break;
        case ControlAction::Stop:
            audioEngine_->stop();
            break;
        case ControlAction::FadeOutStop:
            audioEngine_->fadeOutAndStop(4.0);
            break;
        case ControlAction::RepeatSection:
            if (audioEngine_->isStructuredTimelineMode()) {
                audioEngine_->toggleRepeatCurrentSection();
            } else {
                audioEngine_->toggleClickLoop();
            }
            break;
        case ControlAction::SkipBackSection:
            audioEngine_->skipSection(-1);
            break;
        case ControlAction::SkipForwardSection:
            audioEngine_->skipSection(1);
            break;
        case ControlAction::PreviousSong:
            if (activeView_ == ActiveView::OpenSet && openSetView_) {
                openSetView_->selectPreviousSong();
            }
            break;
        case ControlAction::NextSong:
            if (activeView_ == ActiveView::OpenSet && openSetView_) {
                openSetView_->selectNextSong();
            }
            break;
        case ControlAction::Count:
        default:
            break;
    }
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*) {
    if (!controlSurface_) {
        return false;
    }

    // Key Learn mode: capture ANY key (including Esc) to assign; user clicks Cancel button to abort
    if (auto learningKey = controlSurface_->getLearningKeyAction(); learningKey.has_value()) {
        if (auto kb = KeyBinding::fromKeyPress(key); kb.has_value()) {
            controlSurface_->setKeyBinding(*learningKey, *kb);
            if (settingsView_) settingsView_->refreshSettings();
            return true;
        }
        return false;
    }

    if (controlSurface_->getLearningAction().has_value()) {
        return false;
    }

    if (!controlSurface_->isKeyboardShortcutsEnabled()) {
        return false;
    }
    if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0) {
        return false;
    }
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent()) {
        if (dynamic_cast<juce::TextEditor*>(focused) != nullptr) {
            return false;
        }
    }

    const bool allowSongNav = (activeView_ == ActiveView::OpenSet);
    if (auto matched = controlSurface_->matchKeyPress(key, allowSongNav); matched.has_value()) {
        executeControlAction(*matched);
        return true;
    }
    return false;
}

void MainComponent::updateNavButtonStyles() {
    if (navLibraryButton_) navLibraryButton_->setActive(activeView_ == ActiveView::Library);
    if (navOpenSetButton_) navOpenSetButton_->setActive(activeView_ == ActiveView::OpenSet);
    if (navSettingsButton_) navSettingsButton_->setActive(activeView_ == ActiveView::Settings);
    repaint();
}

void MainComponent::paint(juce::Graphics& g) {
    const NavTabButton* activeTab = navLibraryButton_.get();
    if (activeView_ == ActiveView::OpenSet) activeTab = navOpenSetButton_.get();
    else if (activeView_ == ActiveView::Settings) activeTab = navSettingsButton_.get();

    const juce::Colour activePageBg = activeTab ? activeTab->getPageBackgroundColour() : AppTheme::bgDark;
    const juce::Colour activeAccent = activeTab ? activeTab->getAccentColour() : AppTheme::primaryBlue;

    // Fill page region with active page color and top header strip with recessed dark tone
    g.fillAll(activePageBg);
    g.setColour(AppTheme::headerBg);
    g.fillRect(0, 0, getWidth(), 60);

    if (brandIcon_.isValid() && !brandIconBounds_.isEmpty()) {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(brandIcon_, brandIconBounds_.toFloat(), juce::RectanglePlacement::centred);
    }

    // Horizontal seam below header bar, broken under the active tab so the tab attaches seamlessly to the page
    g.setColour(AppTheme::borderCol.interpolatedWith(activeAccent, 0.45f));
    const float seamY = 59.5f;
    if (activeTab && !activeTab->getBounds().isEmpty()) {
        const float tabLeft = static_cast<float>(activeTab->getX()) + 0.5f;
        const float tabRight = static_cast<float>(activeTab->getRight()) - 0.5f;
        if (tabLeft > 0.0f) {
            g.drawLine(0.0f, seamY, tabLeft, seamY, 1.0f);
        }
        if (tabRight < static_cast<float>(getWidth())) {
            g.drawLine(tabRight, seamY, static_cast<float>(getWidth()), seamY, 1.0f);
        }
    } else {
        g.drawLine(0.0f, seamY, static_cast<float>(getWidth()), seamY, 1.0f);
    }
}

void MainComponent::resized() {
    auto area = getLocalBounds();

    // Top Navigation Bar (height 60 to accommodate large borderless brand logo & attached tabs cleanly)
    auto headerArea = area.removeFromTop(60);
    auto topBar = headerArea.reduced(12, 4);
    auto navHeight = [](juce::Rectangle<int> slot) {
        return slot.withSizeKeepingCentre(slot.getWidth(), 32);
    };

    relaunchButton_.setBounds(navHeight(topBar.removeFromRight(85)));
    topBar.removeFromRight(12);

    brandIconBounds_ = topBar.removeFromLeft(52);
    topBar.removeFromLeft(8);
    brandLabel_.setBounds(navHeight(topBar.removeFromLeft(90)));
    topBar.removeFromLeft(16);

    // Attached tabs sit flush against the bottom of the 60px header bar
    const int tabH = 42;
    const int tabY = headerArea.getBottom() - tabH;
    const int tabW = 108;
    const int tabGap = 4;
    int tabX = topBar.getX();

    if (navLibraryButton_) {
        navLibraryButton_->setBounds(tabX, tabY, tabW, tabH);
        tabX += tabW + tabGap;
    }
    if (navOpenSetButton_) {
        navOpenSetButton_->setBounds(tabX, tabY, tabW, tabH);
        tabX += tabW + tabGap;
    }
    if (navSettingsButton_) {
        navSettingsButton_->setBounds(tabX, tabY, tabW, tabH);
    }

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
