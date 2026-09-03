#include "AppSettings.h"
#include <algorithm>

namespace mma {

JsonValue AppSettings::toJson() const
{
    JsonValue root = JsonValue::makeObject();
    root["destinationFolder"] = JsonValue (destinationFolder);
    root["confirmedSaveLocation"] = JsonValue (confirmedSaveLocation);
    root["askWhereToSaveEveryTime"] = JsonValue (askWhereToSaveEveryTime);
    root["mirrorEnabled"] = JsonValue (mirrorEnabled);
    root["aggregateName"] = JsonValue (aggregateName);
    root["masterVolume"] = JsonValue (masterVolume);
    root["cameraPreviewFullQuality"] = JsonValue (cameraPreviewFullQuality);
    root["cameraTileScale"] = JsonValue (static_cast<double> (cameraTileScale));
    root["combineVideoAndAudio"] = JsonValue (combineVideoAndAudio);

    JsonValue portArr = JsonValue::makeArray();
    for (const auto& p : ports)
    {
        JsonValue pv = JsonValue::makeObject();
        pv["key"] = JsonValue (p.key);
        pv["assignedName"] = JsonValue (p.settings.assignedName);
        pv["trimDb"] = JsonValue (static_cast<double> (p.settings.trimDb));
        pv["channelLayoutIsMono"] = JsonValue (p.settings.channelLayoutIsMono);
        pv["hasChannelLayoutDecision"] = JsonValue (p.settings.hasChannelLayoutDecision);
        portArr.push_back (pv);
    }
    root["ports"] = portArr;

    JsonValue disabledArr = JsonValue::makeArray();
    for (const auto& key : disabledMicKeys)
        disabledArr.push_back (JsonValue (key));
    root["disabledMicrophones"] = disabledArr;

    JsonValue cameraArr = JsonValue::makeArray();
    for (const auto& c : cameras)
    {
        JsonValue cv = JsonValue::makeObject();
        cv["id"] = JsonValue (c.id);
        cv["enabled"] = JsonValue (c.enabled);
        cv["assignedName"] = JsonValue (c.assignedName);
        cameraArr.push_back (cv);
    }
    root["cameras"] = cameraArr;

    return root;
}

AppSettings AppSettings::fromJson (const JsonValue& v)
{
    AppSettings s;

    // Every field falls back to the default already in the struct, so a file
    // written by an older version -- or one missing a key entirely -- loads the
    // rest rather than being thrown away whole.
    if (auto* p = v.find ("destinationFolder")) s.destinationFolder = p->asString();
    if (auto* p = v.find ("confirmedSaveLocation")) s.confirmedSaveLocation = p->asString();
    if (auto* p = v.find ("askWhereToSaveEveryTime")) s.askWhereToSaveEveryTime = p->asBool (false);
    if (auto* p = v.find ("mirrorEnabled")) s.mirrorEnabled = p->asBool (true);
    if (auto* p = v.find ("aggregateName")) s.aggregateName = p->asString (s.aggregateName);
    if (auto* p = v.find ("masterVolume")) s.masterVolume = p->asDouble (s.masterVolume);
    if (auto* p = v.find ("cameraPreviewFullQuality")) s.cameraPreviewFullQuality = p->asBool (false);
    if (auto* p = v.find ("cameraTileScale")) s.cameraTileScale = static_cast<int> (p->asDouble (1.0));
    if (auto* p = v.find ("combineVideoAndAudio")) s.combineVideoAndAudio = p->asBool (false);

    if (auto* p = v.find ("ports"))
        for (const auto& pv : p->asArray())
        {
            PersistedPort port;
            if (auto* n = pv.find ("key")) port.key = n->asString();

            // A port entry with no key cannot be matched to a device, so it is
            // dropped rather than kept as a row that can never apply.
            if (port.key.empty())
                continue;

            if (auto* n = pv.find ("assignedName")) port.settings.assignedName = n->asString();
            if (auto* n = pv.find ("trimDb")) port.settings.trimDb = static_cast<float> (n->asDouble());
            if (auto* n = pv.find ("channelLayoutIsMono")) port.settings.channelLayoutIsMono = n->asBool (true);
            if (auto* n = pv.find ("hasChannelLayoutDecision")) port.settings.hasChannelLayoutDecision = n->asBool (false);
            s.ports.push_back (port);
        }

    if (auto* p = v.find ("disabledMicrophones"))
        for (const auto& dv : p->asArray())
            if (const auto key = dv.asString(); ! key.empty())
                s.disabledMicKeys.push_back (key);

    if (auto* p = v.find ("cameras"))
        for (const auto& cv : p->asArray())
        {
            PersistedCamera camera;
            if (auto* n = cv.find ("id")) camera.id = n->asString();

            if (camera.id.empty())
                continue;

            if (auto* n = cv.find ("enabled")) camera.enabled = n->asBool (false);
            if (auto* n = cv.find ("assignedName")) camera.assignedName = n->asString();
            s.cameras.push_back (camera);
        }

    return s;
}

AppSettings AppSettings::fromJsonString (const std::string& text)
{
    // JsonValue::parse throws on anything it cannot read. A preferences file is
    // never worth failing to launch over, so anything unreadable is defaults.
    try
    {
        return fromJson (JsonValue::parse (text));
    }
    catch (...)
    {
        return {};
    }
}

const PersistedPort* AppSettings::findPort (const std::string& key) const
{
    const auto it = std::find_if (ports.begin(), ports.end(),
                                  [&key] (const PersistedPort& p) { return p.key == key; });
    return it != ports.end() ? &*it : nullptr;
}

const PersistedCamera* AppSettings::findCamera (const std::string& id) const
{
    const auto it = std::find_if (cameras.begin(), cameras.end(),
                                  [&id] (const PersistedCamera& c) { return c.id == id; });
    return it != cameras.end() ? &*it : nullptr;
}

bool AppSettings::isMicDisabled (const std::string& key) const
{
    return std::find (disabledMicKeys.begin(), disabledMicKeys.end(), key) != disabledMicKeys.end();
}

} // namespace mma
