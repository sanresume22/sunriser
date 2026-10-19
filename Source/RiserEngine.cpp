#include "RiserEngine.h"
#include <cmath>

namespace sunriser
{
//==============================================================================
float swellGainAt (float p, float swell)  { return juce::Decibels::decibelsToGain (-30.0f * swell * std::pow (1.0f - p, 1.5f)); }
float pitchAt     (float p, float pitch)  { return pitch * std::pow (p, 1.2f); }
float highpassAt  (float p, float sweep)  { return 20.0f + sweep * 500.0f * p * p * p; }
float lowpassAt   (float p, float sweep)
{
    const float startHz = 20000.0f * std::pow (250.0f / 20000.0f, sweep);
    return startHz * std::pow (20000.0f / startHz, p);
}

//==============================================================================
static inline float hermite (const float* d, int n, double pos)
{
    const int i = (int) pos;
    const float f = (float) (pos - (double) i);
    auto at = [d, n] (int k) { return d[juce::jlimit (0, n - 1, k)]; };
    const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

//==============================================================================
SyllableRange detectFirstSyllable (const juce::AudioBuffer<float>& src, double sr)
{
    const int n = src.getNumSamples(), nch = juce::jmax (1, src.getNumChannels());
    SyllableRange r { 0, n };
    if (n == 0 || sr <= 0) return r;

    const int hop = juce::jmax (32, (int) (sr * 0.005)); // 5 ms frames
    const int frames = n / hop;
    if (frames < 4) return r;

    // RMS envelope
    std::vector<float> raw ((size_t) frames), env ((size_t) frames);
    for (int f = 0; f < frames; ++f)
    {
        double sum = 0;
        for (int ch = 0; ch < src.getNumChannels(); ++ch)
        {
            const float* d = src.getReadPointer (ch) + f * hop;
            for (int i = 0; i < hop; ++i) sum += (double) d[i] * d[i];
        }
        raw[(size_t) f] = (float) std::sqrt (sum / (double) (hop * nch));
    }
    float peak = 0.0f;
    for (int f = 0; f < frames; ++f)
    {
        const float a = raw[(size_t) juce::jmax (0, f - 1)], b = raw[(size_t) f], c = raw[(size_t) juce::jmin (frames - 1, f + 1)];
        env[(size_t) f] = (a + b + c) / 3.0f;
        peak = juce::jmax (peak, env[(size_t) f]);
    }
    if (peak < 1.0e-5f) return r;

    // Onset: first frame above -20 dB of the loudest point, backed up to the start of the attack
    const float onTh = peak * 0.1f;
    int on = 0;
    while (on < frames && env[(size_t) on] < onTh) ++on;
    if (on >= frames) return r;

    int st = on;
    const int back = (int) (0.03 * sr / hop);
    while (st > 0 && on - st < back && env[(size_t) (st - 1)] > onTh * 0.25f) --st;

    // End: the level falls well below the syllable's peak, or a dip is followed by a new onset
    const int minF = juce::jmax (2, (int) (0.06 * sr / hop));
    const int maxF = (int) (0.6 * sr / hop);
    const int lastF = juce::jmin (frames, st + maxF);
    float localPeak = 0.0f, valley = 1.0e9f;
    int valleyF = -1, quietRun = 0, endF = lastF;

    for (int f = st; f < lastF; ++f)
    {
        const float e = env[(size_t) f];
        if (e > localPeak) { localPeak = e; valley = 1.0e9f; valleyF = -1; }
        if (f - st < minF) continue;

        if (e < localPeak * 0.18f) { if (++quietRun >= 3) { endF = f - 1; break; } }
        else quietRun = 0;

        if (e < valley) { valley = e; valleyF = f; }
        if (valleyF >= 0 && valley < localPeak * 0.5f && e > valley * 2.5f && f - valleyF >= 2)
        {
            endF = valleyF;
            break;
        }
    }

    r.start = st * hop;
    r.end   = juce::jlimit (r.start + (int) (0.04 * sr), n, endF * hop);
    return r;
}

//==============================================================================
RenderedRiser::Ptr renderRiser (const juce::AudioBuffer<float>& slice, double sliceRate, const RiserSettings& s)
{
    RenderedRiser::Ptr r = new RenderedRiser();
    r->settings = s;

    const double sr     = juce::jlimit (8000.0, 768000.0, s.sampleRate);
    const double qSec   = 60.0 / juce::jlimit (20.0, 999.0, s.bpm);
    const double barQ   = s.beatsNum * 4.0 / (double) juce::jmax (1, s.beatsDen);
    const double totalQ = juce::jmax (1, s.bars) * barQ;
    const double L      = totalQ * qSec;
    const int    N      = juce::jmax (1, (int) std::ceil (L * sr));
    const double startQ = juce::jmax (s.startDivQ, s.endDivQ);
    const double endQ   = juce::jmin (s.startDivQ, s.endDivQ);

    r->lengthSec   = L;
    r->beatsPerBar = barQ;
    r->audio.setSize (2, N);
    r->audio.clear();

    //== 1. Schedule the repeats ===============================================
    auto addGrain = [&] (double tq, double lenQ)
    {
        const float p = (float) (tq / totalQ);
        Grain g;
        g.startSec  = tq * qSec;
        g.lengthSec = juce::jmax (0.0, lenQ) * qSec;
        g.gain      = swellGainAt (p, s.swell);
        g.semis     = pitchAt (p, s.pitch);
        g.pan       = ((r->grains.size() & 1) ? 1.0f : -1.0f) * s.width * p;
        r->grains.push_back (g);
    };

    if (! s.stepMode)
    {
        // Smooth: the repeat length glides exponentially from startQ to endQ
        const double expo = 0.5 + s.curve * 2.0;
        double tq = 0.0;
        while (tq < totalQ - 1.0e-6 && r->grains.size() < 4000)
        {
            const double p  = tq / totalQ;
            const double iq = startQ * std::pow (endQ / startQ, std::pow (p, expo));
            addGrain (tq, juce::jmin (iq * s.chop, totalQ - tq));
            tq += iq;
        }
    }
    else
    {
        // Steps: classic snare-roll doubling (1/4 -> 1/8 -> 1/16 ...), every hit on the grid
        const int stages = juce::jmax (1, (int) std::lround (std::log2 (startQ / endQ)) + 1);
        const double shape = 1.0 / (1.0 + s.curve * 1.5); // higher curve = longer slow sections
        std::vector<double> bounds ((size_t) stages + 1, 0.0);
        bounds[(size_t) stages] = totalQ;
        for (int k = 1; k < stages; ++k)
        {
            const double prevI = startQ / std::pow (2.0, k - 1);
            const double ideal = totalQ * std::pow ((double) k / stages, shape);
            bounds[(size_t) k] = juce::jlimit (bounds[(size_t) k - 1], totalQ, std::round (ideal / prevI) * prevI);
        }
        for (int k = 0; k < stages; ++k)
        {
            const double I = startQ / std::pow (2.0, k);
            for (double tq = bounds[(size_t) k]; tq < bounds[(size_t) k + 1] - 1.0e-6; tq += I)
                addGrain (tq, juce::jmin (I * s.chop, bounds[(size_t) k + 1] - tq));
        }
    }

    //== 2. Play the syllable for each repeat =================================
    const int sliceLen = slice.getNumSamples();
    if (sliceLen < 4 || slice.getNumChannels() == 0)
        return r;

    const float* srcL = slice.getReadPointer (0);
    const float* srcR = slice.getReadPointer (slice.getNumChannels() > 1 ? 1 : 0);
    float* outL = r->audio.getWritePointer (0);
    float* outR = r->audio.getWritePointer (1);
    const double baseInc  = sliceRate / sr;
    const double srcFade  = juce::jmax (1.0, 0.004 * sliceRate);

    for (const auto& g : r->grains)
    {
        const int s0  = (int) std::llround (g.startSec * sr);
        const int len = juce::jmin ((int) std::llround (g.lengthSec * sr), N - s0);
        if (len <= 0 || s0 < 0) continue;

        const int fadeIn  = juce::jmax (1, juce::jmin (len / 4, (int) (0.002 * sr)));
        const int fadeOut = juce::jmax (1, juce::jmin (len / 3, (int) (0.006 * sr)));
        const float gl = juce::jmin (1.0f, 1.0f - g.pan);
        const float gr = juce::jmin (1.0f, 1.0f + g.pan);
        const double stepInc = std::exp2 (g.semis / 12.0) * baseInc;

        double pos = 0.0;
        for (int i = 0; i < len; ++i)
        {
            if (pos >= sliceLen - 1) break;

            float env = 1.0f;
            if (i < fadeIn)        env = (float) i / (float) fadeIn;
            if (len - i < fadeOut) env *= (float) (len - i) / (float) fadeOut;
            const double srcLeft = (sliceLen - 1) - pos;
            if (srcLeft < srcFade) env *= (float) (srcLeft / srcFade);

            const float a = env * g.gain;
            outL[s0 + i] += hermite (srcL, sliceLen, pos) * a * gl;
            outR[s0 + i] += hermite (srcR, sliceLen, pos) * a * gr;

            // Smooth mode glides the pitch continuously, Steps mode keeps each hit on one pitch
            pos += s.stepMode ? stepInc
                              : std::exp2 (pitchAt ((float) (s0 + i) / (float) N, s.pitch) / 12.0) * baseInc;
        }
    }

    //== 3. Filter sweep (low-pass opens, high-pass thins the lows into the drop) ==
    if (s.sweep > 0.001f)
    {
        juce::dsp::StateVariableTPTFilter<float> lp, hp;
        const juce::dsp::ProcessSpec spec { sr, 512, 2 };
        lp.prepare (spec); lp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);  lp.setResonance (0.9f);
        hp.prepare (spec); hp.setType (juce::dsp::StateVariableTPTFilterType::highpass); hp.setResonance (0.7071f);
        const float nyq = (float) (sr * 0.45);

        for (int i = 0; i < N; ++i)
        {
            if ((i & 15) == 0)
            {
                const float p = (float) i / (float) N;
                lp.setCutoffFrequency (juce::jmin (nyq, lowpassAt (p, s.sweep)));
                hp.setCutoffFrequency (juce::jmin (nyq, highpassAt (p, s.sweep)));
            }
            outL[i] = hp.processSample (0, lp.processSample (0, outL[i]));
            outR[i] = hp.processSample (1, lp.processSample (1, outR[i]));
        }
    }

    //== 4. Reverb that builds towards the drop ================================
    if (s.space > 0.001f)
    {
        juce::Reverb reverb;
        reverb.setSampleRate (sr);
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.86f; rp.damping = 0.35f; rp.width = 1.0f; rp.freezeMode = 0.0f;

        for (int i = 0; i < N; i += 256)
        {
            const int n = juce::jmin (256, N - i);
            const float p = (float) (i + n / 2) / (float) N;
            rp.wetLevel = s.space * 0.33f * std::pow (p, 1.2f);
            rp.dryLevel = 0.5f * (1.0f - 0.3f * s.space * p); // juce::Reverb doubles the dry level
            reverb.setParameters (rp);
            reverb.processStereo (outL + i, outR + i, n);
        }
    }

    //== 5. Tidy up: tiny end fade, normalise to -1 dBFS ========================
    const int endFade = juce::jmin (N, (int) (0.003 * sr));
    for (int i = 0; i < endFade; ++i)
    {
        const float gn = (float) i / (float) juce::jmax (1, endFade);
        outL[N - 1 - i] *= gn;
        outR[N - 1 - i] *= gn;
    }
    const float pk = r->audio.getMagnitude (0, N);
    if (pk > 1.0e-6f)
        r->audio.applyGain (0.89f / pk);

    return r;
}
} // namespace sunriser
