// Lays out the real MainScreen with camera tiles and reports what the picture
// actually comes out as. No display needed: Component layout is arithmetic.
#include "UI/MainScreen.h"
#include <cstdio>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

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

    return 0;
}
