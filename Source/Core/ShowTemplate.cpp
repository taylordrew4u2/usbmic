#include "ShowTemplate.h"
#include "SessionFolderNaming.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace mma {

namespace {

/// Only the choices a person makes about a microphone. The §2.1 layout
/// verdict is a fact about the hardware, learned by listening to it, and a
/// template carrying a stale one could collapse an interface's second input.
PersistedDeviceSettings userChoicesOf (const PersistedDeviceSettings& s)
{
    PersistedDeviceSettings out;
    out.assignedName = s.assignedName;
    out.trimDb = s.trimDb;
    out.disabledInputs = s.disabledInputs;
    out.inputNames = s.inputNames;
    out.inputTrimDb = s.inputTrimDb;
    return out;
}

void setMembership (std::vector<std::string>& keys, const std::string& key, bool member)
{
    const auto it = std::find (keys.begin(), keys.end(), key);

    if (member && it == keys.end())
        keys.push_back (key);
    else if (! member && it != keys.end())
        keys.erase (it);
}

bool contains (const std::vector<std::string>& keys, const std::string& key)
{
    return std::find (keys.begin(), keys.end(), key) != keys.end();
}

const JsonValue* findTyped (const JsonValue& v, const char* key, JsonValue::Type type)
{
    const auto* p = v.find (key);
    return p != nullptr && p->getType() == type ? p : nullptr;
}

} // namespace

ShowTemplate ShowTemplate::extract (const std::string& name, const AppSettings& settings,
                                    const std::vector<std::string>& connectedMicKeys)
{
    ShowTemplate t;
    t.name = cleanName (name);

    // No folder yet is not a choice to carry: applying it would have nowhere
    // to send the takes.
    if (! settings.destinationFolder.empty())
        t.destinationFolder = settings.destinationFolder;

    t.askWhereToSaveEveryTime = settings.askWhereToSaveEveryTime;
    t.mirrorEnabled = settings.mirrorEnabled;
    t.masterVolume = settings.masterVolume;
    t.monitorThroughCombinedDevice = settings.monitorThroughCombinedDevice;
    t.rememberedOutputDeviceId = settings.rememberedOutputDeviceId;
    t.combineVideoAndAudio = settings.combineVideoAndAudio;
    t.deliveryTarget = settings.deliveryTarget;
    t.sampleRateOverride = settings.sampleRateOverride;
    t.bitDepthOverride = settings.bitDepthOverride;

    for (const auto& port : settings.ports)
        t.ports.push_back ({ port.key, userChoicesOf (port.settings) });

    // A microphone that is plugged in but was never named or trimmed has no
    // entry of its own. It still has an answer -- its own name, no trim -- and
    // without one, loading this show after another that renamed it would
    // leave the other show's name on it.
    for (const auto& key : connectedMicKeys)
        if (! key.empty() && settings.findPort (key) == nullptr)
            t.ports.push_back ({ key, PersistedDeviceSettings{} });

    t.disabledMicKeys = settings.disabledMicKeys;
    t.headphonesOffKeys = settings.headphonesOffKeys;
    t.cameras = settings.cameras;

    std::set<std::string> covered;
    for (const auto& port : t.ports)              covered.insert (port.key);
    for (const auto& key : t.disabledMicKeys)     covered.insert (key);
    for (const auto& key : t.headphonesOffKeys)   covered.insert (key);
    t.microphoneKeys.assign (covered.begin(), covered.end());

    return t;
}

