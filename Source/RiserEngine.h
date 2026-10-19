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
enum Engine { engineStutter = 0, engineReverse = 1, engineBoth = 2 };

struct RiserSettings
{
    int    engine     = engineStutter;
    int    bars       = 2;
    int    beatsNum   = 4, beatsDen = 4;
    double bpm        = 128.0;
    double sampleRate = 48000.0;
    bool   land       = false;     // play the dry syllable right on the drop

    // Stutter
    bool   stepMode   = false;
    double startDivQ  = 0.5;    // repeat length at the start, in quarter notes
    double endDivQ    = 0.125;  // repeat length at the end, in quarter notes
    float  curve = 0.35f, chop = 0.85f, pitch = 12.0f, sweep = 0.65f,
           space = 0.35f, swell = 0.7f, width = 0.5f;

    // Reverse swell
    float  revSize = 0.8f, revTone = 0.55f, revPreDelayMs = 0.0f;   // revPreDelayMs = gap before the drop
    float  blend = 0.5f;        // Both mode: 0 = all stutter, 1 = all reverse

    bool operator== (const RiserSettings& o) const
    {
        using juce::exactlyEqual;
        return engine == o.engine && bars == o.bars && beatsNum == o.beatsNum && beatsDen == o.beatsDen
            && exactlyEqual (bpm, o.bpm) && exactlyEqual (sampleRate, o.sampleRate) && land == o.land && stepMode == o.stepMode
            && exactlyEqual (startDivQ, o.startDivQ) && exactlyEqual (endDivQ, o.endDivQ)
            && exactlyEqual (curve, o.curve) && exactlyEqual (chop, o.chop) && exactlyEqual (pitch, o.pitch)
            && exactlyEqual (sweep, o.sweep) && exactlyEqual (space, o.space) && exactlyEqual (swell, o.swell)
            && exactlyEqual (width, o.width) && exactlyEqual (revSize, o.revSize) && exactlyEqual (revTone, o.revTone)
            && exactlyEqual (revPreDelayMs, o.revPreDelayMs) && exactlyEqual (blend, o.blend);
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
    double lengthSec   = 0.0;           // whole buffer (riser + anything after the drop)
    double dropSec     = 0.0;           // where the drop / bar line is
    double landSec     = 0.0;           // length of the dry syllable after the drop (0 = off)
    double beatsPerBar = 4.0;           // in quarter notes
    std::vector<Grain> grains;          // stutter hits (empty in Reverse mode)
    std::vector<float> swellEnvelope;   // reverse swell level over the whole buffer, 0..1
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
