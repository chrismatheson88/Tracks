#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Common/AppTheme.h"
#include "Database/DatabaseService.h"
#include "Audio/AudioEngine.h"
#include "UI/Widgets/TransportComponent.h"
#include "UI/Views/LibraryView.h"
#include "UI/Views/OpenSetView.h"
#include "UI/Views/SettingsView.h"
#include <memory>

namespace tracks {

class MainComponent : public juce::Component {
public:
    MainComponent();
    ~MainComponent() override = default;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    enum class ActiveView {
        Library,
        OpenSet,
        Settings
    };

    AppTheme theme_;
    std::unique_ptr<DatabaseService> db_;
    std::unique_ptr<AudioEngine> audioEngine_;

    // Top Navigation Bar
    juce::Label brandLabel_{{}, "TRACKS"};
    juce::TextButton navLibraryButton_{"Library"};
    juce::TextButton navOpenSetButton_{"Open Set"};
    juce::TextButton navSettingsButton_{"Settings"};

    // Views
    std::unique_ptr<LibraryView> libraryView_;
    std::unique_ptr<OpenSetView> openSetView_;
    std::unique_ptr<SettingsView> settingsView_;
    ActiveView activeView_ = ActiveView::Library;

    // Bottom Transport Bar
    std::unique_ptr<TransportComponent> transport_;

    void switchView(ActiveView view);
    void updateNavButtonStyles();
    void playSong(const Song& song);
};

} // namespace tracks
