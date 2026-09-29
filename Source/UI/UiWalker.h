#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"
#include "MainScreen.h"
#include "AdvancedPanel.h"
#include "CameraPanel.h"
#include "HelpPanel.h"
#include "SaveLocationPrompt.h"
#include "SavedTakePanel.h"
#include "RecoveredTakesPanel.h"
#include "TakeAlertCard.h"
#include "ChannelMeterComponent.h"
#include "../App/Application.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <type_traits>

namespace mma {

/// Test builds only. Drives the real app through every screen the way a
/// person would -- Settings, Help, Cameras, every picker, tick box and slider,
/// a renamed microphone, a real take, the saved-take card, diagnostics export
/// -- by walking the live component tree rather than clicking fixed pixels, so
/// a moved button is still found and a new one is still pressed.
///
/// Inert unless MMA_UI_WALK_REPORT names a file. Every check is written there
/// as it happens ("PASS ..." / "FAIL ..."), and the last line is
/// "UI-WALK DONE failures=N". Tools/e2e_ui_walk.sh reads it.
///
///   MMA_UI_WALK_MODE=crash      record, write "RECORDING" and wait to be killed
///   MMA_UI_WALK_MODE=fault      record, create MMA_UI_WALK_FAULT_FILE (which
///                               Tools/alsa_readi_shim.cpp watches to kill a
///                               microphone), and walk the mid-take alert card
///   MMA_UI_WALK_EXPECT_RECOVERED=1   the recovery card must appear at launch
class UiWalker : private juce::Timer
{
public:
    UiWalker (juce::Component& mainContent, Application& app)
        : root (mainContent), application (app)
    {
        const char* path = std::getenv ("MMA_UI_WALK_REPORT");
        if (path == nullptr || *path == '\0')
            return;

        report.open (path, std::ios::out | std::ios::trunc);
        const char* mode = std::getenv ("MMA_UI_WALK_MODE");
        crashMode = mode != nullptr && juce::String (mode) == "crash";
        faultMode = mode != nullptr && juce::String (mode) == "fault";
        if (const char* f = std::getenv ("MMA_UI_WALK_FAULT_FILE"))
            faultFile = f;
        const char* recovered = std::getenv ("MMA_UI_WALK_EXPECT_RECOVERED");
        expectRecovered = recovered != nullptr && juce::String (recovered) == "1";

        buildScript();
        startTimer (kTickMs);
    }

    ~UiWalker() override { stopTimer(); }

private:
    static constexpr int kTickMs = 100;

    struct Step
    {
        juce::String what;
        std::function<void()> action;           // run once
        std::function<bool()> until;            // polled until true, or the step fails
        int timeoutMs = 3000;
        bool required = true;                   // a failed optional step is reported, not counted
    };

    juce::Component& root;
    Application& application;
    std::ofstream report;
    std::deque<Step> steps;
    std::optional<Step> current;
    int stepElapsedMs = 0;
    int failures = 0;
    int passes = 0;
    bool crashMode = false;
    bool faultMode = false;
    juce::String faultFile;
    bool expectRecovered = false;
    bool finished = false;
    std::set<juce::String> exercised;
    int diagnosticsZipsBefore = 0;
    juce::String takeFolder;

    // --- reporting -------------------------------------------------------

    void line (const juce::String& text)
    {
        report << text.toStdString() << std::endl;
    }

    void pass (const juce::String& what) { ++passes; line ("PASS  " + what); }
    void fail (const juce::String& what)
    {
        ++failures;
        line ("FAIL  " + what);
        line ("      on screen: " + whatIsUp());
    }

