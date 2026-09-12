// Lays out the real MainScreen with camera tiles and reports what the picture
// actually comes out as. No display needed: Component layout is arithmetic.
#include "UI/MainScreen.h"
#include "UI/CameraPanel.h"
#include "UI/ModalCard.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

class FocusProbeCard final : public mma::ModalCard
{
public:
    FocusProbeCard()
    {
        first.setWantsKeyboardFocus (true);
        second.setWantsKeyboardFocus (true);
        addAndMakeVisible (first);
        addAndMakeVisible (second);
    }

    juce::TextButton first { "First" };
    juce::TextButton second { "Second" };

private:
    int getContentHeight() const override { return 40; }

    void layOutContent (juce::Rectangle<int> area) override
    {
        first.setBounds (area.removeFromLeft (100));
        second.setBounds (area.removeFromLeft (100));
    }
};

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int failures = 0;

    std::printf ("-- modal keyboard focus containment --\n");

    juce::Component window;
    juce::TextButton controlBehindCard { "Record" };
    FocusProbeCard card;
    window.addAndMakeVisible (controlBehindCard);
    window.addAndMakeVisible (card);

    auto* focusRoot = card.first.findKeyboardFocusContainer();
    const auto focusContainerIsCard = focusRoot == &card;
    auto traverser = card.first.createKeyboardFocusTraverser();
    const auto focusable = focusRoot != nullptr
                         ? traverser->getAllComponents (focusRoot)
                         : std::vector<juce::Component*> {};
    const auto contains = [&focusable] (juce::Component* component)
    {
        return std::find (focusable.begin(), focusable.end(), component) != focusable.end();
    };
    const auto traversalStaysOnCard = contains (&card.first) && contains (&card.second)
                                   && ! contains (&controlBehindCard);

    std::printf ("focus container: %s\n", focusContainerIsCard ? "PASS" : "FAIL");
    std::printf ("focus traversal stays on card: %s\n\n",
                 traversalStaysOnCard ? "PASS" : "FAIL");
    failures += focusContainerIsCard ? 0 : 1;
    failures += traversalStaysOnCard ? 0 : 1;

    std::printf ("-- skull-meter keyboard button semantics --\n");

    mma::SkullMeterComponent skull;
    int primaryActions = 0;
    skull.onNameClicked = [&primaryActions] { ++primaryActions; };
    const bool spaceConsumed = skull.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
    const bool spaceActivated = spaceConsumed && primaryActions == 1;
    const bool returnConsumed = skull.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    const bool returnActivated = returnConsumed && primaryActions == 2;
    const bool hintNamesBothKeys = skull.getDescription().containsIgnoreCase ("Return or Space");

    std::printf ("Space activates focused meter: %s\n", spaceActivated ? "PASS" : "FAIL");
    std::printf ("Return still activates focused meter: %s\n", returnActivated ? "PASS" : "FAIL");
    std::printf ("accessible hint names both keys: %s\n\n", hintNamesBothKeys ? "PASS" : "FAIL");
    failures += spaceActivated ? 0 : 1;
    failures += returnActivated ? 0 : 1;
    failures += hintNamesBothKeys ? 0 : 1;

    std::printf ("-- camera viewer revision invalidates UI caches --\n");

    int mainViewerCreates = 0;
    mma::MainScreen cameraScreen;
    cameraScreen.makeViewer = [&mainViewerCreates] (const std::string&)
    {
        ++mainViewerCreates;
        return std::make_unique<juce::Component>();
    };
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 1 } });
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 1 } });
    const bool mainCacheKeepsViewer = mainViewerCreates == 1;
    cameraScreen.setCameraTiles ({ { "capture", "HDMI", 2 } });
    const bool mainRevisionRebuildsViewer = mainViewerCreates == 2;

    const auto hasExactLabel = [&cameraScreen] (const juce::String& text)
    {
        for (int i = 0; i < cameraScreen.getNumChildComponents(); ++i)
            if (auto* label = dynamic_cast<juce::Label*> (cameraScreen.getChildComponent (i));
                label != nullptr && label->getText() == text)
                return true;

        return false;
    };

    cameraScreen.setRecording (true);
    const bool recCaptionIsOutsideNativePreview = hasExactLabel ("REC: HDMI");
    cameraScreen.setRecording (false);
    const bool stoppedCaptionDropsRec = hasExactLabel ("HDMI")
                                     && ! hasExactLabel ("REC: HDMI");

    int panelViewerCreates = 0;
    mma::CameraPanel cameraPanel;
    cameraPanel.makeViewer = [&panelViewerCreates] (const std::string&)
    {
        ++panelViewerCreates;
        return std::make_unique<juce::Component>();
    };
    cameraPanel.setCameras ({ { "capture", "HDMI", true, true, false, "HDMI.mov", 1 } });
    cameraPanel.setCameras ({ { "capture", "HDMI", true, true, false, "HDMI.mov", 1 } });
    const bool panelCacheKeepsViewer = panelViewerCreates == 1;
    cameraPanel.setCameras ({ { "capture", "HDMI", true, true, false, "HDMI.mov", 2 } });
    const bool panelRevisionRebuildsViewer = panelViewerCreates == 2;

    auto cameraConfigurationHasState = [&cameraPanel] (bool expectedEnabled)
    {
        bool foundRecordToggle = false;
        bool foundNameEditor = false;
        bool allMatch = true;

        for (int i = 0; i < cameraPanel.getNumChildComponents(); ++i)
        {
            auto* child = cameraPanel.getChildComponent (i);
            if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child);
                toggle != nullptr && toggle->getButtonText() == "Record this camera")
            {
                foundRecordToggle = true;
                allMatch = allMatch && toggle->isEnabled() == expectedEnabled;
            }

            if (auto* editor = dynamic_cast<juce::TextEditor*> (child))
            {
                foundNameEditor = true;
                allMatch = allMatch && editor->isEnabled() == expectedEnabled;
            }
        }

        return foundRecordToggle && foundNameEditor && allMatch;
    };

    cameraPanel.setRecording (true);
    const bool takeFreezesCameraControls = cameraConfigurationHasState (false);
    cameraPanel.setRecording (false);
    const bool stopRestoresCameraControls = cameraConfigurationHasState (true);

    mma::CameraPanel takeUnavailablePanel;
    takeUnavailablePanel.setRecording (true);
    takeUnavailablePanel.setCameras (
        { { "late", "Late HDMI", true, false, false, "V01_Late-HDMI.mov", 1 } });
    juce::String inTakeUnavailableCopy;
    for (int i = 0; i < takeUnavailablePanel.getNumChildComponents(); ++i)
        if (auto* label = dynamic_cast<juce::Label*> (
                takeUnavailablePanel.getChildComponent (i)))
            inTakeUnavailableCopy += " " + label->getText();
    const bool lateCameraCopyNamesThisTake =
        inTakeUnavailableCopy.containsIgnoreCase ("out for this take")
        && inTakeUnavailableCopy.containsIgnoreCase ("next take");

    takeUnavailablePanel.setRecording (false);
    takeUnavailablePanel.setCameras (
        { { "late", "Late HDMI", true, false, false, "V01_Late-HDMI.mov", 1 } });
    juce::String afterTakeUnavailableCopy;
    for (int i = 0; i < takeUnavailablePanel.getNumChildComponents(); ++i)
        if (auto* label = dynamic_cast<juce::Label*> (
                takeUnavailablePanel.getChildComponent (i)))
            afterTakeUnavailableCopy += " " + label->getText();
    const bool afterTakeCopyReturnsToReconnect =
        afterTakeUnavailableCopy.containsIgnoreCase ("reconnect");

    std::printf ("main cache preserves unchanged viewer: %s\n",
                 mainCacheKeepsViewer ? "PASS" : "FAIL");
    std::printf ("main revision rebuilds viewer: %s\n",
                 mainRevisionRebuildsViewer ? "PASS" : "FAIL");
    std::printf ("camera REC state uses caption below native preview: %s\n",
                 recCaptionIsOutsideNativePreview ? "PASS" : "FAIL");
    std::printf ("stopped camera caption clears REC state: %s\n",
                 stoppedCaptionDropsRec ? "PASS" : "FAIL");
    std::printf ("panel cache preserves unchanged viewer: %s\n",
                 panelCacheKeepsViewer ? "PASS" : "FAIL");
    std::printf ("panel revision rebuilds viewer: %s\n\n",
                 panelRevisionRebuildsViewer ? "PASS" : "FAIL");
    std::printf ("recording freezes camera roster controls: %s\n",
                 takeFreezesCameraControls ? "PASS" : "FAIL");
    std::printf ("stopping restores camera roster controls: %s\n\n",
                 stopRestoresCameraControls ? "PASS" : "FAIL");
    std::printf ("late camera copy says this take/next take: %s\n",
                 lateCameraCopyNamesThisTake ? "PASS" : "FAIL");
    std::printf ("after-take unavailable copy returns to reconnect: %s\n\n",
                 afterTakeCopyReturnsToReconnect ? "PASS" : "FAIL");
    failures += mainCacheKeepsViewer ? 0 : 1;
    failures += mainRevisionRebuildsViewer ? 0 : 1;
    failures += recCaptionIsOutsideNativePreview ? 0 : 1;
    failures += stoppedCaptionDropsRec ? 0 : 1;
    failures += panelCacheKeepsViewer ? 0 : 1;
    failures += panelRevisionRebuildsViewer ? 0 : 1;
    failures += takeFreezesCameraControls ? 0 : 1;
    failures += stopRestoresCameraControls ? 0 : 1;
    failures += lateCameraCopyNamesThisTake ? 0 : 1;
    failures += afterTakeCopyReturnsToReconnect ? 0 : 1;

    struct Case { int w, h, mics; const char* label; };
    const Case cases[] = {
        // The size the window ACTUALLY opens at, which is what a user sees
        // before touching anything. The cases below it assume a window someone
        // has already dragged bigger, and measuring only those is how a picture
        // that is small on launch went out reported as large.
        { 1180,  560, 0, "REAL launch, no mics" },
        { 1180,  560, 3, "REAL launch, 3 mics" },
        { 1180,  900, 0, "dragged taller, no mics" },
        { 1180,  900, 3, "dragged taller, 3 mics" },
        { 2000, 1071, 0, "maximised wide, no mics" },
        { 1680, 1050, 3, "1680x1050, 3 mics" },
    };

    for (const auto& c : cases)
    {
        mma::MainScreen s;
        s.setMicCount (c.mics);
        s.setCameraScale (5);                       // the shipped default
        s.setCameraTiles ({ { "cam", "FaceTime" } });

        // What MainComponent does, in its order: tell the screen how much of it
        // the window can actually show, THEN size the content to the larger of
        // what it wants and the viewport.
        //
        // The order is the point. Sizing first and reporting after measures the
        // picture against a canvas the picture itself grew, which is the loop
        // that let a 1007px layout settle inside a 560px window.
        s.setVisibleHeight (c.h);
        s.setSize (c.w, juce::jmax (c.h, s.getRequiredHeight()));
        s.resized();

        // The tile is whatever child sits in the camera row -- the placeholder,
        // since makeViewer is unset here.
        juce::Rectangle<int> tile;
        for (int i = 0; i < s.getNumChildComponents(); ++i)
        {
            auto b = s.getChildComponent (i)->getBounds();
            if (b.getWidth() > tile.getWidth() && b.getHeight() > 60)
                tile = b;
        }

        std::printf ("%-26s window %4dx%-5d  required %5d  wants %5d  picture %4dx%-4d (%.0f%% of width)%s\n",
                     c.label, c.w, c.h, s.getRequiredHeight(), s.getPreferredHeight(),
                     tile.getWidth(), tile.getHeight(),
                     100.0 * tile.getWidth() / juce::jmax (1, c.w - 32),
                     s.getRequiredHeight() > c.h ? "  OVERFLOWS WINDOW" : "");
    }
    std::printf ("\n-- after the window grows to what the screen asked for --\n");

    for (const auto& c : cases)
    {
        mma::MainScreen s;
        s.setMicCount (c.mics);
        s.setCameraScale (5);
        s.setCameraTiles ({ { "cam", "FaceTime" } });

        // Pass one: what does it want in the window it has?
        s.setVisibleHeight (c.h);
        s.setSize (c.w, juce::jmax (c.h, s.getRequiredHeight()));
        s.resized();

        // Pass two: the owner grows the window to that, bounded by the display,
        // and the screen is laid out again in the window it now has.
        const int grown = juce::jmin (s.getPreferredHeight(), 1071);
        s.setVisibleHeight (grown);
        s.setSize (c.w, juce::jmax (grown, s.getRequiredHeight()));
        s.resized();

        juce::Rectangle<int> tile;
        for (int i = 0; i < s.getNumChildComponents(); ++i)
        {
            auto b = s.getChildComponent (i)->getBounds();
            if (b.getWidth() > tile.getWidth() && b.getHeight() > 60)
                tile = b;
        }

        std::printf ("%-26s window %4dx%-5d  required %5d  picture %4dx%-4d (%.0f%% of width)%s\n",
                     c.label, c.w, grown, s.getRequiredHeight(),
                     tile.getWidth(), tile.getHeight(),
                     100.0 * tile.getWidth() / juce::jmax (1, c.w - 32),
                     s.getRequiredHeight() > grown ? "  OVERFLOWS WINDOW" : "");
    }

    std::printf ("\n-- the monitor-problem line, which must fit its reason --\n");

    // The message that named a cause used to be clipped to the first few words
    // by a fixed one-line band, which left exactly the dead end it was written
    // to end. The band has to grow with the text, and the screen has to stay
    // inside the window while it does.
    struct Msg { const char* label; const char* text; };
    const Msg messages[] = {
        { "empty",  "" },
        { "short",  "Mic 1 couldn't be opened for recording." },
        { "reason", "PUPGSIS-T12S 1 couldn't be opened for recording. This interface is "
                    "running at 44.1 kHz and won't change to the 48 kHz this recording uses. "
                    "Set the recording to 44.1 kHz in Settings, or change the interface to "
                    "48 kHz in Audio MIDI Setup." },
    };

    for (const auto& m : messages)
    {
        mma::MainScreen s;
        s.setMicCount (2);
        s.setMonitorProblemText (m.text);

        s.setVisibleHeight (560);
        s.setSize (1180, juce::jmax (560, s.getRequiredHeight()));
        s.resized();

        const int grown = juce::jmin (s.getPreferredHeight(), 1071);
        s.setVisibleHeight (grown);
        s.setSize (1180, juce::jmax (grown, s.getRequiredHeight()));
        s.resized();

        std::printf ("%-8s band %3d px   required %4d  window %4d%s\n",
                     m.label, s.getMonitorProblemBandHeight(),
                     s.getRequiredHeight(), grown,
                     s.getRequiredHeight() > grown ? "  OVERFLOWS WINDOW" : "");
    }

    return failures == 0 ? 0 : 1;
}