AppSettings ShowTemplate::applyTo (const AppSettings& settings) const
{
    AppSettings out = settings;

    if (destinationFolder.has_value() && ! destinationFolder->empty()
        && *destinationFolder != out.destinationFolder)
    {
        out.destinationFolder = *destinationFolder;

        // §10.1: the user agreed to a place, not to a setting. A different
        // folder has not been agreed to, so it is asked about before the next
        // take -- the rule Application::applyDestinationFolder already keeps.
        if (out.confirmedSaveLocation != out.destinationFolder)
            out.confirmedSaveLocation.clear();
    }

    if (askWhereToSaveEveryTime)      out.askWhereToSaveEveryTime = *askWhereToSaveEveryTime;
    if (mirrorEnabled)                out.mirrorEnabled = *mirrorEnabled;
    if (masterVolume)                 out.masterVolume = *masterVolume;
    if (monitorThroughCombinedDevice) out.monitorThroughCombinedDevice = *monitorThroughCombinedDevice;
    if (rememberedOutputDeviceId)     out.rememberedOutputDeviceId = *rememberedOutputDeviceId;
    if (combineVideoAndAudio)         out.combineVideoAndAudio = *combineVideoAndAudio;
    if (deliveryTarget)               out.deliveryTarget = *deliveryTarget;
    if (sampleRateOverride)           out.sampleRateOverride = *sampleRateOverride;
    if (bitDepthOverride)             out.bitDepthOverride = *bitDepthOverride;

    for (const auto& port : ports)
    {
        auto it = std::find_if (out.ports.begin(), out.ports.end(),
                                [&port] (const PersistedPort& p) { return p.key == port.key; });

        if (it == out.ports.end())
        {
            out.ports.push_back ({ port.key, PersistedDeviceSettings{} });
            it = out.ports.end() - 1;
        }

        // The layout verdict on the existing entry stays; everything the user
        // chose comes from the template.
        auto& s = it->settings;
        s.assignedName = port.settings.assignedName;
        s.trimDb = port.settings.trimDb;
        s.disabledInputs = port.settings.disabledInputs;
        s.inputNames = port.settings.inputNames;
        s.inputTrimDb = port.settings.inputTrimDb;
    }

    // Only the microphones the template has an answer for. A mic it never
    // heard of keeps its switch and its headphones exactly as they were.
    std::set<std::string> covered (microphoneKeys.begin(), microphoneKeys.end());
    for (const auto& port : ports)              covered.insert (port.key);
    for (const auto& key : disabledMicKeys)     covered.insert (key);
    for (const auto& key : headphonesOffKeys)   covered.insert (key);

    for (const auto& key : covered)
    {
        setMembership (out.disabledMicKeys, key, contains (disabledMicKeys, key));
        setMembership (out.headphonesOffKeys, key, contains (headphonesOffKeys, key));
    }

    // A camera entry is nothing but the user's answers, so the template's
    // replaces it whole -- which also carries any per-camera setting added to
    // PersistedCamera later without this needing to know about it.
    for (const auto& camera : cameras)
    {
        const auto it = std::find_if (out.cameras.begin(), out.cameras.end(),
                                      [&camera] (const PersistedCamera& c) { return c.id == camera.id; });

        if (it != out.cameras.end())
            *it = camera;
        else
            out.cameras.push_back (camera);
    }

    out.wasUnreadable = false;
    return out;
}

JsonValue ShowTemplate::toJson() const
{
    JsonValue root = JsonValue::makeObject();
    root["formatVersion"] = JsonValue (kFormatVersion);
    root["name"] = JsonValue (name);

    if (destinationFolder)            root["destinationFolder"] = JsonValue (*destinationFolder);
    if (askWhereToSaveEveryTime)      root["askWhereToSaveEveryTime"] = JsonValue (*askWhereToSaveEveryTime);
    if (mirrorEnabled)                root["mirrorEnabled"] = JsonValue (*mirrorEnabled);
    if (masterVolume)                 root["masterVolume"] = JsonValue (*masterVolume);
    if (monitorThroughCombinedDevice) root["monitorThroughCombinedDevice"] = JsonValue (*monitorThroughCombinedDevice);
    if (rememberedOutputDeviceId)     root["rememberedOutputDeviceId"] = JsonValue (*rememberedOutputDeviceId);
    if (combineVideoAndAudio)         root["combineVideoAndAudio"] = JsonValue (*combineVideoAndAudio);
    if (deliveryTarget)               root["deliveryTarget"] = JsonValue (*deliveryTarget);
    if (sampleRateOverride)           root["sampleRateOverride"] = JsonValue (static_cast<double> (*sampleRateOverride));
    if (bitDepthOverride)             root["bitDepthOverride"] = JsonValue (static_cast<double> (*bitDepthOverride));

    JsonValue mics = JsonValue::makeArray();
    for (const auto& key : microphoneKeys)
        mics.push_back (JsonValue (key));
    root["microphones"] = mics;

    // Ports, cameras and the two key lists are written by AppSettings itself,
    // so a template and settings.json can never disagree about their shape.
    AppSettings lists;
    lists.ports = ports;
    lists.disabledMicKeys = disabledMicKeys;
    lists.headphonesOffKeys = headphonesOffKeys;
    lists.cameras = cameras;
    const auto listsJson = lists.toJson();

    for (const char* key : { "ports", "disabledMicrophones", "headphonesOff", "cameras" })
        if (const auto* v = listsJson.find (key))
            root[key] = *v;

    return root;
}

