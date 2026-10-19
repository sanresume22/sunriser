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
// Stutter engine: accelerating, pitch-rising repeats of the syllable (length N = up to the drop)
static void renderStutter (const juce::AudioBuffer<float>& slice, double sliceRate, const RiserSettings& s,
                           double sr, double qSec, double totalQ, int N,
                           juce::AudioBuffer<float>& out, std::vector<Grain>& grains)
{
    out.setSize (2, N);
    out.clear();
    const double startQ = juce::jmax (s.startDivQ, s.endDivQ);
    const double endQ   = juce::jmin (s.startDivQ, s.endDivQ);

    //== 1. Schedule the repeats ===============================================
    auto addGrain = [&] (double tq, double lenQ)
    {
        const float p = (float) (tq / totalQ);
        Grain g;
        g.startSec  = tq * qSec;
        g.lengthSec = juce::jmax (0.0, lenQ) * qSec;
        g.gain      = swellGainAt (p, s.swell);
        g.semis     = pitchAt (p, s.pitch);
        g.pan       = ((grains.size() & 1) ? 1.0f : -1.0f) * s.width * p;
        grains.push_back (g);
    };

    if (! s.stepMode)
    {
        // Smooth: the repeat length glides exponentially from startQ to endQ
        const double expo = 0.5 + s.curve * 2.0;
        double tq = 0.0;
        while (tq < totalQ - 1.0e-6 && grains.size() < 4000)
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
        return;

    const float* srcL = slice.getReadPointer (0);
    const float* srcR = slice.getReadPointer (slice.getNumChannels() > 1 ? 1 : 0);
    float* outL = out.getWritePointer (0);
    float* outR = out.getWritePointer (1);
    const double baseInc = sliceRate / sr;
    const double srcFade = juce::jmax (1.0, 0.004 * sliceRate);

    for (const auto& g : grains)
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

    // tiny fade into the drop
    const int endFade = juce::jmin (N, (int) (0.003 * sr));
    for (int i = 0; i < endFade; ++i)
    {
        const float gn = (float) i / (float) juce::jmax (1, endFade);
        outL[N - 1 - i] *= gn;
        outR[N - 1 - i] *= gn;
    }
}

//==============================================================================
/** Dense stereo feedback-delay-network reverb with an exact RT60, used for the reverse swell. */
class LongReverb
{
public:
    LongReverb (double sr, float rt60Sec, float tone)
    {
        static const float lineMs[kLines] = { 31.3f, 37.9f, 41.7f, 47.3f, 53.1f, 61.7f, 67.9f, 73.3f };
        static const float apMs[kAps]     = { 4.77f, 3.59f, 12.73f, 9.31f };
        for (int i = 0; i < kLines; ++i)
        {
            const int len = juce::jmax (8, (int) (lineMs[i] * 0.001 * sr));
            lines[i].assign ((size_t) len, 0.0f);
            gains[i] = (float) std::pow (10.0, -3.0 * len / (juce::jmax (0.05f, rt60Sec) * sr));
        }
        for (int i = 0; i < kAps; ++i)
            aps[i].assign ((size_t) juce::jmax (4, (int) (apMs[i] * 0.001 * sr)), 0.0f);

        // Tone: dark (1.5 kHz damping) .. bright (14 kHz)
        const double dampHz = 1500.0 * std::pow (14000.0 / 1500.0, (double) tone);
        dampCoef = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * dampHz / sr));
    }

    void process (float in, float& outL, float& outR)
    {
        // input diffusion
        for (int i = 0; i < kAps; ++i)
        {
            auto& b = aps[i];
            const float delayed = b[(size_t) apIdx[i]];
            const float y = -0.6f * in + delayed;
            b[(size_t) apIdx[i]] = in + 0.6f * y;
            apIdx[i] = (apIdx[i] + 1) % (int) b.size();
            in = y;
        }

        float y[kLines], sum = 0.0f;
        for (int i = 0; i < kLines; ++i)
        {
            const float d = lines[i][(size_t) idx[i]];
            damp[i] += dampCoef * (d - damp[i]);
            y[i] = damp[i];
            sum += y[i];
        }
        sum *= 2.0f / (float) kLines; // Householder feedback matrix
        static const float inSign[kLines] = { 1, -1, 1, -1, 1, 1, -1, -1 };
        for (int i = 0; i < kLines; ++i)
        {
            lines[i][(size_t) idx[i]] = (y[i] - sum) * gains[i] + in * inSign[i];
            idx[i] = (idx[i] + 1) % (int) lines[i].size();
        }
        outL = 0.25f * (y[0] - y[1] + y[2] - y[3] + y[4] - y[5] + y[6] - y[7]);
        outR = 0.25f * (y[0] + y[1] - y[2] - y[3] + y[4] + y[5] - y[6] - y[7]);
    }