    /// The screens and cards showing, and every enabled button's label, so a
    /// failure says what the walker was looking at.
    juce::String whatIsUp() const
    {
        juce::StringArray parts;
        if (isUp<AdvancedPanel>())       parts.add ("Settings");
        if (isUp<HelpPanel>())           parts.add ("Help");
        if (isUp<CameraPanel>())         parts.add ("Cameras");
        if (isUp<SaveLocationPrompt>())  parts.add ("save-location card");
        if (isUp<SavedTakePanel>())      parts.add ("saved-take card");
        if (isUp<RecoveredTakesPanel>()) parts.add ("recovered-takes card");
        if (isUp<TakeAlertCard>())       parts.add ("mid-take alert card");
        if (find<juce::AlertWindow>() != nullptr) parts.add ("dialog");

        juce::StringArray buttons;
        for (auto* c : showing())
            if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->isEnabled() && b->getButtonText().isNotEmpty())
                buttons.addIfNotAlreadyThere ("\"" + b->getButtonText() + "\"");

        return (parts.isEmpty() ? juce::String ("main screen only") : parts.joinIntoString (", "))
             + "; buttons " + buttons.joinIntoString (" ");
    }

    // --- finding things in the live window --------------------------------

    static void collect (juce::Component& c, std::vector<juce::Component*>& out)
    {
        for (int i = 0; i < c.getNumChildComponents(); ++i)
        {
            auto* child = c.getChildComponent (i);
            if (child->isShowing())
            {
                out.push_back (child);
                collect (*child, out);
            }
        }
    }

    std::vector<juce::Component*> showing() const
    {
        std::vector<juce::Component*> out;
        collect (root, out);

        // Anything modal lives outside the window's content: an AlertWindow.
        auto* modal = juce::ModalComponentManager::getInstance();
        for (int i = 0; i < modal->getNumModalComponents(); ++i)
            if (auto* m = modal->getModalComponent (i); m != nullptr && m->isShowing())
            {
                out.push_back (m);
                collect (*m, out);
            }

        return out;
    }

    template <typename T>
    T* find() const
    {
        for (auto* c : showing())
            if (auto* t = dynamic_cast<T*> (c))
                return t;
        return nullptr;
    }

    template <typename Panel>
    bool isUp() const { return find<Panel>() != nullptr; }

    /// A showing, enabled button with this text, inside a Panel if one is
    /// given (void: anywhere). Several screens have a "Done" or a "Close".
    template <typename Panel = void>
    juce::Button* button (const juce::String& text) const
    {
        for (auto* c : showing())
            if (auto* b = dynamic_cast<juce::Button*> (c))
                if (b->getButtonText() == text && b->isEnabled())
                {
                    if constexpr (std::is_void_v<Panel>)
                        return b;
                    else if (b->findParentComponentOfClass<Panel>() != nullptr)
                        return b;
                }
        return nullptr;
    }

    template <typename Panel = void>
    bool click (const juce::String& text)
    {
        if (auto* b = button<Panel> (text))
        {
            exercised.insert (b->getButtonText());
            b->triggerClick();
            return true;
        }
        return false;
    }

    /// The monitor mute button reads "Mute" or "Unmute" depending on state.
    juce::Button* muteButton() const
    {
        if (auto* b = button<MainScreen> ("Mute"))
            return b;
        return button<MainScreen> ("Unmute");
    }

    bool muted() const
    {
        auto* b = muteButton();
        return b != nullptr && b->getToggleState();
    }

    /// What the headphones actually do, not what the button shows.
    bool busMuted()
    {
        auto* bus = application.getMonitorBus();
        return bus != nullptr && bus->isMuted();
    }

    bool recording() { return application.getRecordingEngine().getState() == RecordingState::Recording; }

    juce::Component* mainComponent() const
    {
        for (auto* c : showing())
            if (dynamic_cast<MainComponent*> (c) != nullptr)
                return c;
        return dynamic_cast<MainComponent*> (&root) != nullptr ? &root : nullptr;
    }

    bool pressKey (int keyCode)
    {
        if (auto* mc = mainComponent())
            return mc->keyPressed (juce::KeyPress (keyCode));
        return false;
    }

    static juce::File desktop() { return juce::File::getSpecialLocation (juce::File::userDesktopDirectory); }

