#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Common/AppTheme.h"
#include "Database/DatabaseService.h"
#include "Audio/AudioEngine.h"
#include "Services/ControlSurfaceService.h"
#include "UI/Widgets/TransportComponent.h"
#include "UI/Views/LibraryView.h"
#include "UI/Views/OpenSetView.h"
#include "UI/Views/SettingsView.h"
#include <memory>

namespace tracks {

class MainComponent : public juce::Component,
                      private juce::KeyListener {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class NavTabButton;

    enum class ActiveView {
        Library,
        OpenSet,
        Settings
    };

    AppTheme theme_;
    std::unique_ptr<DatabaseService> db_;
    std::unique_ptr<AudioEngine> audioEngine_;
    std::unique_ptr<ControlSurfaceService> controlSurface_;

    // Top Navigation Bar
    juce::Image brandIcon_;
    juce::Rectangle<int> brandIconBounds_;
    juce::Label brandLabel_{{}, "TRACKS"};
    std::unique_ptr<NavTabButton> navLibraryButton_;
    std::unique_ptr<NavTabButton> navOpenSetButton_;
    std::unique_ptr<NavTabButton> navSettingsButton_;
    juce::TextButton relaunchButton_{"Relaunch"};

    // Views
    std::unique_ptr<LibraryView> libraryView_;
    std::unique_ptr<OpenSetView> openSetView_;
    std::unique_ptr<SettingsView> settingsView_;
    ActiveView activeView_ = ActiveView::Library;

    // Bottom Transport Bar
    std::unique_ptr<TransportComponent> transport_;

    bool announceSongName_ = true;
    bool suppressSongAnnouncement_ = true;
    std::string lastAnnouncedSongKey_;

    void switchView(ActiveView view);
    void updateNavButtonStyles();
    void playSong(const Song& song);
    void executeControlAction(ControlAction action);
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;
    void relaunchApp();
};

} // namespace tracks
