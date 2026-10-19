#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace sunriser;

static constexpr double kMaxSourceSeconds = 20.0;

//==============================================================================
class RenderThread : public juce::Thread
{
public:
    explicit RenderThread (SunRiserProcessor& p) : juce::Thread ("SunRiser render"), owner (p) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (200);
            if (threadShouldExit()) break;

            std::optional<SunRiserProcessor::RenderJob> job;
            {
                const juce::ScopedLock sl (owner.jobLock);
                job.swap (owner.pendingJob);
            }
            if (job.has_value())
                owner.publishRender (renderRiser (job->slice, job->sliceRate, job->settings));
        }
    }

private:
    SunRiserProcessor& owner;
};

//==============================================================================
SunRiserProcessor::SunRiserProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SUNRISER", createLayout())
{
    formatManager.registerBasicFormats();

    pLength = apvts.getRawParameterValue (IDs::length);  pMode   = apvts.getRawParameterValue (IDs::mode);
    pStart  = apvts.getRawParameterValue (IDs::startRate); pEnd  = apvts.getRawParameterValue (IDs::endRate);
    pCurve  = apvts.getRawParameterValue (IDs::curve);   pChop   = apvts.getRawParameterValue (IDs::chop);
    pPitch  = apvts.getRawParameterValue (IDs::pitch);   pSweep  = apvts.getRawParameterValue (IDs::sweep);
    pSpace  = apvts.getRawParameterValue (IDs::space);   pSwell  = apvts.getRawParameterValue (IDs::swell);
    pWidth  = apvts.getRawParameterValue (IDs::width);   pOutput = apvts.getRawParameterValue (IDs::output);
    pEngine = apvts.getRawParameterValue (IDs::engine);  pLand   = apvts.getRawParameterValue (IDs::land);
    pRevSize = apvts.getRawParameterValue (IDs::revSize); pRevTone = apvts.getRawParameterValue (IDs::revTone);
    pRevPre = apvts.getRawParameterValue (IDs::revPre);  pBlend  = apvts.getRawParameterValue (IDs::blend);

    outGain.reset (48000.0, 0.02);
    outGain.setCurrentAndTargetValue (1.0f);

    renderThread = std::make_unique<RenderThread> (*this);
    renderThread->startThread (juce::Thread::Priority::low);
    startTimerHz (25);
}

SunRiserProcessor::~SunRiserProcessor()
{
    stopTimer();
    renderThread->signalThreadShouldExit();
    renderThread->notify();
    renderThread->stopThread (5000);
}

juce::AudioProcessorValueTreeState::ParameterLayout SunRiserProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pct = [] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; };
    auto pctAttr = [pct] { return AudioParameterFloatAttributes().withStringFromValueFunction (pct); };

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { IDs::length, 1 },    "Length", StringArray { "1 Bar", "2 Bars", "4 Bars", "8 Bars" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { IDs::mode, 1 },      "Feel",   StringArray { "Smooth", "Steps" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { IDs::startRate, 1 }, "From",   StringArray { "1/2", "1/4", "1/8" }, 2));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { IDs::endRate, 1 },   "To",     StringArray { "1/16", "1/32", "1/64" }, 1));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::curve, 1 }, "Curve", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f, pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::chop, 1 },  "Chop",  NormalisableRange<float> (0.1f, 1.0f, 0.001f), 0.85f, pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::pitch, 1 }, "Pitch", NormalisableRange<float> (0.0f, 24.0f, 1.0f), 12.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return "+" + String (roundToInt (v)) + " st"; })));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::sweep, 1 }, "Sweep", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.65f, pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::space, 1 }, "Space", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f, pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::swell, 1 }, "Swell", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.7f,  pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::width, 1 }, "Width", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f,  pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::output, 1 }, "Output", NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; })));

    // Reverse swell
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { IDs::engine, 1 }, "Mode", StringArray { "Stutter", "Reverse", "Both" }, 0));
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { IDs::land, 1 },   "Land on Drop", false));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { IDs::revSize, 1 }, "Reverse Size", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.8f, pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { IDs::revTone, 1 }, "Reverse Tone", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.55f, pctAttr()));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { IDs::revPre, 1 },  "Gap", NormalisableRange<float> (0.0f, 250.0f, 1.0f), 0.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " ms"; })));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { IDs::blend, 1 },   "Blend", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                {
                    const int rv = roundToInt (v * 100.0f);
                    return rv <= 2 ? String ("Stutter") : rv >= 98 ? String ("Reverse") : String (100 - rv) + " / " + String (rv);
                })));
    return layout;
}