    static int countDiagnosticsZips()
    {
        return desktop().getNumberOfChildFiles (juce::File::findFiles, "SobStage-diagnostics*.zip");
    }

    static bool newestDiagnosticsZipIsReal()
    {
        auto zips = desktop().findChildFiles (juce::File::findFiles, false, "SobStage-diagnostics*.zip");
        if (zips.isEmpty())
            return false;

        std::sort (zips.begin(), zips.end(),
                   [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() < b.getLastModificationTime(); });
        juce::ZipFile zip (zips.getLast());
        return zip.getNumEntries() > 0;
    }

    /// Dismisses whatever card or drawer is up, so the next step starts from
    /// the plain main screen.
    void backToMain()
    {
        for (const auto& text : { "Done", "Not yet", "Keep recording", "Cancel" })
            if (auto* b = button (text))
                b->triggerClick();

        pressKey (juce::KeyPress::escapeKey);
    }

    // --- the script -------------------------------------------------------

    void add (juce::String what, std::function<void()> action, std::function<bool()> until,
              int timeoutMs = 3000, bool required = true)
    {
        steps.push_back ({ std::move (what), std::move (action), std::move (until), timeoutMs, required });
    }

    void check (juce::String what, std::function<bool()> condition, int timeoutMs = 3000)
    {
        add (std::move (what), [] {}, std::move (condition), timeoutMs);
    }

    void settle (int ms)
    {
        auto remaining = std::make_shared<int> (ms);
        add ({}, [] {}, [remaining] { *remaining -= kTickMs; return *remaining <= 0; }, ms + 1000);
    }

    /// Queued at the front, so a step can expand into the controls it finds.
    void insertNext (std::vector<Step> more)
    {
        for (auto it = more.rbegin(); it != more.rend(); ++it)
            steps.push_front (std::move (*it));
    }

    /// Every tick box, picker and slider inside Panel, each moved and put
    /// back, with the app given a moment to act on each change.
    template <typename Panel>
    std::vector<Step> exerciseControlsIn (const juce::String& where)
    {
        std::vector<Step> more;
        auto* panel = find<Panel>();
        if (panel == nullptr)
            return more;

        std::vector<juce::Component*> inside;
        collect (*panel, inside);

        const auto settleStep = [] { return Step { {}, [] {}, [n = std::make_shared<int> (4)] { return --*n <= 0; }, 2000, false }; };

        for (auto* c : inside)
        {
            juce::Component::SafePointer<juce::Component> safe (c);

            // A toggle whose state is set by the app rather than by the click
            // (the monitor mute follows the headphone bus) has its own checks.
            if (auto* t = dynamic_cast<juce::ToggleButton*> (c); t != nullptr && t->isEnabled() && t->getClickingTogglesState())
            {
                const auto name = t->getButtonText();
                const bool before = t->getToggleState();
                more.push_back ({ where + ": tick box \"" + name + "\" turns " + (before ? "off" : "on"),
                                  [safe] { if (auto* b = dynamic_cast<juce::Button*> (safe.getComponent())) b->triggerClick(); },
                                  [safe, before] { auto* b = dynamic_cast<juce::Button*> (safe.getComponent()); return b == nullptr || b->getToggleState() != before; } });
                more.push_back (settleStep());
                more.push_back ({ where + ": tick box \"" + name + "\" goes back",
                                  [safe, before] { if (auto* b = dynamic_cast<juce::Button*> (safe.getComponent())) if (b->getToggleState() != before) b->triggerClick(); },
                                  [safe, before] { auto* b = dynamic_cast<juce::Button*> (safe.getComponent()); return b == nullptr || b->getToggleState() == before; } });
                more.push_back (settleStep());
                exercised.insert ("toggle:" + name);
            }
            else if (auto* combo = dynamic_cast<juce::ComboBox*> (c); combo != nullptr && combo->isEnabled() && combo->getNumItems() > 0)
            {
                const int original = combo->getSelectedItemIndex();
                const auto label = combo->getName().isNotEmpty() ? combo->getName() : combo->getTooltip();

                for (int i = 0; i < combo->getNumItems(); ++i)
                {
                    if (! combo->isItemEnabled (combo->getItemId (i)))
                        continue;

                    const auto item = combo->getItemText (i);
                    more.push_back ({ where + ": picker " + label + " set to \"" + item + "\"",
                                      [safe, i] { if (auto* cb = dynamic_cast<juce::ComboBox*> (safe.getComponent())) cb->setSelectedItemIndex (i, juce::sendNotificationSync); },
                                      [] { return true; } });
                    more.push_back (settleStep());
                }

                more.push_back ({ where + ": picker " + label + " restored",
                                  [safe, original] { if (auto* cb = dynamic_cast<juce::ComboBox*> (safe.getComponent())) cb->setSelectedItemIndex (original, juce::sendNotificationSync); },
                                  [] { return true; } });
                more.push_back (settleStep());
                exercised.insert ("picker:" + label);
            }
            else if (auto* slider = dynamic_cast<juce::Slider*> (c); slider != nullptr && slider->isEnabled())
            {
                const double original = slider->getValue();
                for (double v : { slider->getMinimum(), slider->getMaximum(), original })
                {
                    more.push_back ({ where + ": slider to " + juce::String (v, 1),
                                      [safe, v] { if (auto* s = dynamic_cast<juce::Slider*> (safe.getComponent())) s->setValue (v, juce::sendNotificationSync); },
                                      [safe, v] { auto* s = dynamic_cast<juce::Slider*> (safe.getComponent()); return s == nullptr || std::abs (s->getValue() - v) < 1e-6; } });
                    more.push_back (settleStep());
                }
                exercised.insert ("slider:" + where);
            }
        }

        return more;
    }

