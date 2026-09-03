// Lays out the real MainScreen with camera tiles and reports what the picture
// actually comes out as. No display needed: Component layout is arithmetic.
#include "UI/MainScreen.h"
#include <cstdio>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    struct Case { int w, h, mics; const char* label; };
    const Case cases[] = {
        { 1180,  900, 0, "opens at, no mics" },
        { 1180,  900, 3, "opens at, 3 mics" },
        { 2000, 1071, 0, "maximised wide, no mics" },
        { 1680, 1050, 3, "1680x1050, 3 mics" },
    };

    for (const auto& c : cases)
    {
        mma::MainScreen s;
        s.setMicCount (c.mics);
        s.setCameraScale (5);                       // the shipped default
        s.setCameraTiles ({ { "cam", "FaceTime" } });

        // What MainComponent does: size the content to the larger of what it
        // wants and the viewport, then lay it out.
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

        std::printf ("%-26s window %4dx%-5d  screen h %5d  required %5d  picture %4dx%-4d (%.0f%% of width)\n",
                     c.label, c.w, c.h, s.getHeight(), s.getRequiredHeight(),
                     tile.getWidth(), tile.getHeight(),
                     100.0 * tile.getWidth() / juce::jmax (1, c.w - 32));
    }
    return 0;
}
