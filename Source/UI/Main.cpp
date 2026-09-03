#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "AppLookAndFeel.h"
#include "../App/Application.h"

namespace mma {

/// §6.6: a laptop sleeping mid-take is a total failure, so the recording
/// lifetime holds a sleep/screensaver inhibitor for its whole duration.
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, Application& app)
        : juce::DocumentWindow (name,
                                juce::Colour (0xff16110f), // §9.2 background
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (app), true);
        setResizable (true, false);
        // Small enough for a laptop half-screen, never small enough to crush
        // the meters into unreadability.
        setResizeLimits (560, 420, 4096, 4096);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

class SobStageApplication : public juce::JUCEApplication
{
public:
    SobStageApplication() = default;

    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override
    {
        // From CMake's project() version, not typed again here. The literal
        // this replaces said 0.1.0 on every build from v0.1.0 to v0.9.2, so
        // the one place a user could read the version was wrong for nine
        // releases -- and "which build am I running" is the first question
        // asked when a change appears not to have arrived.
       #if defined (SOBSTAGE_VERSION_STRING)
        return SOBSTAGE_VERSION_STRING;
       #else
        return "dev";
       #endif
    }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        // §11: the diagnostics bundle promises a log, so one has to exist
        // before the first thing that can go wrong. The path comes from
        // Application so the bundle and the logger cannot disagree about where
        // it is, and it is capped so a bundle a user emails stays small.
        constexpr int kMaxLogBytes = 256 * 1024;

        logger = std::make_unique<juce::FileLogger> (
            Application::getLogFile(),
            "SobStage " + getApplicationVersion(),
            kMaxLogBytes);

        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("Starting up.");

        application = std::make_unique<Application>();
        // Installed before any window exists, so every control is painted in the
        // §9.2 palette from its first frame rather than flashing JUCE's default
        // blue and being recoloured afterwards.
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        application->initialise();
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *application);
    }

    void shutdown() override
    {
        mainWindow.reset();

        // Cleared before the look-and-feel goes out of scope: JUCE asserts if a
        // component outlives the look-and-feel it points at.
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

        if (application != nullptr)
            application->shutdown();

        application.reset();

        // Cleared before the logger is destroyed: anything that logs during
        // teardown would otherwise write through a dangling pointer.
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<Application> application;
    // Declared before mainWindow, so it is destroyed after it. Members are
    // destroyed in reverse declaration order, and JUCE asserts if a component
    // outlives the look-and-feel it points at.
    AppLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace mma

START_JUCE_APPLICATION (mma::SobStageApplication)
