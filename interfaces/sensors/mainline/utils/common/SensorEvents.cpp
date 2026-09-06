/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "libsensors_common/SensorEvents.h"

#include <android-base/stringprintf.h>

#include <time.h>

#include <cinttypes>
#include <cstring>

namespace aidl::android::hardware::sensors::mainline {

using ::aidl::android::hardware::sensors::Event;
using ::aidl::android::hardware::sensors::SensorInfo;
using ::aidl::android::hardware::sensors::SensorStatus;
using ::aidl::android::hardware::sensors::SensorType;
using EventPayload = Event::EventPayload;

namespace {
constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;
}  // namespace

int64_t GetBootTimeNs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * kNanosecondsPerSecond + ts.tv_nsec;
}

Event MakeEvent(int32_t handle, SensorType type, int64_t timestamp_ns) {
    Event event;
    event.sensorHandle = handle;
    event.sensorType = type;
    event.timestamp = timestamp_ns;
    return event;
}

Event MakeVec3Event(int32_t handle, SensorType type, int64_t timestamp_ns, float x, float y,
                    float z, SensorStatus status) {
    Event event = MakeEvent(handle, type, timestamp_ns);
    EventPayload::Vec3 vec3;
    vec3.x = x;
    vec3.y = y;
    vec3.z = z;
    vec3.status = status;
    event.payload.set<EventPayload::Tag::vec3>(vec3);
    return event;
}

Event MakeScalarEvent(int32_t handle, SensorType type, int64_t timestamp_ns, float value) {
    Event event = MakeEvent(handle, type, timestamp_ns);
    event.payload.set<EventPayload::Tag::scalar>(value);
    return event;
}

Event MakeStepCountEvent(int32_t handle, int64_t timestamp_ns, int64_t steps) {
    Event event = MakeEvent(handle, SensorType::STEP_COUNTER, timestamp_ns);
    event.payload.set<EventPayload::Tag::stepCount>(steps);
    return event;
}

Event MakeVec4Event(int32_t handle, SensorType type, int64_t timestamp_ns, float x, float y,
                    float z, float w) {
    Event event = MakeEvent(handle, type, timestamp_ns);
    EventPayload::Vec4 vec4;
    vec4.x = x;
    vec4.y = y;
    vec4.z = z;
    vec4.w = w;
    event.payload.set<EventPayload::Tag::vec4>(vec4);
    return event;
}

Event MakeRotationVectorEvent(int32_t handle, SensorType type, int64_t timestamp_ns, float x,
                              float y, float z, float w, float accuracy_rad) {
    Event event = MakeEvent(handle, type, timestamp_ns);
    EventPayload::Data data;
    data.values[0] = x;
    data.values[1] = y;
    data.values[2] = z;
    data.values[3] = w;
    data.values[4] = accuracy_rad;
    event.payload.set<EventPayload::Tag::data>(data);
    return event;
}

Event MakeFlushCompleteEvent(int32_t handle) {
    Event event = MakeEvent(handle, SensorType::META_DATA, 0);
    EventPayload::MetaData meta;
    meta.what = EventPayload::MetaData::MetaDataEventType::META_DATA_FLUSH_COMPLETE;
    event.payload.set<EventPayload::Tag::meta>(meta);
    return event;
}

bool IsFlushCompleteEvent(const Event& event) {
    return event.sensorType == SensorType::META_DATA &&
           event.payload.getTag() == EventPayload::Tag::meta &&
           event.payload.get<EventPayload::Tag::meta>().what ==
                   EventPayload::MetaData::MetaDataEventType::META_DATA_FLUSH_COMPLETE;
}

bool HaveSamePayload(const Event& a, const Event& b) {
    return a.payload == b.payload;
}

std::string EventToString(const Event& event) {
    std::string payload;
    switch (event.payload.getTag()) {
        case EventPayload::Tag::vec3: {
            const auto& v = event.payload.get<EventPayload::Tag::vec3>();
            payload = ::android::base::StringPrintf("vec3=(%g, %g, %g) status=%d", v.x, v.y, v.z,
                                                    static_cast<int>(v.status));
            break;
        }
        case EventPayload::Tag::vec4: {
            const auto& v = event.payload.get<EventPayload::Tag::vec4>();
            payload = ::android::base::StringPrintf("vec4=(%g, %g, %g, %g)", v.x, v.y, v.z, v.w);
            break;
        }
        case EventPayload::Tag::scalar:
            payload = ::android::base::StringPrintf("scalar=%g",
                                                    event.payload.get<EventPayload::Tag::scalar>());
            break;
        case EventPayload::Tag::stepCount:
            payload = ::android::base::StringPrintf(
                    "steps=%" PRId64, event.payload.get<EventPayload::Tag::stepCount>());
            break;
        case EventPayload::Tag::meta:
            payload = ::android::base::StringPrintf(
                    "meta=%d", static_cast<int>(event.payload.get<EventPayload::Tag::meta>().what));
            break;
        case EventPayload::Tag::data: {
            const auto& d = event.payload.get<EventPayload::Tag::data>().values;
            payload = ::android::base::StringPrintf("data=(%g, %g, %g, %g, %g, ...)", d[0], d[1],
                                                    d[2], d[3], d[4]);
            break;
        }
        default:
            payload = ::android::base::StringPrintf("payload_tag=%d",
                                                    static_cast<int>(event.payload.getTag()));
            break;
    }
    return ::android::base::StringPrintf("handle=%d type=%s ts=%" PRId64 " %s", event.sensorHandle,
                                         toString(event.sensorType).c_str(), event.timestamp,
                                         payload.c_str());
}

std::string SensorInfoToString(const SensorInfo& info) {
    return ::android::base::StringPrintf(
            "handle=%d type=%s name='%s' vendor='%s' version=%d maxRange=%g resolution=%g "
            "power=%g minDelayUs=%d maxDelayUs=%d flags=0x%x",
            info.sensorHandle, toString(info.type).c_str(), info.name.c_str(), info.vendor.c_str(),
            info.version, info.maxRange, info.resolution, info.power, info.minDelayUs,
            info.maxDelayUs, info.flags);
}

}  // namespace aidl::android::hardware::sensors::mainline
