//
// Copyright 2017 Valve Corporation. All rights reserved. Subject to the following license:
// https://valvesoftware.github.io/steam-audio/license.html
//

#include "steamaudio_unity_native.h"

#include <phonon_interfaces.h>

// --------------------------------------------------------------------------------------------------------------------
// Global State
// --------------------------------------------------------------------------------------------------------------------

IPLContext gContext = nullptr;
IPLHRTF gHRTF[2] = { nullptr, nullptr };
IPLSimulationSettings gSimulationSettings;
IPLSource gReverbSource[2] = { nullptr, nullptr };
IPLReflectionMixer gReflectionMixer[2] = { nullptr, nullptr };

std::atomic<bool> gNewHRTFWritten{ false };
std::atomic<bool> gIsSimulationSettingsValid{ false };
std::atomic<bool> gNewReverbSourceWritten{ false };
std::atomic<bool> gNewReflectionMixerWritten{ false };


// --------------------------------------------------------------------------------------------------------------------
// API Functions
// --------------------------------------------------------------------------------------------------------------------

extern UnityAudioEffectDefinition gSpatializeEffectDefinition;
extern UnityAudioEffectDefinition gAmbisonicDecoderEffectDefinition;
extern UnityAudioEffectDefinition gMixerReturnEffectDefinition;
extern UnityAudioEffectDefinition gReverbEffectDefinition;

int UnityGetAudioEffectDefinitions(UnityAudioEffectDefinition*** definitions)
{
    static UnityAudioEffectDefinition* effects[] = { 
        &gMixerReturnEffectDefinition, 
        &gReverbEffectDefinition,
        &gSpatializeEffectDefinition,
        &gAmbisonicDecoderEffectDefinition 
    };

    *definitions = effects;
    return (sizeof(effects) / sizeof(effects[0]));
}

void UNITY_AUDIODSP_CALLBACK iplUnityGetVersion(unsigned int* major, unsigned int* minor, unsigned int* patch)
{
    if (major)
        *major = STEAMAUDIO_UNITY_VERSION_MAJOR;
    if (minor)
        *minor = STEAMAUDIO_UNITY_VERSION_MINOR;
    if (patch)
        *patch = STEAMAUDIO_UNITY_VERSION_PATCH;
}

void UNITY_AUDIODSP_CALLBACK iplUnityInitialize(IPLContext context)
{
    assert(gContext == nullptr);

    gContext = iplContextRetain(context);
}

void UNITY_AUDIODSP_CALLBACK iplUnityTerminate()
{
    gNewReflectionMixerWritten = false;
    iplReflectionMixerRelease(&gReflectionMixer[0]);
    iplReflectionMixerRelease(&gReflectionMixer[1]);

    gNewReverbSourceWritten = false;
    iplSourceRelease(&gReverbSource[0]);
    iplSourceRelease(&gReverbSource[1]);

    gIsSimulationSettingsValid = false;

    gNewHRTFWritten = false;
    iplHRTFRelease(&gHRTF[0]);
    iplHRTFRelease(&gHRTF[1]);

    iplContextRelease(&gContext);
}

void UNITY_AUDIODSP_CALLBACK iplUnitySetHRTF(IPLHRTF hrtf)
{
    if (hrtf == gHRTF[1])
        return;

    setHRTF(hrtf);
}

void UNITY_AUDIODSP_CALLBACK iplUnitySetSimulationSettings(IPLSimulationSettings simulationSettings)
{
    gSimulationSettings = simulationSettings;

    gIsSimulationSettingsValid = true;
}

void UNITY_AUDIODSP_CALLBACK iplUnitySetReverbSource(IPLSource reverbSource)
{
    if (reverbSource == gReverbSource[1])
        return;

    if (!gNewReverbSourceWritten)
    {
        iplSourceRelease(&gReverbSource[1]);
        gReverbSource[1] = iplSourceRetain(reverbSource);

        gNewReverbSourceWritten = true;
    }
}


// --------------------------------------------------------------------------------------------------------------------
// Helper Functions
// --------------------------------------------------------------------------------------------------------------------

IPLSpeakerLayout speakerLayoutForNumChannels(int numChannels)
{
    IPLSpeakerLayout speakerLayout;
    speakerLayout.numSpeakers = numChannels;
    speakerLayout.speakers = nullptr;

    if (numChannels == 1)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_MONO;
    else if (numChannels == 2)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    else if (numChannels == 4)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_QUADRAPHONIC;
    else if (numChannels == 6)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_5_1;
    else if (numChannels == 8)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1;
    else
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_CUSTOM;

    return speakerLayout;
}

