#pragma once

// A stand-in for Apple's <CoreAudio/CoreAudio.h>, covering exactly the surface
// CoreAudioBackend.cpp uses and nothing more.
//
// The point is not to reimplement CoreAudio. It is to let the real, unmodified
// backend source compile and run on a machine that has no CoreAudio, driven by
// a virtual HAL (FakeCoreAudio.h) that can be configured to behave like the
// hardware people actually own -- interleaved stereo microphones, devices that
// advertise a continuous sample-rate range, devices that refuse hog mode.
//
// Every declaration here matches Apple's in type and semantics. Where a
// constant's numeric value is observable (the four-character property
// selectors) the real value is used, so a typo in the backend fails here the
// same way it would on macOS.

#include <cstdint>
#include <cstddef>

/// CoreAudio spells its selectors as four-character codes. Written out rather
/// than as multi-character literals, which are implementation-defined and warn.
constexpr uint32_t mmaFourCC (char a, char b, char c, char d)
{
    return (static_cast<uint32_t> (static_cast<unsigned char> (a)) << 24)
         | (static_cast<uint32_t> (static_cast<unsigned char> (b)) << 16)
         | (static_cast<uint32_t> (static_cast<unsigned char> (c)) << 8)
         |  static_cast<uint32_t> (static_cast<unsigned char> (d));
}

using OSStatus = int32_t;
using UInt32   = uint32_t;
using SInt32   = int32_t;
using Float64  = double;
using Boolean  = unsigned char;

constexpr OSStatus noErr = 0;
constexpr OSStatus kAudioHardwareUnspecifiedError = mmaFourCC ('w', 'h', 'a', 't');
constexpr OSStatus kAudioHardwareBadObjectError   = mmaFourCC ('!', 'o', 'b', 'j');
constexpr OSStatus kAudioHardwareUnknownPropertyError = mmaFourCC ('w', 'h', 'o', '?');

// --- CoreFoundation strings -------------------------------------------------
// Only the three calls the backend makes are provided. A CFStringRef here is an
// owning handle the caller releases, exactly as on macOS, so a missing
// CFRelease shows up as a leak in the simulation too.
struct __CFString;
using CFStringRef = const __CFString*;
using CFStringEncoding = UInt32;
constexpr CFStringEncoding kCFStringEncodingUTF8 = 0x08000100;

Boolean CFStringGetCString (CFStringRef value, char* buffer, long bufferSize, CFStringEncoding encoding);
void CFRelease (CFStringRef value);

// --- Objects and properties -------------------------------------------------
using AudioObjectID = UInt32;
using AudioObjectPropertySelector = UInt32;
using AudioObjectPropertyScope = UInt32;
using AudioObjectPropertyElement = UInt32;

constexpr AudioObjectID kAudioObjectUnknown = 0;
constexpr AudioObjectID kAudioObjectSystemObject = 1;

constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeGlobal = mmaFourCC ('g', 'l', 'o', 'b');
constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeInput  = mmaFourCC ('i', 'n', 'p', 't');
constexpr AudioObjectPropertyScope kAudioObjectPropertyScopeOutput = mmaFourCC ('o', 'u', 't', 'p');
constexpr AudioObjectPropertyElement kAudioObjectPropertyElementMain = 0;

constexpr AudioObjectPropertySelector kAudioObjectPropertyName = mmaFourCC ('l', 'n', 'a', 'm');
constexpr AudioObjectPropertySelector kAudioHardwarePropertyDevices = mmaFourCC ('d', 'e', 'v', '#');
constexpr AudioObjectPropertySelector kAudioDevicePropertyDeviceUID = mmaFourCC ('u', 'i', 'd', ' ');
constexpr AudioObjectPropertySelector kAudioDevicePropertyStreamConfiguration = mmaFourCC ('s', 'l', 'a', 'y');
constexpr AudioObjectPropertySelector kAudioDevicePropertyNominalSampleRate = mmaFourCC ('n', 's', 'r', 't');
constexpr AudioObjectPropertySelector kAudioDevicePropertyAvailableNominalSampleRates = mmaFourCC ('n', 's', 'r', '#');
constexpr AudioObjectPropertySelector kAudioDevicePropertyBufferFrameSize = mmaFourCC ('f', 's', 'i', 'z');
constexpr AudioObjectPropertySelector kAudioDevicePropertyHogMode = mmaFourCC ('o', 'i', 'n', 'k');

struct AudioObjectPropertyAddress
{
    AudioObjectPropertySelector mSelector;
    AudioObjectPropertyScope mScope;
    AudioObjectPropertyElement mElement;
};

struct AudioValueRange
{
    Float64 mMinimum;
    Float64 mMaximum;
};

// --- Buffers ----------------------------------------------------------------
struct AudioBuffer
{
    UInt32 mNumberChannels;
    UInt32 mDataByteSize;
    void* mData;
};

// Apple declares the trailing array as [1] and over-allocates. The backend
// indexes past it for multi-buffer devices, which is correct against Apple's
// layout, so the same declaration is used here rather than a "safer" one that
// would not reproduce the real memory layout.
struct AudioBufferList
{
    UInt32 mNumberBuffers;
    AudioBuffer mBuffers[1];
};

struct AudioTimeStamp
{
    Float64 mSampleTime;
    uint64_t mHostTime;
    Float64 mRateScalar;
    uint64_t mWordClockTime;
    UInt32 mFlags;
    UInt32 mReserved;
};

// --- Calls ------------------------------------------------------------------
using AudioObjectPropertyListenerProc = OSStatus (*) (AudioObjectID,
                                                      UInt32,
                                                      const AudioObjectPropertyAddress*,
                                                      void*);

using AudioDeviceIOProc = OSStatus (*) (AudioObjectID,
                                        const AudioTimeStamp*,
                                        const AudioBufferList*,
                                        const AudioTimeStamp*,
                                        AudioBufferList*,
                                        const AudioTimeStamp*,
                                        void*);

using AudioDeviceIOProcID = AudioDeviceIOProc;

OSStatus AudioObjectGetPropertyDataSize (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         UInt32 qualifierDataSize,
                                         const void* qualifierData,
                                         UInt32* outSize);

OSStatus AudioObjectGetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32 qualifierDataSize,
                                     const void* qualifierData,
                                     UInt32* ioSize,
                                     void* outData);

OSStatus AudioObjectSetPropertyData (AudioObjectID object,
                                     const AudioObjectPropertyAddress* address,
                                     UInt32 qualifierDataSize,
                                     const void* qualifierData,
                                     UInt32 inSize,
                                     const void* inData);

OSStatus AudioObjectAddPropertyListener (AudioObjectID object,
                                         const AudioObjectPropertyAddress* address,
                                         AudioObjectPropertyListenerProc listener,
                                         void* clientData);

OSStatus AudioObjectRemovePropertyListener (AudioObjectID object,
                                            const AudioObjectPropertyAddress* address,
                                            AudioObjectPropertyListenerProc listener,
                                            void* clientData);

OSStatus AudioDeviceCreateIOProcID (AudioObjectID device,
                                    AudioDeviceIOProc proc,
                                    void* clientData,
                                    AudioDeviceIOProcID* outProcId);

OSStatus AudioDeviceDestroyIOProcID (AudioObjectID device, AudioDeviceIOProcID procId);
OSStatus AudioDeviceStart (AudioObjectID device, AudioDeviceIOProcID procId);
OSStatus AudioDeviceStop (AudioObjectID device, AudioDeviceIOProcID procId);
