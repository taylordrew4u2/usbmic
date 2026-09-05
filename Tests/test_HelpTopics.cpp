#include "TestFramework.h"
#include "Core/HelpTopics.h"
#include <cctype>

using namespace mma;

namespace {

bool anyTopicMentions (const std::string& phrase)
{
    for (const auto& t : HelpTopics::all())
        if (t.body.find (phrase) != std::string::npos || t.heading.find (phrase) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE (HelpTopics_EveryTopicHasAHeadingAndABody)
{
    const auto topics = HelpTopics::all();
    REQUIRE (topics.size() >= 5);

    for (const auto& t : topics)
    {
        REQUIRE (! t.heading.empty());
        REQUIRE (t.body.size() > 40);
    }

    REQUIRE (! HelpTopics::introduction().empty());
}

TEST_CASE (HelpTopics_SilentMixerChecklistComesFirst)
{
    // Silent takes from a mixer are what this screen exists for, so the
    // checklist is the first thing on it and names the four things that
    // actually caused them: the USB send, the faders, the mute, permission.
    const auto topics = HelpTopics::all();
    const auto& first = topics.front();

    REQUIRE (first.body.find ("LOOPBACK") != std::string::npos);
    REQUIRE (first.body.find ("fader") != std::string::npos);
    REQUIRE (first.body.find ("muted") != std::string::npos);
    REQUIRE (first.body.find ("Privacy & Security") != std::string::npos);
}

TEST_CASE (HelpTopics_NamesTheCausesTheAppHasBeenBittenBy)
{
    // Each of these was a real silent recording at some point. The words
    // that explain them stay on the screen.
    REQUIRE (anyTopicMentions ("Automatic"));        // rate refusal (v1.4.x)
    REQUIRE (anyTopicMentions ("16-bit"));           // bit depth mismatch
    REQUIRE (anyTopicMentions ("phantom"));          // condenser on a mixer without 48 V
    REQUIRE (anyTopicMentions ("Export diagnostics"));
    REQUIRE (anyTopicMentions ("in use"));           // another app holding the device
}

TEST_CASE (HelpTopics_ChecklistStepsAreNumberedInOrder)
{
    const auto& body = HelpTopics::all().front().body;
    int expected = 1;

    for (size_t pos = 0; pos < body.size(); )
    {
        const auto end = body.find ('\n', pos);
        const auto line = body.substr (pos, end == std::string::npos ? std::string::npos : end - pos);

        if (line.size() > 2 && std::isdigit (static_cast<unsigned char> (line[0])) && line[1] == '.')
        {
            REQUIRE (line[0] - '0' == expected);
            ++expected;
        }

        if (end == std::string::npos) break;
        pos = end + 1;
    }

    REQUIRE (expected > 4);
}
