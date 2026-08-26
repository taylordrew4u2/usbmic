#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
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

class MultiMicAggregatorApplication : public juce::JUCEApplication
{
public:
    MultiMicAggregatorApplication() = default;

    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        application = std::make_unique<Application>();
        application->initialise();
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *application);
    }

    void shutdown() override
    {
        mainWindow.reset();

        if (application != nullptr)
            application->shutdown();

        application.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<Application> application;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace mma

START_JUCE_APPLICATION (mma::MultiMicAggregatorApplication)