std::optional<ShowTemplate> ShowTemplate::fromJsonString (const std::string& text)
{
    if (text.find_first_not_of (" \t\r\n") == std::string::npos)
        return std::nullopt;

    JsonValue parsed;

    try
    {
        parsed = JsonValue::parse (text);
    }
    catch (...)
    {
        return std::nullopt;
    }

    if (parsed.getType() != JsonValue::Type::Object || parsed.getValuedMemberCount() == 0)
        return std::nullopt;

    ShowTemplate t;
    using T = JsonValue::Type;

    if (const auto* p = findTyped (parsed, "name", T::String)) t.name = cleanName (p->asString());

    // Each value is taken only when it is the right kind. A wrong one is
    // skipped, not defaulted: a default would be applied, and silently
    // switching the backup copy off is worse than leaving it as it was.
    if (const auto* p = findTyped (parsed, "destinationFolder", T::String))
        if (! p->asString().empty())
            t.destinationFolder = p->asString();

    if (const auto* p = findTyped (parsed, "askWhereToSaveEveryTime", T::Bool))      t.askWhereToSaveEveryTime = p->asBool();
    if (const auto* p = findTyped (parsed, "mirrorEnabled", T::Bool))                t.mirrorEnabled = p->asBool();
    if (const auto* p = findTyped (parsed, "monitorThroughCombinedDevice", T::Bool)) t.monitorThroughCombinedDevice = p->asBool();
    if (const auto* p = findTyped (parsed, "rememberedOutputDeviceId", T::String))   t.rememberedOutputDeviceId = p->asString();
    if (const auto* p = findTyped (parsed, "combineVideoAndAudio", T::Bool))         t.combineVideoAndAudio = p->asBool();
    if (const auto* p = findTyped (parsed, "deliveryTarget", T::String))             t.deliveryTarget = p->asString();

    if (const auto* p = findTyped (parsed, "masterVolume", T::Number))
        if (std::isfinite (p->asDouble()))
            t.masterVolume = std::clamp (p->asDouble(), 0.0, 100.0);

    if (const auto* p = findTyped (parsed, "sampleRateOverride", T::Number))
        if (const double rate = p->asDouble(); rate >= 0.0 && rate <= 768000.0)
            t.sampleRateOverride = static_cast<uint32_t> (rate);

    // 0 is "the default"; anything else the app cannot write is not a choice.
    if (const auto* p = findTyped (parsed, "bitDepthOverride", T::Number))
        if (const int bits = p->asInt(); bits == 0 || bits == 16 || bits == 24 || bits == 32)
            t.bitDepthOverride = bits;

    if (const auto* p = findTyped (parsed, "microphones", T::Array))
        for (const auto& v : p->asArray())
            if (const auto key = v.asString(); ! key.empty() && ! contains (t.microphoneKeys, key))
                t.microphoneKeys.push_back (key);

    // The lists are read by AppSettings, for the same reason they are written
    // by it -- one parser, which already drops a port or camera with no key.
    const auto lists = AppSettings::fromJson (parsed);

    for (const auto& port : lists.ports)
        t.ports.push_back ({ port.key, userChoicesOf (port.settings) });

    t.disabledMicKeys = lists.disabledMicKeys;
    t.headphonesOffKeys = lists.headphonesOffKeys;
    t.cameras = lists.cameras;

    return t;
}

std::string ShowTemplate::cleanName (const std::string& rawName)
{
    constexpr size_t kMaxLength = 60;
    const auto isSpace = [] (char c) { return std::isspace (static_cast<unsigned char> (c)) != 0; };

    auto first = rawName.begin();
    auto last = rawName.end();
    while (first != last && isSpace (*first))       ++first;
    while (last != first && isSpace (*(last - 1)))  --last;

    std::string name (first, last);

    // Line breaks and other control characters would split the name across
    // the picker's row.
    std::replace_if (name.begin(), name.end(),
                     [] (char c) { return static_cast<unsigned char> (c) < 0x20; }, ' ');

    if (name.size() > kMaxLength)
    {
        // Cut on a character boundary: a UTF-8 continuation byte left at the
        // end would make the name invalid text.
        size_t cut = kMaxLength;
        while (cut > 0 && (static_cast<unsigned char> (name[cut]) & 0xC0) == 0x80)
            --cut;
        name.resize (cut);
    }

    return name;
}

