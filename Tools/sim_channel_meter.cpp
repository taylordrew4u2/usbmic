// Drives one microphone's channel strip the way the app does -- audio blocks
// into a real Metering, the strip's own 60 Hz timer reading it -- through every
// state the crying face has, and checks each one twice: what the component says
// its face is, and what it actually painted, read back from its pixels.
//
//   sim_channel_meter [snapshot-dir]
//
// With a directory, every state is also written out as a PNG to look at.

#include "UI/ChannelMeterComponent.h"
#include "UI/MeterFaceProbe.h"
#include "UI/AppLookAndFeel.h"
#include "Core/Metering.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using mma::ChannelMeterComponent;
using Face = ChannelMeterComponent::Face;
using mma::MeterFaceProbe;

int checks = 0, failures = 0;

void check (bool condition, const juce::String& label)
{
    ++checks;
    std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", label.toRawUTF8());
    failures += condition ? 0 : 1;
}

const char* faceName (Face f)
{
    switch (f)
    {
        case Face::Asleep:  return "asleep";
        case Face::Frown:   return "frown";
        case Face::OneTear: return "one tear";
        case Face::Sob:     return "sob";
    }
    return "?";
}

/// A microphone: a sine at a set peak level, pushed into the meter in 10 ms
/// blocks, as the audio callback would. A peak of 0 dBFS is a mic slammed into
/// full scale, which is what latches a clip (§8.1: three samples at -0.1).
class FakeMic : private juce::Timer
{
public:
    explicit FakeMic (mma::Metering& m) : metering (m) { startTimer (10); }
    ~FakeMic() override { stopTimer(); }

    void setPeakDb (float db) { amplitude = db <= -120.0f ? 0.0f : std::pow (10.0f, db / 20.0f); }
    void setClipping()        { amplitude = 1.0f; square = true; }
    void setSine()            { square = false; }

private:
    void timerCallback() override
    {
        std::vector<float> block (480);
        for (auto& s : block)
        {
            const float v = std::sin (phase);
            s = amplitude * (square ? (v >= 0.0f ? 1.0f : -1.0f) : v);
            phase += 2.0f * juce::MathConstants<float>::pi * 440.0f / 48000.0f;
        }
        phase = std::fmod (phase, 2.0f * juce::MathConstants<float>::pi);
        metering.processAudioBlock (block.data(), (int) block.size());
    }

    mma::Metering& metering;
    float amplitude = 0.0f, phase = 0.0f;
    bool square = false;
};

void run (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

/// Runs the app's timers until the strip is showing a level within 1.5 dB of
/// target. The meter's ballistics count timer ticks, not wall time, and a busy
/// CI machine ticks slower than 60 Hz -- so waiting a fixed time checked the
/// face mid-decay on macOS. Waiting on the level checks it where it lands.
void settleAt (ChannelMeterComponent& meter, float targetDb, int timeoutMs = 15000)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (std::abs (meter.getDisplayedLevelDb() - targetDb) > 1.5f
           && juce::Time::getMillisecondCounter() - start < (juce::uint32) timeoutMs)
        run (50);
    run (100); // one more repaint at the settled level
    check (std::abs (meter.getDisplayedLevelDb() - targetDb) <= 1.5f,
           "the meter settles at " + juce::String (targetDb, 1) + " dBFS (shows "
               + juce::String (meter.getDisplayedLevelDb(), 1) + ")");
}

juce::String snapshotDir;

void snapshot (ChannelMeterComponent& meter, const juce::String& name)
{
    if (snapshotDir.isEmpty())
        return;
    const auto image = meter.createComponentSnapshot (meter.getLocalBounds(), true, 3.0f);
    const auto file = juce::File (snapshotDir).getChildFile (name + ".png");
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (image, out);
}

