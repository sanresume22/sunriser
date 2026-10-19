#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace sunriser
{
//==============================================================================
/** Finds the first syllable / transient event in a sound (sample indices). */
struct SyllableRange { int start = 0, end = 0; };
SyllableRange detectFirstSyllable (const juce::AudioBuffer<float>& source, double sampleRate);

//==============================================================================
struct RiserSettings
{
    int    bars       = 2;
    int    beatsNum   = 4, beatsDen = 4;
    double bpm        = 128.0;
    double sampleRate = 48000.0;
    bool   stepMode   = false;
    double startDivQ  = 0.5;    // repeat length at the start, in quarter notes
    double endDivQ    = 0.125;  // repeat length at the end, in quarter notes
    float  curve = 0.35f, chop = 0.85f, pitch = 12.0f, sweep = 0.65f,
           space = 0.35f, swell = 0.7f, width = 0.5f;

    bool operator== (const RiserSettings& o) const
    {
        return bars == o.bars && beatsNum == o.beatsNum && beatsDen == o.beatsDen
            && bpm == o.bpm && sampleRate == o.sampleRate && stepMode == o.stepMode
            && startDivQ == o.startDivQ && endDivQ == o.endDivQ
            && curve == o.curve && chop == o.chop && pitch == o.pitch && sweep == o.sweep
            && space == o.space && swell == o.swell && width == o.width;
    }
    bool operator!= (const RiserSettings& o) const { return ! (*this == o); }
};

struct Grain
{
    double startSec = 0, lengthSec = 0;
    float  gain = 1.0f, semis = 0.0f, pan = 0.0f;
};

struct RenderedRiser : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<RenderedRiser>;

    juce::AudioBuffer<float> audio;     // always stereo
    double lengthSec   = 0.0;
    double beatsPerBar = 4.0;           // in quarter notes
    std::vector<Grain> grains;
    RiserSettings settings;
};

/** Renders a complete riser from a (stereo) syllable slice. Not real-time: run on a background thread. */
RenderedRiser::Ptr renderRiser (const juce::AudioBuffer<float>& slice, double sliceRate, const RiserSettings& settings);

//==============================================================================
// Automation curves over riser progress p (0..1). Shared by the renderer and the GUI.
float swellGainAt (float p, float swell);
float pitchAt     (float p, float pitchSemis);
float lowpassAt   (float p, float sweep);
float highpassAt  (float p, float sweep);
} // namespace sunriser
