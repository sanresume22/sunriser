#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "Theme.h"

class MainPanel;

class SunRiserEditor : public juce::AudioProcessorEditor
{
public:
    static constexpr int baseW = 1040, baseH = 670;

    explicit SunRiserEditor (SunRiserProcessor&);
    ~SunRiserEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void applyTheme (int index);

private:
    SunRiserProcessor& proc;
    SunLookAndFeel lnf;
    std::unique_ptr<MainPanel> panel;
    std::unique_ptr<juce::TooltipWindow> tooltips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SunRiserEditor)
};
