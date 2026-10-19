// Offline test: synthesises a 3-syllable "vocal", loads it, checks syllable detection,
// renders + plays the riser through processBlock, round-trips the state and snapshots the GUI.
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <iostream>

using namespace juce;

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { std::cout << "FAIL: " << msg << "\n"; ++failures; } else std::cout << "ok:   " << msg << "\n"; } while (0)

static void addSyllable (AudioBuffer<float>& b, double sr, double t0, double t1, double f0)
{
    for (int i = (int) (t0 * sr); i < (int) (t1 * sr); ++i)
    {
        const double t = i / sr - t0, dur = t1 - t0;
        const double env = jmin (1.0, t / 0.012) * jmin (1.0, (dur - t) / 0.05) * (1.0 - 0.4 * t / dur);
        double v = 0;
        for (int h = 1; h <= 12; ++h)
        {
            const double f = f0 * h * (1.0 + 0.03 * t);
            const double formant = std::exp (-std::pow ((f - 700.0) / 300.0, 2.0)) + 0.6 * std::exp (-std::pow ((f - 1200.0) / 400.0, 2.0)) + 0.15;
            v += formant / h * std::sin (MathConstants<double>::twoPi * f * t);
        }
        for (int ch = 0; ch < 2; ++ch) b.addSample (ch, i, (float) (0.35 * env * v));
    }
}

static bool waitForRender (SunRiserProcessor& p, int previousVersion)
{
    for (int i = 0; i < 100; ++i)
    {
        MessageManager::getInstance()->runDispatchLoopUntil (40);
        if (p.getRenderVersion() > previousVersion && p.getLatestRender() != nullptr) return true;
    }
    return false;
}