//==============================================================================
void SunRiserProcessor::prepareToPlay (double sampleRate, int)
{
    outGain.reset (sampleRate, 0.02);
    fadeLen = juce::jmax (16, (int) (sampleRate * 0.005));
    needsRender = true;
}

bool SunRiserProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void SunRiserProcessor::startVoice (bool fromMidi)
{
    playPos = 0;
    playing = active != nullptr;
    midiTriggered = fromMidi;
    fadeRemaining = -1;
}

void SunRiserProcessor::beginFadeOut()
{
    if (playing && fadeRemaining < 0)
        fadeRemaining = fadeLen;
}

void SunRiserProcessor::renderVoice (juce::AudioBuffer<float>& buf, int from, int to)
{
    if (! playing || active == nullptr) { playing = false; return; }

    const auto len = (juce::int64) active->audio.getNumSamples();
    const float* L = active->audio.getReadPointer (0);
    const float* R = active->audio.getReadPointer (1);
    const int outCh = buf.getNumChannels();

    for (int i = from; i < to; ++i)
    {
        if (playPos >= len) { playing = false; break; }

        float g = outGain.getNextValue();
        if (fadeRemaining >= 0)
        {
            g *= (float) fadeRemaining / (float) fadeLen;
            if (--fadeRemaining < 0) { playing = false; break; }
        }

        const float l = L[playPos] * g, r = R[playPos] * g;
        if (outCh >= 2)      { buf.addSample (0, i, l); buf.addSample (1, i, r); }
        else if (outCh == 1)   buf.addSample (0, i, 0.5f * (l + r));
        ++playPos;
    }
}

void SunRiserProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const int numSamples = buffer.getNumSamples();

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())          hostBpm = *bpm;
            if (auto ts = pos->getTimeSignature()) { hostNum = ts->numerator; hostDen = ts->denominator; }

            const bool hostPlaying = pos->getIsPlaying();
            if (wasHostPlaying && ! hostPlaying && midiTriggered)
                beginFadeOut();
            wasHostPlaying = hostPlaying;
        }
    }

    // Pick up a freshly rendered riser (keeps the playback position if one is running)
    {
        const juce::SpinLock::ScopedTryLockType sl (swapLock);
        if (sl.isLocked() && incoming != nullptr && retired == nullptr)
        {
            if (active != nullptr && playing)
                playPos = (juce::int64) ((double) playPos / juce::jmax (1, active->audio.getNumSamples())
                                         * incoming->audio.getNumSamples());
            retired = std::move (active);   // swaps: retired <- active, active <- null
            active  = std::move (incoming); // swaps: active <- incoming, incoming <- null
        }
    }

    if (previewRequested.exchange (false)) startVoice (false);
    if (stopRequested.exchange (false))    beginFadeOut();

    outGain.setTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));

    int pos = 0;
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        const int t = juce::jlimit (0, numSamples, meta.samplePosition);
        renderVoice (buffer, pos, t);
        pos = t;

        if (msg.isNoteOn())                                   startVoice (true);
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())  beginFadeOut();
    }
    renderVoice (buffer, pos, numSamples);

    playProgress = (playing && active != nullptr)
                     ? (float) ((double) playPos / juce::jmax (1, active->audio.getNumSamples()))
                     : -1.0f;
}

//==============================================================================
RiserSettings SunRiserProcessor::currentSettings() const
{
    static const int    bars[]   = { 1, 2, 4, 8 };
    static const double starts[] = { 2.0, 1.0, 0.5 };
    static const double ends[]   = { 0.25, 0.125, 0.0625 };

    RiserSettings s;
    s.bars       = bars[juce::jlimit (0, 3, (int) pLength->load())];
    s.stepMode   = pMode->load() > 0.5f;
    s.startDivQ  = starts[juce::jlimit (0, 2, (int) pStart->load())];
    s.endDivQ    = ends[juce::jlimit (0, 2, (int) pEnd->load())];
    s.curve = pCurve->load(); s.chop  = pChop->load();  s.pitch = pPitch->load();
    s.sweep = pSweep->load(); s.space = pSpace->load(); s.swell = pSwell->load(); s.width = pWidth->load();
    s.bpm        = hostBpm.load();
    s.beatsNum   = juce::jlimit (1, 32, hostNum.load());
    s.beatsDen   = juce::jlimit (1, 32, hostDen.load());
    s.sampleRate = getSampleRate() > 0 ? getSampleRate() : 48000.0;
    s.engine        = juce::jlimit (0, 2, (int) pEngine->load());
    s.land          = pLand->load() > 0.5f;
    s.revSize       = pRevSize->load();
    s.revTone       = pRevTone->load();
    s.revPreDelayMs = pRevPre->load();
    s.blend         = pBlend->load();
    return s;
}