/// Both halves of the check: the face the strip reports, and the tears that
/// are really on screen, which must match it -- none for asleep and frown, one
/// on the left for one tear, one on each side for a sob.
void expectFace (ChannelMeterComponent& meter, Face expected, const juce::String& when)
{
    const auto face = meter.getFace();
    check (face == expected, when + ": face is " + faceName (expected)
                                 + " (got " + faceName (face) + ")");

    const auto p = MeterFaceProbe::of (meter);
    const bool left = p.tearOnLeft(), right = p.tearOnRight();

    switch (expected)
    {
        case Face::Asleep:
        case Face::Frown:
            check (p.tears() < MeterFaceProbe::kTearMinPixels,
                   when + ": no tear is drawn (" + juce::String (p.tears()) + " tear pixels)");
            break;
        case Face::OneTear:
            check (left && ! right, when + ": one tear is drawn, under the left eye ("
                                         + juce::String (p.tearLeft) + " left, "
                                         + juce::String (p.tearRight) + " right)");
            break;
        case Face::Sob:
            check (left && right, when + ": two tears are drawn ("
                                      + juce::String (p.tearLeft) + " left, "
                                      + juce::String (p.tearRight) + " right)");
            break;
    }
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    mma::AppLookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    if (argc > 1)
        snapshotDir = argv[1];

    std::printf ("Channel strip: the crying face against a live meter\n");
    std::printf ("===================================================\n");

    {
        mma::Metering metering (48000.0);
        ChannelMeterComponent meter;
        meter.setSize (220, 40);
        meter.setMicName ("Vocal L");

        std::printf ("\n-- no microphone --\n");
        run (200);
        expectFace (meter, Face::Asleep, "a strip with no signal yet");
        check (MeterFaceProbe::of (meter).fillLow + MeterFaceProbe::of (meter).fillMid
                   + MeterFaceProbe::of (meter).fillHigh == 0,
               "an asleep badge carries no level fill");
        check (meter.getDescription().contains ("No signal"),
               "a screen reader hears \"No signal\"");
        snapshot (meter, "01-asleep");

        FakeMic mic (metering);
        meter.setMetering (&metering);
        meter.setNoSignal (false);

        std::printf ("\n-- live, quiet --\n");
        mic.setPeakDb (-200.0f);
        run (400);
        expectFace (meter, Face::Frown, "a live microphone in silence");
        snapshot (meter, "02-silent");

        mic.setPeakDb (-30.0f);
        settleAt (meter, -30.0f);
        expectFace (meter, Face::Frown, "a microphone peaking at -30 dBFS");
        {
            const auto p = MeterFaceProbe::of (meter);
            check (p.fillLow > 20, "the face does not hide the green level fill ("
                                       + juce::String (p.fillLow) + " px)");
        }
        snapshot (meter, "03-quiet");

        std::printf ("\n-- the -18 dBFS line --\n");
        mic.setPeakDb (-19.5f);
        settleAt (meter, -19.5f, 15000);
        expectFace (meter, Face::Frown, "just under the line, at -19.5 dBFS");

        mic.setPeakDb (-16.5f);
        settleAt (meter, -16.5f);
        expectFace (meter, Face::OneTear, "just over the line, at -16.5 dBFS");
        snapshot (meter, "04-over-the-line");

        mic.setPeakDb (-8.0f);   // the virtual mics' tone, 0.4 of full scale
        settleAt (meter, -8.0f);
        expectFace (meter, Face::OneTear, "a loud microphone at -8 dBFS");
        {
            const auto p = MeterFaceProbe::of (meter);
            check (p.fillMid > 20, "the tear does not hide the yellow level fill ("
                                       + juce::String (p.fillMid) + " px)");
        }
        snapshot (meter, "05-loud");

        std::printf ("\n-- clipping --\n");
        mic.setClipping();
        settleAt (meter, 0.0f);
        expectFace (meter, Face::Sob, "a microphone driven into full scale");
        {
            const auto p = MeterFaceProbe::of (meter);
            check (p.clipRing > 20, "the badge ring turns clip yellow ("
                                        + juce::String (p.clipRing) + " px)");
        }
        check (meter.getDescription().contains ("Clipping"),
               "a screen reader hears that it is clipping");
        snapshot (meter, "06-sob");

        mic.setSine();
        mic.setPeakDb (-30.0f);
        settleAt (meter, -30.0f);
        expectFace (meter, Face::Sob, "once quiet again, the clip stays latched");

        check (meter.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)),
               "Return clears the clip");
        run (300);
        expectFace (meter, Face::Frown, "after the clip is cleared, back to a frown");
        {
            const auto p = MeterFaceProbe::of (meter);
            check (p.clipRing < 6, "and the clip-yellow ring is gone ("
                                       + juce::String (p.clipRing) + " px)");
        }
        snapshot (meter, "07-cleared");

        std::printf ("\n-- falling back --\n");
        mic.setPeakDb (-8.0f);
        settleAt (meter, -8.0f);
        expectFace (meter, Face::OneTear, "loud again");
        mic.setPeakDb (-40.0f);
        settleAt (meter, -40.0f);
        expectFace (meter, Face::Frown, "the tear dries once the level falls away");

        std::printf ("\n-- the microphone goes away --\n");
        mic.setPeakDb (-8.0f);
        settleAt (meter, -8.0f);
        meter.setNoSignal (true);
        run (200);
        expectFace (meter, Face::Asleep, "a microphone lost while loud");
        check (meter.getDescription().contains ("No signal"),
               "and a screen reader hears \"No signal\"");
        snapshot (meter, "08-lost");
        meter.setNoSignal (false);

        std::printf ("\n-- highlighted, the mic being heard --\n");
        run (300);
        check (MeterFaceProbe::of (meter).highlightRing == 0, "no ring before it is highlighted");
        meter.setHighlighted (true);
        check (MeterFaceProbe::of (meter).highlightRing > 10, "a bone ring round the highlighted strip");
        expectFace (meter, Face::OneTear, "the face still shows through the highlight");
        snapshot (meter, "09-highlighted");

        meter.setMetering (nullptr);
    }

    std::printf ("\n-- every size the main screen gives a strip --\n");
    for (auto size : { juce::Point<int> { 150, 28 }, { 220, 40 }, { 320, 56 } })
    {
        mma::Metering metering (48000.0);
        ChannelMeterComponent meter;
        meter.setSize (size.x, size.y);
        meter.setMicName ("Kick");
        meter.setMetering (&metering);
        meter.setNoSignal (false);

        FakeMic mic (metering);
        const auto at = juce::String (size.x) + "x" + juce::String (size.y);

        mic.setPeakDb (-8.0f);
        settleAt (meter, -8.0f);
        expectFace (meter, Face::OneTear, at + ", loud");

        mic.setClipping();
        settleAt (meter, 0.0f);
        expectFace (meter, Face::Sob, at + ", clipping");
        snapshot (meter, "10-size-" + at);

        meter.setMetering (nullptr);
    }

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
