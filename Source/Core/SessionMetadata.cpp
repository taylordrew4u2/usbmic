#include "SessionMetadata.h"

namespace mma {

JsonValue SessionMetadata::toJson() const
{
    JsonValue root = JsonValue::makeObject();
    root["appVersion"] = JsonValue (appVersion);
    root["startTimestamp"] = JsonValue (startTimestampIso);
    root["stopTimestamp"] = JsonValue (stopTimestampIso);
    root["sampleRate"] = JsonValue (sampleRate);
    root["bitDepth"] = JsonValue (bitDepth);
    root["bufferSizeSamples"] = JsonValue (bufferSizeSamples);
    root["measuredLatencyMs"] = JsonValue (measuredLatencyMs);
    root["mirrorEnabled"] = JsonValue (mirrorEnabled);
    root["mirrorActive"] = JsonValue (mirrorActive);
    root["mirrorPath"] = JsonValue (mirrorPath);

    JsonValue deviceArr = JsonValue::makeArray();
    for (const auto& d : devices)
    {
        JsonValue dv = JsonValue::makeObject();
        dv["name"] = JsonValue (d.name);
        dv["usbId"] = JsonValue (d.usbId);
        dv["trimDb"] = JsonValue (static_cast<double> (d.trimDb));
        deviceArr.push_back (dv);
    }
    root["devices"] = deviceArr;

    JsonValue driftArr = JsonValue::makeArray();
    for (const auto& e : driftLog)
    {
        JsonValue dv = JsonValue::makeObject();
        dv["timestampSeconds"] = JsonValue (e.timestampSeconds);
        dv["deviceUsbId"] = JsonValue (e.deviceUsbId);
        dv["driftPpm"] = JsonValue (e.drift_ppm);
        driftArr.push_back (dv);
    }
    root["driftLog"] = driftArr;

    JsonValue bufArr = JsonValue::makeArray();
    for (const auto& e : bufferChanges)
    {
        JsonValue dv = JsonValue::makeObject();
        dv["timestampSeconds"] = JsonValue (e.timestampSeconds);
        dv["oldBufferSize"] = JsonValue (e.oldBufferSize);
        dv["newBufferSize"] = JsonValue (e.newBufferSize);
        bufArr.push_back (dv);
    }
    root["bufferChanges"] = bufArr;

    JsonValue dropoutArr = JsonValue::makeArray();
    for (const auto& e : dropouts)
    {
        JsonValue dv = JsonValue::makeObject();
        dv["timestampSeconds"] = JsonValue (e.timestampSeconds);
        dv["deviceUsbId"] = JsonValue (e.deviceUsbId);
        dv["description"] = JsonValue (e.description);
        dropoutArr.push_back (dv);
    }
    root["dropouts"] = dropoutArr;

    JsonValue failoverArr = JsonValue::makeArray();
    for (const auto& e : failovers)
    {
        JsonValue dv = JsonValue::makeObject();
        dv["timestampSeconds"] = JsonValue (e.timestampSeconds);
        dv["oldMasterUsbId"] = JsonValue (e.oldMasterUsbId);
        dv["newMasterUsbId"] = JsonValue (e.newMasterUsbId);
        failoverArr.push_back (dv);
    }
    root["failovers"] = failoverArr;

    return root;
}

SessionMetadata SessionMetadata::fromJson (const JsonValue& v)
{
    SessionMetadata m;
    if (auto* p = v.find ("appVersion")) m.appVersion = p->asString();
    if (auto* p = v.find ("startTimestamp")) m.startTimestampIso = p->asString();
    if (auto* p = v.find ("stopTimestamp")) m.stopTimestampIso = p->asString();
    if (auto* p = v.find ("sampleRate")) m.sampleRate = p->asDouble (48000.0);
    if (auto* p = v.find ("bitDepth")) m.bitDepth = p->asInt (24);
    if (auto* p = v.find ("bufferSizeSamples")) m.bufferSizeSamples = p->asInt (64);
    if (auto* p = v.find ("measuredLatencyMs")) m.measuredLatencyMs = p->asDouble (0.0);
    if (auto* p = v.find ("mirrorEnabled")) m.mirrorEnabled = p->asBool (true);
    if (auto* p = v.find ("mirrorActive")) m.mirrorActive = p->asBool (true);
    if (auto* p = v.find ("mirrorPath")) m.mirrorPath = p->asString();

    if (auto* p = v.find ("devices"))
        for (const auto& dv : p->asArray())
        {
            DeviceRecord d;
            if (auto* n = dv.find ("name")) d.name = n->asString();
            if (auto* n = dv.find ("usbId")) d.usbId = n->asString();
            if (auto* n = dv.find ("trimDb")) d.trimDb = static_cast<float> (n->asDouble());
            m.devices.push_back (d);
        }

    if (auto* p = v.find ("driftLog"))
        for (const auto& dv : p->asArray())
        {
            DriftLogEntry e;
            if (auto* n = dv.find ("timestampSeconds")) e.timestampSeconds = n->asDouble();
            if (auto* n = dv.find ("deviceUsbId")) e.deviceUsbId = n->asString();
            if (auto* n = dv.find ("driftPpm")) e.drift_ppm = n->asDouble();
            m.driftLog.push_back (e);
        }

    if (auto* p = v.find ("bufferChanges"))
        for (const auto& dv : p->asArray())
        {
            BufferChangeEntry e;
            if (auto* n = dv.find ("timestampSeconds")) e.timestampSeconds = n->asDouble();
            if (auto* n = dv.find ("oldBufferSize")) e.oldBufferSize = n->asInt();
            if (auto* n = dv.find ("newBufferSize")) e.newBufferSize = n->asInt();
            m.bufferChanges.push_back (e);
        }

    if (auto* p = v.find ("dropouts"))
        for (const auto& dv : p->asArray())
        {
            DropoutEntry e;
            if (auto* n = dv.find ("timestampSeconds")) e.timestampSeconds = n->asDouble();
            if (auto* n = dv.find ("deviceUsbId")) e.deviceUsbId = n->asString();
            if (auto* n = dv.find ("description")) e.description = n->asString();
            m.dropouts.push_back (e);
        }

    if (auto* p = v.find ("failovers"))
        for (const auto& dv : p->asArray())
        {
            FailoverEntry e;
            if (auto* n = dv.find ("timestampSeconds")) e.timestampSeconds = n->asDouble();
            if (auto* n = dv.find ("oldMasterUsbId")) e.oldMasterUsbId = n->asString();
            if (auto* n = dv.find ("newMasterUsbId")) e.newMasterUsbId = n->asString();
            m.failovers.push_back (e);
        }

    return m;
}

} // namespace mma
