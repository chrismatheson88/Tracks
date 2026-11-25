#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>
#include "MainComponent.h"
#include "Common/AppTheme.h"
#include "Common/MacDockIcon.h"

namespace tracks {

class TracksApplication : public juce::JUCEApplication {
public:
    TracksApplication() = default;

    const juce::String getApplicationName() override       { return "Tracks"; }
    const juce::String getApplicationVersion() override    { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise(const juce::String& /*commandLine*/) override {
        setMacDockIconFromPngData(BinaryData::TracksIcon_png, BinaryData::TracksIcon_pngSize);
        mainWindow_ = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override {
        mainWindow_ = nullptr;
    }

    void systemRequestedQuit() override {
        quit();
    }

    void anotherInstanceStarted(const juce::String& /*commandLine*/) override {}

    class MainWindow : public juce::DocumentWindow {
    public:
        explicit MainWindow(juce::String name)
            : DocumentWindow(name,
                             AppTheme::bgDark,
                             DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);

            setResizable(true, true);
            setResizeLimits(960, 600, 3840, 2160);
            centreWithSize(getWidth(), getHeight());

            setVisible(true);
        }

        void closeButtonPressed() override {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace tracks

START_JUCE_APPLICATION(tracks::TracksApplication)