private:
    static constexpr int kLines = 8, kAps = 4;
    std::vector<float> lines[kLines], aps[kAps];
    float gains[kLines] {}, damp[kLines] {};
    int idx[kLines] {}, apIdx[kAps] {};
    float dampCoef = 0.5f;
};

/** Resamples the slice to the output rate. */
static juce::AudioBuffer<float> resampleSlice (const juce::AudioBuffer<float>& slice, double sliceRate, double sr)
{
    const int n = slice.getNumSamples();
    const double ratio = sliceRate / sr;
    const int outN = juce::jmax (1, (int) std::floor ((n - 1) / ratio));
    juce::AudioBuffer<float> out (2, outN);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = slice.getReadPointer (juce::jmin (ch, slice.getNumChannels() - 1));
        float* d = out.getWritePointer (ch);
        for (int i = 0; i < outN; ++i)
            d[i] = hermite (src, n, i * ratio);
    }
    return out;
}

/** Reverse swell: reverse the syllable -> long reverb (100% wet) -> reverse again.
    The swell rises until `dropSample` (minus the optional gap) and is cut there. */
static juce::AudioBuffer<float> renderReverseSwell (const juce::AudioBuffer<float>& dry, const RiserSettings& s,
                                                    double sr, int dropSample)
{
    const int len = dry.getNumSamples();
    const int gap = juce::jmin (dropSample / 2, (int) (juce::jlimit (0.0f, 500.0f, s.revPreDelayMs) * 0.001 * sr));
    const int cut = dropSample - gap;   // where the swell stops
    // Size: the reverb's RT60 is a fraction of the riser length, so the swell fills the bars you choose
    const float rt60 = (float) (cut / sr) * (0.4f + 1.2f * juce::jlimit (0.0f, 1.0f, s.revSize));
    const int wetLen = len + (int) (rt60 * sr);

    // 1+2. reverse the syllable and feed it through the reverb (100% wet)
    juce::AudioBuffer<float> wet (2, wetLen);
    LongReverb rev (sr, rt60, s.revTone);
    const float* dl = dry.getReadPointer (0);
    const float* dr = dry.getReadPointer (1);
    float* wl = wet.getWritePointer (0);
    float* wr = wet.getWritePointer (1);
    for (int t = 0; t < wetLen; ++t)
    {
        const float in = t < len ? 0.5f * (dl[len - 1 - t] + dr[len - 1 - t]) : 0.0f;
        rev.process (in, wl[t], wr[t]);
    }

    // clean the low end of the wet signal
    {
        juce::dsp::StateVariableTPTFilter<float> hp;
        hp.prepare ({ sr, 512, 2 });
        hp.setType (juce::dsp::StateVariableTPTFilterType::highpass);
        hp.setCutoffFrequency (120.0f);
        for (int t = 0; t < wetLen; ++t)
        {
            wl[t] = hp.processSample (0, wl[t]);
            wr[t] = hp.processSample (1, wr[t]);
        }
    }

    // 3. reverse again. In the reversed timeline the original syllable would start at
    //    k0 = wetLen - len: that is where the swell peaks, so line it up with the cut point.
    const int k0 = wetLen - len;
    const int offset = cut - k0;                  // output index of reversed sample 0
    juce::AudioBuffer<float> out (2, dropSample);
    out.clear();
    for (int o = juce::jmax (0, offset); o < cut; ++o)
    {
        const int k = o - offset;
        out.setSample (0, o, wl[wetLen - 1 - k]);
        out.setSample (1, o, wr[wetLen - 1 - k]);
    }

    // smooth fade-in over the first 8th of the riser, quick fade at the cut
    const int fi = juce::jmax (1, cut / 8);
    for (int i = 0; i < fi; ++i)
    {
        const float g = std::sin ((float) i / (float) fi * juce::MathConstants<float>::halfPi);
        for (int ch = 0; ch < 2; ++ch) out.setSample (ch, i, out.getSample (ch, i) * g * g);
    }
    const int fo = juce::jmin (cut, (int) (0.004 * sr));
    for (int i = 0; i < fo; ++i)
        for (int ch = 0; ch < 2; ++ch) out.setSample (ch, cut - 1 - i, out.getSample (ch, cut - 1 - i) * (float) i / (float) fo);
    return out;
}