int orderForNumChannels(int numChannels)
{
    return static_cast<int>(sqrtf(static_cast<float>(numChannels))) - 1;
}

int numChannelsForOrder(int order)
{
    return (order + 1) * (order + 1);
}

int numSamplesForDuration(float duration,
                          int samplingRate)
{
    return static_cast<int>(ceilf(duration * samplingRate));
}

IPLVector3 convertVector(float x,
                         float y, 
                         float z)
{
    return IPLVector3{ x, y, -z };
}

IPLVector3 unitVector(IPLVector3 v)
{
    auto length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-2f)
        length = 1e-2f;

    return IPLVector3{ v.x / length, v.y / length, v.z / length };
}

float dot(const IPLVector3& a, 
          const IPLVector3& b)
{
    return (a.x * b.x + a.y * b.y + a.z * b.z);
}

IPLVector3 cross(const IPLVector3& a,
                 const IPLVector3& b)
{
    IPLVector3 c;
    c.x = a.y * b.z - a.z * b.y;
    c.y = a.z * b.x - a.x * b.z;
    c.z = a.x * b.y - a.y * b.x;
    return c;
}

void applyVolumeRamp(float startVolume, 
                     float endVolume, 
                     int numSamples, 
                     float* buffer)
{
    for (auto i = 0; i < numSamples; ++i)
    {
        auto fraction = static_cast<float>(i) / static_cast<float>(numSamples);
        auto volume = fraction * endVolume + (1.0f - fraction) * startVolume;

        buffer[i] *= volume;
    }
}

//void crossfadeInputAndOutput(const float* inBuffer, const int numChannels, const int numSamples, float* outBuffer)
//{
//    auto step = 1.0f / (numSamples - 1);
//    auto weight = 0.0f;
//
//    for (auto i = 0, index = 0; i < numSamples; ++i, weight += step)
//        for (auto j = 0; j < numChannels; ++j, ++index)
//            outBuffer[index] = weight * outBuffer[index] + (1.0f - weight) * inBuffer[index];
//}

IPLCoordinateSpace3 calcSourceCoordinates(const float* sourceMatrix)
{
    auto S = sourceMatrix;

    IPLCoordinateSpace3 sourceCoordinates;
    sourceCoordinates.origin = convertVector(S[12], S[13], S[14]);
    sourceCoordinates.up = unitVector(convertVector(S[4], S[5], S[6]));
    sourceCoordinates.ahead = unitVector(convertVector(S[8], S[9], S[10]));
    sourceCoordinates.right = unitVector(cross(sourceCoordinates.ahead, sourceCoordinates.up));
    return sourceCoordinates;
}

IPLCoordinateSpace3 calcListenerCoordinates(const float* listenerMatrix)
{
    auto L = listenerMatrix;

    auto listenerScaleSquared = 1.0f / (L[1] * L[1] + L[5] * L[5] + L[9] * L[9]);

    auto Lx = -listenerScaleSquared * (L[0] * L[12] + L[1] * L[13] + L[2] * L[14]);
    auto Ly = -listenerScaleSquared * (L[4] * L[12] + L[5] * L[13] + L[6] * L[14]);
    auto Lz = -listenerScaleSquared * (L[8] * L[12] + L[9] * L[13] + L[10] * L[14]);

    IPLCoordinateSpace3 listenerCoordinates;
    listenerCoordinates.origin = convertVector(Lx, Ly, Lz);
    listenerCoordinates.up = unitVector(convertVector(L[1], L[5], L[9]));
    listenerCoordinates.ahead = unitVector(convertVector(L[2], L[6], L[10]));
    listenerCoordinates.right = unitVector(cross(listenerCoordinates.ahead, listenerCoordinates.up));
    return listenerCoordinates;
}

void getLatestHRTF()
{
    if (gNewHRTFWritten)
    {
        iplHRTFRelease(&gHRTF[0]);
        gHRTF[0] = iplHRTFRetain(gHRTF[1]);

        gNewHRTFWritten = false;
    }
}

void setHRTF(IPLHRTF hrtf)
{
    if (!gNewHRTFWritten)
    {
        iplHRTFRelease(&gHRTF[1]);
        gHRTF[1] = iplHRTFRetain(hrtf);

        gNewHRTFWritten = true;
    }
}