static void snapshot (Component& c, const File& f)
{
    auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    f.deleteFile();
    FileOutputStream out (f);
    PNGImageFormat().writeImageToStream (img, out);
    std::cout << "      wrote " << f.getFullPathName() << "\n";
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    const File outDir (argc > 1 ? String (argv[1]) : File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();

    // 1. Synthetic vocal: "ta - da - la"
    const double srcRate = 44100.0;
    AudioBuffer<float> vocal (2, (int) (srcRate * 1.4));
    vocal.clear();
    Random rng (1);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < vocal.getNumSamples(); ++i) vocal.setSample (ch, i, (rng.nextFloat() - 0.5f) * 0.0005f);
    addSyllable (vocal, srcRate, 0.10, 0.34, 196.0);
    addSyllable (vocal, srcRate, 0.45, 0.80, 220.0);
    addSyllable (vocal, srcRate, 0.92, 1.25, 247.0);

    const auto vocalFile = outDir.getChildFile ("test_vocal.wav");
    {
        vocalFile.deleteFile();
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (vocalFile), srcRate, 2, 24, {}, 0));
        w->writeFromAudioSampleBuffer (vocal, 0, vocal.getNumSamples());
    }

    // 2. Processor
    std::unique_ptr<SunRiserProcessor> proc (new SunRiserProcessor());
    proc->setPlayConfigDetails (0, 2, 48000.0, 512);
    proc->prepareToPlay (48000.0, 512);

    CHECK (proc->loadAudioFile (vocalFile), "load WAV file");
    const auto sl = proc->getSlice();
    const double s0 = sl.getStart() / srcRate, s1 = sl.getEnd() / srcRate;
    std::cout << "      detected syllable: " << s0 * 1000 << " ms -> " << s1 * 1000 << " ms\n";
    CHECK (s0 > 0.07 && s0 < 0.12, "syllable start near 100 ms");
    CHECK (s1 > 0.30 && s1 < 0.42, "syllable end near 340 ms (before the 2nd syllable)");

    CHECK (waitForRender (*proc, 0), "riser rendered in background");
    auto r = proc->getLatestRender();
    const double expectedLen = 2 * 4 * 60.0 / 128.0;
    std::cout << "      length " << r->lengthSec << " s, hits " << r->grains.size() << "\n";
    CHECK (std::abs (r->lengthSec - expectedLen) < 1e-6, "2 bars @128 BPM = 3.75 s");
    CHECK (r->grains.size() > 20, "repeats accelerate (many hits)");
    CHECK (r->grains.back().semis > 9.0f, "pitch rises towards +12 st");
    CHECK (r->audio.getMagnitude (0, r->audio.getNumSamples()) > 0.8f, "rendered audio normalised");

    // 3. Play through processBlock with a MIDI note
    AudioBuffer<float> out (2, (int) (expectedLen * 48000) + 4800);
    out.clear();
    int written = 0;
    bool first = true;
    while (written < out.getNumSamples())
    {
        const int n = jmin (512, out.getNumSamples() - written);
        AudioBuffer<float> block (2, n);
        MidiBuffer midi;
        if (first) { midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 10); first = false; }
        proc->processBlock (block, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, written, block, ch, 0, n);
        written += n;
    }
    const int endSample = 10 + r->audio.getNumSamples();
    CHECK (out.getMagnitude (0, endSample) > 0.5f, "MIDI note plays the riser");
    CHECK (out.getMagnitude (endSample + 10, out.getNumSamples() - endSample - 10) < 1e-6f, "riser stops exactly at its end (on the drop)");
    CHECK (out.getMagnitude (0, 0, 10) == 0.0f, "nothing before the note");
    {
        const auto f = outDir.getChildFile ("riser_smooth.wav");
        f.deleteFile();
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (f), 48000.0, 2, 24, {}, 0));
        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    }

    // 4. State round-trip
    MemoryBlock state;
    proc->getStateInformation (state);
    std::cout << "      state size " << state.getSize() / 1024 << " KB\n";
    {
        SunRiserProcessor p2;
        p2.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (p2.getSource() != nullptr, "sample restored from project state");
        CHECK (p2.getSlice() == proc->getSlice(), "syllable selection restored");
    }

    // 5. Export
    const auto exported = proc->exportRiserToTempFile();
    CHECK (exported.existsAsFile() && exported.getSize() > 100000, "drag-out WAV exported: " + exported.getFileName().toStdString());

    // 6. GUI snapshots
    {
        std::unique_ptr<AudioProcessorEditor> ed (proc->createEditor());
        auto* sunEd = dynamic_cast<SunRiserEditor*> (ed.get());
        ed->setSize (SunRiserEditor::baseW, SunRiserEditor::baseH);
        MessageManager::getInstance()->runDispatchLoopUntil (100);
        snapshot (*ed, outDir.getChildFile ("gui_sunrise.png"));

        sunEd->applyTheme (2);
        snapshot (*ed, outDir.getChildFile ("gui_dusk.png"));
        sunEd->applyTheme (4);
        snapshot (*ed, outDir.getChildFile ("gui_daylight.png"));
        sunEd->applyTheme (3);
        snapshot (*ed, outDir.getChildFile ("gui_ocean.png"));
        sunEd->applyTheme (1);

        // Steps mode, 4 bars
        const int v = proc->getRenderVersion();
        proc->apvts.getParameter (IDs::mode)->setValueNotifyingHost (1.0f);
        proc->apvts.getParameter (IDs::length)->setValueNotifyingHost (proc->apvts.getParameter (IDs::length)->convertTo0to1 (2.0f));
        CHECK (waitForRender (*proc, v), "re-render after parameter change");
        auto r2 = proc->getLatestRender();
        CHECK (r2->settings.stepMode && r2->settings.bars == 4, "steps mode, 4 bars");
        bool onGrid = true;
        const double q = 60.0 / 128.0;
        for (auto& g : r2->grains)
        {
            const double inQ = g.startSec / q / 0.125; // every hit on a 1/32 grid
            if (std::abs (inQ - std::round (inQ)) > 1e-6) onGrid = false;
        }
        CHECK (onGrid, "steps mode hits are on the grid");
        MessageManager::getInstance()->runDispatchLoopUntil (100);
        snapshot (*ed, outDir.getChildFile ("gui_golden_steps.png"));

        // empty state
        ed.reset();
        SunRiserProcessor empty;
        std::unique_ptr<AudioProcessorEditor> ed2 (empty.createEditor());
        MessageManager::getInstance()->runDispatchLoopUntil (100);
        snapshot (*ed2, outDir.getChildFile ("gui_empty.png"));
    }

    std::cout << (failures == 0 ? "\nALL TESTS PASSED\n" : "\nSOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
