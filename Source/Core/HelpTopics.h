#pragma once
#include <string>
#include <vector>

namespace mma {

/// One entry in the app's Help screen: a heading and the plain-language body
/// under it. The body may contain newlines; a line starting with a digit and
/// a full stop is a step in a checklist.
struct HelpTopic
{
    std::string heading;
    std::string body;
};

/// §10.1 and §10.6: the answers to "why is it silent?", in the app, in order
/// of likelihood, rather than in a README the user is not looking at when
/// the meter is flat.
///
/// Plain data rather than UI so the words can be checked by a test: every
/// silent-recording cause this app has actually been bitten by has an entry
/// here, and a test says which ones must stay.
class HelpTopics
{
public:
    /// Every topic, in the order the screen shows them. First the checklist
    /// for a mixer or interface, because that is where most silent takes come
    /// from; last the diagnostics export, because that is what to do when
    /// nothing above applied.
    static std::vector<HelpTopic> all();

    /// The one-line introduction over the topics.
    static std::string introduction();
};

} // namespace mma