void SunRiserProcessor::timerCallback()
{
    // Free buffers the audio thread has finished with (never deallocate on the audio thread)
    {
        RenderedRiser::Ptr toFree;
        {
            const juce::SpinLock::ScopedLockType sl (swapLock);
            toFree = std::move (retired);
        }
    }

    const auto settings = currentSettings();
    SourceSample::Ptr src;
    juce::Range<int> sl;
    {
        const juce::ScopedLock lock (sourceLock);
        src = source;
        sl = slice;
    }
    if (src == nullptr || sl.getLength() < 4)
        return;

    if (! needsRender.exchange (false) && hasRequested && settings == lastRequested)
        return;

    lastRequested = settings;
    hasRequested = true;

    RenderJob job;
    job.settings = settings;
    job.sliceRate = src->sampleRate;
    job.slice.setSize (2, sl.getLength());
    for (int ch = 0; ch < 2; ++ch)
        job.slice.copyFrom (ch, 0, src->audio, juce::jmin (ch, src->audio.getNumChannels() - 1), sl.getStart(), sl.getLength());

    {
        const juce::ScopedLock lock (jobLock);
        pendingJob = std::move (job);
    }
    renderThread->notify();
}

void SunRiserProcessor::publishRender (RenderedRiser::Ptr r)
{
    {
        RenderedRiser::Ptr old;
        {
            const juce::SpinLock::ScopedLockType sl (swapLock);
            old = incoming;
            incoming = r;
        }
    }
    {
        const juce::ScopedLock sl (guiLock);
        guiRender = r;
    }
    ++renderVersion;
}

RenderedRiser::Ptr SunRiserProcessor::getLatestRender() const
{
    const juce::ScopedLock sl (guiLock);
    return guiRender;
}

//==============================================================================
static juce::MemoryBlock encodeAudio (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    juce::MemoryBlock block;
    juce::FlacAudioFormat flac;
    juce::WavAudioFormat wav;

    for (juce::AudioFormat* fmt : { static_cast<juce::AudioFormat*> (&flac), static_cast<juce::AudioFormat*> (&wav) })
    {
        block.reset();
        auto* stream = new juce::MemoryOutputStream (block, false);
        std::unique_ptr<juce::AudioFormatWriter> writer (fmt->createWriterFor (stream, sampleRate, (unsigned int) audio.getNumChannels(), 24, {}, 0));
        if (writer == nullptr) { delete stream; continue; }
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        writer.reset(); // flushes into block
        if (block.getSize() > 0) return block;
    }
    return {};
}

bool SunRiserProcessor::setSourceFromReader (juce::AudioFormatReader& reader, const juce::String& name, const juce::MemoryBlock* encoded)
{
    if (reader.sampleRate <= 0 || reader.lengthInSamples <= 0)
    {
        lastError = "That file has no audio in it.";
        return false;
    }

    const int n = (int) juce::jmin (reader.lengthInSamples, (juce::int64) (reader.sampleRate * kMaxSourceSeconds));
    SourceSample::Ptr s = new SourceSample();
    s->sampleRate = reader.sampleRate;
    s->name = name;
    s->audio.setSize (2, n);
    reader.read (&s->audio, 0, n, 0, true, true);
    if (reader.numChannels == 1)
        s->audio.copyFrom (1, 0, s->audio, 0, 0, n);
    s->encoded = encoded != nullptr ? *encoded : encodeAudio (s->audio, s->sampleRate);

    const auto syl = detectFirstSyllable (s->audio, s->sampleRate);
    {
        const juce::ScopedLock sl (sourceLock);
        source = s;
        slice = { syl.start, juce::jmax (syl.start + 4, syl.end) };
    }
    ++sourceVersion;
    ++sliceVersion;
    needsRender = true;
    lastError = {};
    return true;
}

