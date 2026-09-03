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

    return 0;
}
