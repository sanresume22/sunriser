#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <optional>
#include "RiserEngine.h"

namespace IDs
{
    inline constexpr const char* length    = "length";
    inline constexpr const char* mode      = "mode";
    inline constexpr const char* startRate = "startRate";
    inline constexpr const char* endRate   = "endRate";
    inline constexpr const char* curve     = "curve";
    inline constexpr const char* chop      = "chop";
    inline constexpr const char* pitch     = "pitch";
    inline constexpr const char* sweep     = "sweep";
    inline constexpr const char* space     = "space";
    inline constexpr const char* swell     = "swell";
    inline constexpr const char* width     = "width";
    inline constexpr const char* output    = "output";
    inline constexpr const char* engine    = "engine";
    inline constexpr const char* land      = "land";
    inline constexpr const char* revSize   = "revSize";
    inline constexpr const char* revTone   = "revTone";
    inline constexpr const char* revPre    = "revPreDelay";
    inline constexpr const char* blend     = "blend";
}

/** The loaded sound (max 20 s, always stereo). Immutable once created. */
struct SourceSample : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<SourceSample>;
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    juce::String name;
    juce::MemoryBlock encoded;   // FLAC/WAV copy saved with the DAW project
};

class SunRiserProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    SunRiserProcessor();
    ~SunRiserProcessor() override;

    //== AudioProcessor ========================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }
    const juce::String getName() const override           { return "SunRiser"; }
    bool acceptsMidi() const override                     { return true; }
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 0.0; }
    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //== Message-thread API used by the GUI ===================================
    bool loadAudioFile (const juce::File&);
    juce::String getLastError() const                     { return lastError; }
    SourceSample::Ptr getSource() const;
    juce::Range<int> getSlice() const;
    void setSlice (juce::Range<int>);
    void autoDetectSlice();

    sunriser::RenderedRiser::Ptr getLatestRender() const;
    int getRenderVersion() const                          { return renderVersion.load(); }
    int getSourceVersion() const                          { return sourceVersion.load(); }
    int getSliceVersion() const                           { return sliceVersion.load(); }

    void startPreview()                                   { previewRequested = true; }
    void stopPlayback()                                   { stopRequested = true; }
    float getPlayProgress() const                         { return playProgress.load(); }
    bool isVoicePlaying() const                           { return playProgress.load() >= 0.0f; }

    juce::File exportRiser (const juce::File& target);
    juce::File exportRiserToTempFile();

    int  getThemeIndex() const;
    void setThemeIndex (int);

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioFormatManager formatManager;

private:
    friend class RenderThread;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    sunriser::RiserSettings currentSettings() const;
    bool setSourceFromReader (juce::AudioFormatReader&, const juce::String& name, const juce::MemoryBlock* encoded);
    void publishRender (sunriser::RenderedRiser::Ptr);
    void startVoice (bool fromMidi);
    void beginFadeOut();
    void renderVoice (juce::AudioBuffer<float>&, int from, int to);

    //== source + slice (message thread, guarded) ==
    mutable juce::CriticalSection sourceLock;
    SourceSample::Ptr source;
    juce::Range<int> slice;
    juce::String lastError;
    std::atomic<int> sourceVersion { 0 }, sliceVersion { 0 }, renderVersion { 0 };
    std::atomic<bool> needsRender { false };

    //== background rendering ==
    struct RenderJob { juce::AudioBuffer<float> slice; double sliceRate = 44100.0; sunriser::RiserSettings settings; };
    juce::CriticalSection jobLock;
    std::optional<RenderJob> pendingJob;
    sunriser::RiserSettings lastRequested;
    bool hasRequested = false;
    std::unique_ptr<juce::Thread> renderThread;

    mutable juce::CriticalSection guiLock;
    sunriser::RenderedRiser::Ptr guiRender;

    //== audio thread hand-over (old buffers are freed on the message thread) ==
    juce::SpinLock swapLock;
    sunriser::RenderedRiser::Ptr incoming, active, retired;

    //== host info ==
    std::atomic<double> hostBpm { 128.0 };
    std::atomic<int> hostNum { 4 }, hostDen { 4 };
    bool wasHostPlaying = false;

    //== voice ==
    std::atomic<bool> previewRequested { false }, stopRequested { false };
    std::atomic<float> playProgress { -1.0f };
    bool playing = false, midiTriggered = false;
    juce::int64 playPos = 0;
    int fadeRemaining = -1, fadeLen = 256;
    juce::SmoothedValue<float> outGain;

    std::atomic<float>* pLength = nullptr; std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pStart = nullptr;  std::atomic<float>* pEnd = nullptr;
    std::atomic<float>* pCurve = nullptr;  std::atomic<float>* pChop = nullptr;
    std::atomic<float>* pPitch = nullptr;  std::atomic<float>* pSweep = nullptr;
    std::atomic<float>* pSpace = nullptr;  std::atomic<float>* pSwell = nullptr;
    std::atomic<float>* pWidth = nullptr;  std::atomic<float>* pOutput = nullptr;
    std::atomic<float>* pEngine = nullptr; std::atomic<float>* pLand = nullptr;
    std::atomic<float>* pRevSize = nullptr; std::atomic<float>* pRevTone = nullptr;
    std::atomic<float>* pRevPre = nullptr; std::atomic<float>* pBlend = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SunRiserProcessor)
};