std::string ShowTemplate::fileNameFor (const std::string& name)
{
    const auto clean = cleanName (name);
    const auto stem = SessionFolderNaming::sanitizeNameOrEmpty (clean);

    if (! stem.empty())
        return stem + ".json";

    // §6.2's sanitizing now keeps letters in every script, so "日本語ライブ"
    // gets a file of that name. What can still leave nothing is a name made
    // only of characters no file name may carry; and shows saved by earlier
    // versions, which kept ASCII only, are already in "Show.json", "Show-2.json"
    // ... The file name is only where the show is kept -- it is listed, loaded
    // and deleted by the name stored inside -- so such a name gets a plain file
    // of its own (fileNameForSaving adds "-2"... when that one is taken).
    const bool hasNonAscii = std::any_of (clean.begin(), clean.end(),
                                          [] (char c) { return static_cast<unsigned char> (c) >= 0x80; });

    return hasNonAscii ? std::string ("Show.json") : std::string();
}

namespace {

bool sameFileName (const std::string& a, const std::string& b)
{
    // Beyond ASCII too, now that file names keep other scripts: "Ü.json" and
    // "ü.json" are one file on a Mac, and saving one must not replace the other.
    return SessionFolderNaming::foldCaseForComparison (a)
        == SessionFolderNaming::foldCaseForComparison (b);
}

std::string stemOf (const std::string& fileName)
{
    const auto dot = fileName.rfind ('.');
    return dot == std::string::npos ? fileName : fileName.substr (0, dot);
}

} // namespace

std::vector<std::pair<std::string, std::string>> ShowTemplate::listShows (const std::vector<StoredFile>& files)
{
    // In file-name order, so which of two copies keeps the name does not
    // depend on the order the disk happened to return them in.
    std::vector<const StoredFile*> sorted;
    for (const auto& f : files)
        if (f.name.has_value() && ! f.fileName.empty())
            sorted.push_back (&f);

    std::sort (sorted.begin(), sorted.end(),
               [] (const StoredFile* a, const StoredFile* b) { return a->fileName < b->fileName; });

    std::vector<std::pair<std::string, std::string>> shows;
    std::set<const StoredFile*> placed;

    const auto claim = [&] (const std::string& listed, const StoredFile* f)
    {
        if (listed.empty() || placed.count (f) != 0)
            return;

        for (const auto& show : shows)
            if (show.first == listed)
                return;

        shows.emplace_back (listed, f->fileName);
        placed.insert (f);
    };

    // A name's own file first, then the "-2" files that names sharing it were
    // given, then whatever is left under its file name.
    for (const auto* f : sorted)
        if (sameFileName (f->fileName, fileNameFor (*f->name)))
            claim (cleanName (*f->name), f);

    for (const auto* f : sorted)
        claim (cleanName (*f->name), f);

    for (const auto* f : sorted)
        claim (stemOf (f->fileName), f);

    return shows;
}

std::string ShowTemplate::findFileFor (const std::string& name, const std::vector<StoredFile>& files)
{
    const auto clean = cleanName (name);

    for (const auto& show : listShows (files))
        if (show.first == clean)
            return show.second;

    return {};
}

std::string ShowTemplate::fileNameForSaving (const std::string& name, const std::vector<StoredFile>& files)
{
    const auto base = fileNameFor (name);

    if (base.empty())
        return {};

    if (auto own = findFileFor (name, files); ! own.empty())
        return own;

    // Any file at all counts as taken, a stray one that is not a template
    // included: saving must never write over something it did not make.
    const auto taken = [&files] (const std::string& candidate)
    {
        return std::any_of (files.begin(), files.end(),
                            [&candidate] (const StoredFile& f) { return sameFileName (f.fileName, candidate); });
    };

    const auto stem = stemOf (base);
    auto candidate = base;

    for (int suffix = 2; taken (candidate); ++suffix)
        candidate = stem + "-" + std::to_string (suffix) + ".json";

    return candidate;
}

} // namespace mma