    void buildScript()
    {
        line ("UI-WALK START");

        check ("the main screen comes up", [this] { return find<MainScreen>() != nullptr; }, 30000);

        if (expectRecovered)
        {
            check ("the interrupted take from last time is offered back at launch",
                   [this] { return isUp<RecoveredTakesPanel>(); }, 30000);
            add ("the recovery card's Done closes it",
                 [this] { click<RecoveredTakesPanel> ("Done"); },
                 [this] { return ! isUp<RecoveredTakesPanel>(); });
        }

        check ("the microphones are found and shown", [this]
        {
            auto* ms = find<MainScreen>();
            return ms != nullptr && ms->getMicCount() > 0;
        }, 30000);

        // A leftover card from an earlier run must not swallow every click.
        add ("nothing is covering the main screen", [this] { backToMain(); },
             [this] { return ! isUp<RecoveredTakesPanel>() && ! isUp<SavedTakePanel>(); }, 5000, false);

        if (faultMode)
        {
            add ("recording starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            settle (2000);
            add ("a microphone dies mid-take", [this]
            {
                takeFolder = application.getCurrentSessionFolder();
                juce::File (faultFile).replaceWithText ("dead");
            }, [this] { return juce::File (faultFile).existsAsFile(); });
            check ("the mid-take alert card says so", [this] { return isUp<TakeAlertCard>(); }, 30000);
            add ("Keep recording closes the card and the take carries on", [this] { click<TakeAlertCard> ("Keep recording"); },
                 [this] { return ! isUp<TakeAlertCard>() && recording(); });
            settle (2000);
            add ("the take still stops normally", [this]
            {
                backToMain();
                click<MainScreen> ("Recording. Tap to stop.");
            }, [this] { return ! recording(); }, 20000);
            check ("the saved-take card comes up", [this] { return isUp<SavedTakePanel>(); }, 30000);
            add ("the take's files are on disk", [] {}, [this]
            {
                const juce::File folder (takeFolder);
                return folder.getChildFile ("MIX.wav").existsAsFile() && folder.getChildFile ("session.json").existsAsFile();
            }, 10000);
            add ("the take's record says which microphone stopped, and when", [] {}, [this]
            {
                return juce::File (takeFolder).getChildFile ("session.json").loadFileAsString()
                           .contains ("Microphone stopped sending audio");
            }, 10000);
            add ("the saved-take card's Done closes it", [this] { click<SavedTakePanel> ("Done"); },
                 [this] { return ! isUp<SavedTakePanel>(); });
            return;
        }

        if (crashMode)
        {
            add ("recording starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
            add ("the take has a folder", [] {}, [this] { return application.getCurrentSessionFolder().isNotEmpty(); }, 10000);
            add ("the take is running", [this] { line ("RECORDING " + application.getCurrentSessionFolder()); },
                 [] { return false; }, 600000);
            return;
        }

        // Main screen controls.
        add ("the monitor mute button mutes", [this] { if (auto* b = muteButton()) b->triggerClick(); },
             [this] { return muted() && busMuted(); });
        add ("and unmutes", [this] { if (auto* b = muteButton()) b->triggerClick(); },
             [this] { return ! muted() && ! busMuted(); });
        add ("the space bar mutes the monitor", [this] { pressKey (juce::KeyPress::spaceKey); },
             [this] { return muted() && busMuted(); });
        add ("and space again unmutes it", [this] { pressKey (juce::KeyPress::spaceKey); },
             [this] { return ! muted() && ! busMuted(); });
        add ("the main screen's controls", [this] { insertNext (exerciseControlsIn<MainScreen> ("main screen")); }, [] { return true; });

        add ("the session name can be typed", [this]
        {
            if (auto* editor = find<juce::TextEditor>())
            {
                editor->setText ("UI walk", true);
                editor->grabKeyboardFocus();
            }
        }, [this] { auto* ms = find<MainScreen>(); return ms != nullptr && ms->getSessionName() == "UI walk"; });

        // Rename microphone 1, and check the name reaches its file later.
        add ("clicking a microphone's name asks for a new one", [this]
        {
            if (auto* ms = find<MainScreen>())
                if (auto* meter = ms->getChannelMeter (0); meter != nullptr && meter->onNameClicked)
                    meter->onNameClicked();
        }, [this] { return find<juce::AlertWindow>() != nullptr; });
        add ("typing a name and pressing Save renames it", [this]
        {
            if (auto* alert = find<juce::AlertWindow>())
            {
                if (auto* editor = alert->getTextEditor ("name"))
                    editor->setText ("Walker Vox");
                click<juce::AlertWindow> ("Save");
            }
        }, [this] { return find<juce::AlertWindow>() == nullptr && application.getMicDisplayName (0).contains ("Walker"); });

        // Settings drawer.
        add ("Settings opens", [this] { click<MainScreen> ("Settings"); }, [this] { return isUp<AdvancedPanel>(); });
        add ("every control in Settings", [this] { insertNext (exerciseControlsIn<AdvancedPanel> ("Settings")); }, [] { return true; });
        add ("Export diagnostics in Settings writes a zip", [this]
        {
            diagnosticsZipsBefore = countDiagnosticsZips();
            click<AdvancedPanel> ("Export diagnostics");
        }, [this] { return countDiagnosticsZips() > diagnosticsZipsBefore && newestDiagnosticsZipIsReal(); }, 20000);
        add ("Help opens from Settings", [this] { click<AdvancedPanel> ("Help"); }, [this] { return isUp<HelpPanel>(); });
        add ("Help's Open Settings goes back to Settings", [this] { click<HelpPanel> ("Open Settings"); },
             [this] { return isUp<AdvancedPanel>(); });
        add ("Settings' Close closes it", [this] { click<AdvancedPanel> ("Close"); },
             [this] { return ! isUp<AdvancedPanel>(); });
        add ("Escape closes Settings too", [this] { click<MainScreen> ("Settings"); },
             [this] { return isUp<AdvancedPanel>(); });
        add ("  (Escape)", [this] { pressKey (juce::KeyPress::escapeKey); }, [this] { return ! isUp<AdvancedPanel>(); });

        // Help drawer.
        add ("Help opens from the main screen", [this] { click<MainScreen> ("Help"); }, [this] { return isUp<HelpPanel>(); });
        add ("Export diagnostics in Help writes another zip", [this]
        {
            diagnosticsZipsBefore = countDiagnosticsZips();
            click<HelpPanel> ("Export diagnostics");
        }, [this] { return countDiagnosticsZips() > diagnosticsZipsBefore; }, 20000);
        add ("Help's Close closes it", [this] { click<HelpPanel> ("Close"); }, [this] { return ! isUp<HelpPanel>(); });

        // Cameras.
        add ("Cameras opens", [this] { click<MainScreen> ("Cameras"); }, [this] { return isUp<CameraPanel>(); });
        add ("every control on the Cameras screen", [this] { insertNext (exerciseControlsIn<CameraPanel> ("Cameras")); }, [] { return true; });
        add ("Cameras' Done goes back", [this] { click<CameraPanel> ("< Done"); }, [this] { return ! isUp<CameraPanel>(); });

        // A take.
        add ("recording starts", [this] { startRecording(); }, [this] { return recordingOrAnswerPrompt(); }, 90000);
        add ("the button says it is recording", [] {}, [this]
        {
            return button<MainScreen> ("Recording. Tap to stop.") != nullptr;
        });
        settle (1500);
        add ("Settings opens during a take", [this] { click<MainScreen> ("Settings"); }, [this] { return isUp<AdvancedPanel>(); });
        add ("changing the buffer size during a take", [this]
        {
            // §5.4: a mid-take buffer change restarts capture. The take must
            // survive it.
            if (auto* panel = find<AdvancedPanel>())
            {
                std::vector<juce::Component*> inside;
                collect (*panel, inside);
                for (auto* c : inside)
                    if (auto* cb = dynamic_cast<juce::ComboBox*> (c); cb != nullptr && cb->getText().containsIgnoreCase ("sample")
                                                                       && cb->isEnabled() && cb->getNumItems() > 1)
                    {
                        bufferOverrideBefore = application.getBufferSizeOverride();
                        cb->setSelectedItemIndex ((cb->getSelectedItemIndex() + 1) % cb->getNumItems(), juce::sendNotificationSync);
                        break;
                    }
            }
        }, [this]
        {
            // The picker must have been found, and the choice must have landed.
            return recording() && bufferOverrideBefore != -12345
                && application.getBufferSizeOverride() != bufferOverrideBefore;
        });
        settle (2000);
        add ("the take is still running after it", [] {}, [this] { return recording(); });
        add ("Settings closes during a take", [this] { pressKey (juce::KeyPress::escapeKey); },
             [this] { return ! isUp<AdvancedPanel>(); });
        settle (2000);
        add ("recording stops", [this]
        {
            takeFolder = application.getCurrentSessionFolder();
            click<MainScreen> ("Recording. Tap to stop.");
        }, [this] { return ! recording(); }, 20000);
        check ("the saved-take card says the take is saved", [this] { return isUp<SavedTakePanel>(); }, 30000);
        add ("the take's files are on disk, with the renamed microphone", [] {}, [this] { return takeFilesLookRight(); }, 10000);
        add ("the saved-take card's Done closes it", [this] { click<SavedTakePanel> ("Done"); },
             [this] { return ! isUp<SavedTakePanel>(); });

        // Anything with a label nobody above pressed: press it, then clear up.
        add ("every other button on the main screen", [this] { insertNext (pressEverythingElse()); }, [] { return true; });

        // A choice made in Settings is on disk.
        add ("a Settings choice is saved to disk", [] {}, [] {
            return Application::getSettingsFile().existsAsFile() && Application::getSettingsFile().getSize() > 0;
        }, 10000);
    }

    void startRecording()
    {
        backToMain();
        click<MainScreen> ("Start recording");
    }

    /// Polled while waiting for a take to start: a fresh profile asks where
    /// recordings go first, and that card can take a moment to appear.
    bool recordingOrAnswerPrompt()
    {
        if (recording())
            return true;

        if (isUp<SaveLocationPrompt>())
        {
            click<SaveLocationPrompt> ("Start recording");
            return false;
        }

        // At launch the button stays disabled while each save location is
        // checked, which can take half a minute. Keep pressing once it is ready.
        if (++ticksSinceRecordPress >= 20)
        {
            ticksSinceRecordPress = 0;
            click<MainScreen> ("Start recording");
        }

        return false;
    }

    int ticksSinceRecordPress = 0;
    int bufferOverrideBefore = -12345;

    bool takeFilesLookRight()
    {
        if (takeFolder.isEmpty())
            return false;

        const juce::File folder (takeFolder);
        const auto wavs = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
        bool hasMix = false, hasRenamed = false;

        for (const auto& w : wavs)
        {
            hasMix = hasMix || w.getFileName() == "MIX.wav";
            hasRenamed = hasRenamed || w.getFileName().contains ("Walker");
        }

        return hasMix && hasRenamed && wavs.size() >= 2
            && folder.getChildFile ("session.json").existsAsFile();
    }

    std::vector<Step> pressEverythingElse()
    {
        // Buttons that hand off to the operating system -- a folder picker, a
        // file manager window -- have nothing in this process to verify.
        static const std::set<juce::String> leaveAlone {
            "Change...", "Choose a different folder...", "Open the folder",
            "Start recording", "Recording. Tap to stop."
        };

        std::vector<Step> more;
        for (auto* c : showing())
            if (auto* b = dynamic_cast<juce::Button*> (c))
            {
                const auto text = b->getButtonText();
                if (text.isEmpty() || leaveAlone.count (text) != 0 || exercised.count (text) != 0
                    || dynamic_cast<juce::ToggleButton*> (b) != nullptr || ! b->isEnabled())
                    continue;

                exercised.insert (text);
                juce::Component::SafePointer<juce::Component> safe (b);
                more.push_back ({ "\"" + text + "\" does not break anything",
                                  [safe] { if (auto* btn = dynamic_cast<juce::Button*> (safe.getComponent())) btn->triggerClick(); },
                                  [this] { return find<MainScreen>() != nullptr || isUp<CameraPanel>(); } });
                more.push_back ({ {}, [this] { backToMain(); }, [n = std::make_shared<int> (5)] { return --*n <= 0; }, 2000, false });
            }

        return more;
    }

    // --- the loop ---------------------------------------------------------

    void timerCallback() override
    {
        if (finished)
            return;

        if (! current.has_value())
        {
            if (steps.empty())
            {
                finish();
                return;
            }

            // Taken off the queue before it runs, so steps it adds with
            // insertNext() come straight after it rather than ahead of it.
            current = std::move (steps.front());
            steps.pop_front();
            stepElapsedMs = 0;
            current->action();
        }

        stepElapsedMs += kTickMs;

        if (current->until())
        {
            if (current->what.isNotEmpty())
                pass (current->what);
            current.reset();
        }
        else if (stepElapsedMs >= current->timeoutMs)
        {
            if (current->what.isNotEmpty())
            {
                if (current->required)
                    fail (current->what);
                else
                    line ("SKIP  " + current->what);
            }
            current.reset();
        }
    }

    void finish()
    {
        finished = true;
        stopTimer();
        line ("controls exercised: " + juce::String (static_cast<int> (exercised.size())));
        line ("UI-WALK DONE passes=" + juce::String (passes) + " failures=" + juce::String (failures));

        // Quit the way the window's close button does, so shutdown is walked too.
        juce::MessageManager::callAsync ([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UiWalker)
};

} // namespace mma