static void normaliseTo (juce::AudioBuffer<float>& b, float target)
{
    const float pk = b.getMagnitude (0, b.getNumSamples());
    if (pk > 1.0e-6f) b.applyGain (target / pk);
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

    r->dropSec     = L;
    r->lengthSec   = L;
    r->beatsPerBar = barQ;
    r->audio.setSize (2, N);
    r->audio.clear();

    if (slice.getNumSamples() < 4 || slice.getNumChannels() == 0)
        return r;

    const bool useStutter = s.engine != engineReverse;
    const bool useReverse = s.engine != engineStutter;
    const float blend = juce::jlimit (0.0f, 1.0f, s.blend);
    const float wStutter = s.engine == engineBoth ? std::cos (blend * juce::MathConstants<float>::halfPi) : 1.0f;
    const float wReverse = s.engine == engineBoth ? std::sin (blend * juce::MathConstants<float>::halfPi) : 1.0f;

    const auto dry = resampleSlice (slice, sliceRate, sr);

    juce::AudioBuffer<float> stutter, swell;
    if (useStutter)
    {
        renderStutter (slice, sliceRate, s, sr, qSec, totalQ, N, stutter, r->grains);
        normaliseTo (stutter, 0.89f * wStutter);
    }
    if (useReverse)
    {
        swell = renderReverseSwell (dry, s, sr, N);
        normaliseTo (swell, 0.89f * wReverse);
    }

    const int landLen = s.land ? dry.getNumSamples() : 0;
    const int total = juce::jmax (N, swell.getNumSamples(), N + landLen);
    r->audio.setSize (2, total, false, true, false);
    r->audio.clear();

    if (useStutter) for (int ch = 0; ch < 2; ++ch) r->audio.addFrom (ch, 0, stutter, ch, 0, stutter.getNumSamples());
    if (useReverse) for (int ch = 0; ch < 2; ++ch) r->audio.addFrom (ch, 0, swell, ch, 0, swell.getNumSamples());

    if (landLen > 0)
    {
        const float pk = dry.getMagnitude (0, landLen);
        const float g = pk > 1.0e-6f ? 0.8f / pk : 0.0f;
        for (int ch = 0; ch < 2; ++ch) r->audio.addFrom (ch, N, dry, ch, 0, landLen, g);
        r->landSec = landLen / sr;
    }

    // normalise the whole thing to -1 dBFS
    const float peak = r->audio.getMagnitude (0, total);
    const float norm = peak > 1.0e-6f ? 0.89f / peak : 1.0f;
    r->audio.applyGain (norm);
    r->lengthSec = total / sr;

    // swell envelope for the GUI (relative to full scale)
    if (useReverse)
    {
        const int bins = 600;
        r->swellEnvelope.assign ((size_t) bins, 0.0f);
        const int n = swell.getNumSamples();
        for (int b = 0; b < bins; ++b)
        {
            const int a = (int) ((juce::int64) b * total / bins), e = (int) ((juce::int64) (b + 1) * total / bins);
            float m = 0.0f;
            for (int i = a; i < juce::jmin (e, n); ++i)
                m = juce::jmax (m, std::abs (swell.getSample (0, i)), std::abs (swell.getSample (1, i)));
            r->swellEnvelope[(size_t) b] = juce::jmin (1.0f, m * norm / 0.89f);
        }
    }

    // stutter hit heights follow the blend in Both mode
    if (useStutter && s.engine == engineBoth)
        for (auto& g : r->grains) g.gain *= juce::jmax (0.15f, wStutter);

    return r;
}
} // namespace sunriser
