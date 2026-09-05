#include "HelpTopics.h"

namespace mma {

std::string HelpTopics::introduction()
{
    return "Most problems come down to one of these. Start with the one that "
           "matches what you are seeing.";
}

std::vector<HelpTopic> HelpTopics::all()
{
    return {
        { "The recording is silent, or the skull never fills",
          "On a mixer or interface the sound has to reach the USB cable before "
          "this app can hear it. Check these, in this order:\n"
          "1. The microphone has a skull on the main screen. If it does not, "
          "open Settings and tick its box, and the socket under it.\n"
          "2. This app is allowed to use the microphone: System Settings > "
          "Privacy & Security > Microphone > SobStage switched on. Without "
          "it every meter stays flat and nothing says why.\n"
          "3. The channel is not muted, its fader is up, and the main fader is "
          "up. A mixer sends only what its faders let through.\n"
          "4. The mixer is sending to USB. On a small mixer this is a button "
          "marked LOOPBACK, USB or PC SEND (on a PUPGSIS T12S it is LOOPBACK). "
          "With it off the computer gets silence however loud the room is.\n"
          "5. The microphone's gain knob is up and the mic is in a socket that "
          "goes into the mix, not a headphone or line-out socket.\n"
          "6. Speak. The skull should fill and the number under it should move. "
          "If it does, record. If it does not, unplug the USB cable, plug it "
          "back in, and wait for the strip to come back." },

        { "Dynamic or condenser microphone",
          "A dynamic microphone (an SM58, most handheld stage mics) needs "
          "nothing but a cable. A condenser microphone needs 48 V phantom "
          "power from the socket it is plugged into, and a socket that cannot "
          "supply it leaves the mic silent. Many small USB mixers have no 48 V "
          "at all (the PUPGSIS T12S is one), so a condenser on them never "
          "makes a sound. Use a dynamic mic on those, or a separate phantom "
          "power supply between the mic and the mixer." },

        { "The amber line under the strips",
          "That line is the app saying a device could not be opened, and why, "
          "in the words the system gave. If it mentions the sample rate, the "
          "device refused the rate it was asked for: open Settings and put "
          "Sample rate back on Automatic, which stays on whatever the device "
          "is already running. If it says the device is in use, another app is "
          "holding it: quit that app (a browser tab with a call open counts) "
          "and the line goes away on its own. For anything else, unplug the "
          "device, plug it back in, and look again." },

        { "Sample rate, bit depth and buffer size",
          "Sample rate: leave it on Automatic. It follows the rate the "
          "interface is already running, which is the one the interface will "
          "accept. Choose a specific rate only when a project needs it.\n"
          "Bit depth: match the device. Small mixers are 16-bit (the PUPGSIS "
          "T12S is); most interfaces are 24-bit. Asking a 16-bit device for 24 "
          "gains nothing and some devices refuse it.\n"
          "Buffer size: leave it on Automatic. Smaller means less delay in the "
          "headphones and more chance of a dropout. The app steps up on its own "
          "if this computer cannot keep up." },

        { "A mixer or interface with several sockets",
          "An interface is one box in Settings with a tick box for each socket "
          "under it. Untick the sockets nobody is using and they are not "
          "recorded: no strip, no file, no share of the space estimate. A "
          "small stereo mixer sends its whole mix over USB as left and right, "
          "so it shows two sockets and both carry the mix. Click a strip's "
          "name on the main screen to name that socket's person. The name goes "
          "on the strip and in the file, and it follows the box across a "
          "replug and a relaunch." },

        { "Where the files are",
          "Every take is a folder inside the place shown in the footer of the "
          "main screen, with one file per microphone and one for the mix. "
          "Change the place in Settings > Save recordings to. The backup copy, "
          "on by default, writes a second copy somewhere else in case the "
          "first drive fails. After every take a card says exactly where the "
          "files went, with a button that opens the folder." },

        { "Still stuck",
          "Settings > Export diagnostics bundles the log, the list of devices "
          "the system reported and recent session details, never audio, into "
          "one file. Send it with a sentence on what you expected and what "
          "happened instead." },
    };
}

} // namespace mma