bool SunRiserProcessor::loadAudioFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        lastError = "Couldn't read " + file.getFileName() + " (try WAV, AIFF, FLAC, MP3 or OGG).";
        return false;
    }
    return setSourceFromReader (*reader, file.getFileNameWithoutExtension(), nullptr);
}

SourceSample::Ptr SunRiserProcessor::getSource() const
{
    const juce::ScopedLock sl (sourceLock);
    return source;
}

juce::Range<int> SunRiserProcessor::getSlice() const
{
    const juce::ScopedLock sl (sourceLock);
    return slice;
}

void SunRiserProcessor::setSlice (juce::Range<int> r)
{
    {
        const juce::ScopedLock sl (sourceLock);
        if (source == nullptr) return;
        const int n = source->audio.getNumSamples();
        const int minLen = (int) (source->sampleRate * 0.02);
        const int start = juce::jlimit (0, juce::jmax (0, n - minLen), r.getStart());
        slice = { start, juce::jlimit (juce::jmin (n, start + minLen), n, r.getEnd()) };
    }
    ++sliceVersion;
    needsRender = true;
}

void SunRiserProcessor::autoDetectSlice()
{
    auto s = getSource();
    if (s == nullptr) return;
    const auto syl = detectFirstSyllable (s->audio, s->sampleRate);
    setSlice ({ syl.start, syl.end });
}

//==============================================================================
juce::File SunRiserProcessor::exportRiser (const juce::File& target)
{
    auto r = getLatestRender();
    if (r == nullptr) return {};

    juce::AudioBuffer<float> out;
    out.makeCopyOf (r->audio);
    out.applyGain (juce::Decibels::decibelsToGain (pOutput->load()));

    target.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (target.createOutputStream());
    if (stream == nullptr) return {};

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), r->settings.sampleRate, 2, 24, {}, 0));
    if (writer == nullptr) return {};
    stream.release(); // now owned by the writer
    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    return target;
}

juce::File SunRiserProcessor::exportRiserToTempFile()
{
    auto r = getLatestRender();
    auto src = getSource();
    if (r == nullptr || src == nullptr) return {};

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SunRiser");
    dir.createDirectory();
    const double bpm = r->settings.bpm;
    const auto bpmText = std::abs (bpm - std::round (bpm)) < 0.01 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 1);
    static const char* modeNames[] = { "", "reverse ", "stutter+reverse " };
    const auto name = juce::File::createLegalFileName ("SunRiser - " + src->name + " - " + modeNames[juce::jlimit (0, 2, r->settings.engine)]
                                                      + juce::String (r->settings.bars)
                                                      + (r->settings.bars == 1 ? " bar " : " bars ") + bpmText + "bpm.wav");
    return exportRiser (dir.getChildFile (name));
}

int SunRiserProcessor::getThemeIndex() const     { return (int) apvts.state.getProperty ("theme", 0); }
void SunRiserProcessor::setThemeIndex (int i)    { apvts.state.setProperty ("theme", i, nullptr); }

//==============================================================================
void SunRiserProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock sl (sourceLock);
        if (source != nullptr && source->encoded.getSize() > 0)
        {
            juce::ValueTree s ("SAMPLE");
            s.setProperty ("name", source->name, nullptr);
            s.setProperty ("start", slice.getStart(), nullptr);
            s.setProperty ("end", slice.getEnd(), nullptr);
            s.setProperty ("data", juce::var (source->encoded), nullptr);
            state.appendChild (s, nullptr);
        }
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void SunRiserProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    auto sample = tree.getChildWithName ("SAMPLE");
    if (sample.isValid())
        tree.removeChild (sample, nullptr);
    apvts.replaceState (tree);

    if (sample.isValid())
        if (auto* mb = sample.getProperty ("data").getBinaryData())
        {
            const juce::MemoryBlock block (*mb);
            std::unique_ptr<juce::AudioFormatReader> reader (
                formatManager.createReaderFor (std::make_unique<juce::MemoryInputStream> (block, true)));
            if (reader != nullptr && setSourceFromReader (*reader, sample.getProperty ("name").toString(), &block))
                setSlice ({ (int) sample.getProperty ("start"), (int) sample.getProperty ("end") });
        }
    needsRender = true;
}

//==============================================================================
juce::AudioProcessorEditor* SunRiserProcessor::createEditor()
{
    return new SunRiserEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SunRiserProcessor();
}
