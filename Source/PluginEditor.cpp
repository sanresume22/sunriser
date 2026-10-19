#include "PluginEditor.h"

using namespace juce;
using namespace sunriser;

static const Theme& themeOf (const Component& c)
{
    if (auto* l = dynamic_cast<SunLookAndFeel*> (&c.getLookAndFeel()))
        return l->getTheme();
    return getThemes().front();
}

static Colour wellColour (const Theme& t)  { return t.light ? t.bgTop : t.panel.darker (0.3f); }

//==============================================================================
class Knob : public Component
{
public:
    Knob (AudioProcessorValueTreeState& state, const String& paramID, const String& title, const String& tip)
        : name (title.toUpperCase())
    {
        slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 20);
        slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        slider.setTooltip (tip + "\n(double-click to reset)");
        slider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        slider.setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
        addAndMakeVisible (slider);
        attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (state, paramID, slider);
        if (auto* p = state.getParameter (paramID))
            slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    }

    void paint (Graphics& g) override
    {
        g.setColour (themeOf (*this).textDim);
        g.setFont (sunFont (11.5f, true));
        g.drawText (name, getLocalBounds().removeFromTop (16), Justification::centred);
    }

    void resized() override { slider.setBounds (getLocalBounds().withTrimmedTop (16)); }
    void enablementChanged() override { setAlpha (isEnabled() ? 1.0f : 0.35f); }

private:
    String name;
    Slider slider;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
/** Pill-shaped segmented selector. Subclasses decide what selecting a segment does. */
class SegmentedBase : public Component, public SettableTooltipClient
{
public:
    explicit SegmentedBase (StringArray segmentLabels, float fontSize = 12.5f)
        : labels (std::move (segmentLabels)), textSize (fontSize) {}

    void setIndex (int i) { if (i != index) { index = i; repaint(); } }
    int getIndex() const  { return index; }

    void paint (Graphics& g) override
    {
        const auto& t = themeOf (*this);
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (wellColour (t));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (t.panelEdge);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);

        const float segW = r.getWidth() / (float) labels.size();
        for (int i = 0; i < labels.size(); ++i)
        {
            auto seg = Rectangle<float> (r.getX() + segW * (float) i, r.getY(), segW, r.getHeight()).reduced (3.0f);
            if (i == index)
            {
                g.setGradientFill (ColourGradient (t.accentA, seg.getTopLeft(), t.accentB, seg.getBottomRight(), false));
                g.fillRoundedRectangle (seg, 6.0f);
            }
            else if (i == hover)
            {
                g.setColour (t.accentA.withAlpha (0.14f));
                g.fillRoundedRectangle (seg, 6.0f);
            }
            g.setColour (i == index ? Colours::white : (i == hover ? t.text : t.textDim));
            g.setFont (sunFont (textSize, true));
            g.drawText (labels[i], seg, Justification::centred);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int i = segmentAt (e.x);
        if (i != index) select (i);
    }
    void mouseMove (const MouseEvent& e) override { setHover (segmentAt (e.x)); }
    void mouseExit (const MouseEvent&) override   { setHover (-1); }

protected:
    virtual void select (int i) = 0;

private:
    int segmentAt (int x) const { return jlimit (0, labels.size() - 1, x * labels.size() / jmax (1, getWidth())); }
    void setHover (int h) { if (h != hover) { hover = h; repaint(); } }

    StringArray labels;
    float textSize;
    int index = 0, hover = -1;
};

/** Segmented control bound to a choice parameter. */
class Segmented : public SegmentedBase
{
public:
    Segmented (RangedAudioParameter& p, StringArray segmentLabels, const String& tip, float fontSize = 12.5f)
        : SegmentedBase (std::move (segmentLabels), fontSize),
          attachment (p, [this] (float v) { setIndex (roundToInt (v)); if (onChange) onChange(); }, nullptr)
    {
        setTooltip (tip);
        attachment.sendInitialUpdate();
    }
    std::function<void()> onChange;

protected:
    void select (int i) override { attachment.setValueAsCompleteGesture ((float) i); }

private:
    ParameterAttachment attachment;
};

/** Segmented control for GUI-only state. */
class LocalSegmented : public SegmentedBase
{
public:
    LocalSegmented (StringArray segmentLabels, float fontSize = 12.5f) : SegmentedBase (std::move (segmentLabels), fontSize) {}
    std::function<void (int)> onSelect;

protected:
    void select (int i) override { setIndex (i); if (onSelect) onSelect (i); }
};

//==============================================================================
class WaveformView : public Component
{
public:
    explicit WaveformView (SunRiserProcessor& p) : proc (p) {}

    std::function<void()> onBrowse;
    bool dragHover = false;

    void refresh()
    {
        auto src = proc.getSource();
        if (proc.getSourceVersion() != shownSource)
        {
            shownSource = proc.getSourceVersion();
            viewEnd = 0;
            if (src != nullptr)
            {
                const int n = src->audio.getNumSamples();
                viewEnd = jlimit (jmin (n, 16), n, jmax (proc.getSlice().getEnd() * 3, (int) (src->sampleRate * 1.5)));
            }
            peaksWidth = -1;
        }
        repaint();
    }

    void paint (Graphics& g) override
    {
        const auto& t = themeOf (*this);
        auto r = getLocalBounds().toFloat();
        g.setColour (wellColour (t));
        g.fillRoundedRectangle (r, 10.0f);

        auto src = proc.getSource();
        if (src == nullptr || dragHover)
        {
            paintDropZone (g, r, t, src != nullptr);
            return;
        }

        buildPeaks (*src);
        const auto sl = proc.getSlice();
        const float x0 = sampleToX (sl.getStart()), x1 = sampleToX (sl.getEnd());
        const float mid = r.getCentreY(), half = r.getHeight() * 0.40f;

        Path wave;
        const int w = (int) peaks.size();
        if (w > 0)
        {
            wave.startNewSubPath (0.0f, mid - peaks[0].second * half);
            for (int i = 1; i < w; ++i) wave.lineTo ((float) i, mid - peaks[(size_t) i].second * half);
            for (int i = w - 1; i >= 0; --i) wave.lineTo ((float) i, mid - peaks[(size_t) i].first * half);
            wave.closeSubPath();
        }

        // Highlighted syllable region
        auto region = Rectangle<float> (x0, r.getY(), jmax (2.0f, x1 - x0), r.getHeight());
        g.setGradientFill (ColourGradient (t.accentA.withAlpha (0.20f), region.getTopLeft(), t.accentB.withAlpha (0.10f), region.getBottomRight(), false));
        g.fillRect (region);

        {
            Graphics::ScopedSaveState s (g);
            g.excludeClipRegion (region.toNearestInt());
            g.setColour (t.textDim.withAlpha (0.35f));
            g.fillPath (wave);
        }
        {
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (region.toNearestInt());
            g.setGradientFill (ColourGradient (t.accentA, region.getX(), mid, t.accentB, region.getRight(), mid, false));
            g.fillPath (wave);
        }
        g.setColour (t.textDim.withAlpha (0.25f));
        g.drawHorizontalLine ((int) mid, 0.0f, r.getWidth());

        // Handles
        for (auto [x, c] : { std::pair<float, Colour> { x0, t.accentA }, { x1, t.accentB } })
        {
            g.setColour (c);
            g.fillRect (x - 1.0f, r.getY(), 2.0f, r.getHeight());
            auto grip = Rectangle<float> (10.0f, 22.0f).withCentre ({ x, r.getY() + 16.0f });
            g.fillRoundedRectangle (grip, 4.0f);
            g.setColour (Colours::white.withAlpha (0.85f));
            g.fillRect (grip.getCentreX() - 2.0f, grip.getY() + 6.0f, 1.0f, 10.0f);
            g.fillRect (grip.getCentreX() + 1.0f, grip.getY() + 6.0f, 1.0f, 10.0f);
        }

        // Label pill above the syllable
        const auto ms = roundToInt (sl.getLength() * 1000.0 / src->sampleRate);
        const String label = "FIRST SYLLABLE  " + String (ms) + " ms";
        auto pill = Rectangle<float> (150.0f, 20.0f).withCentre ({ jlimit (80.0f, r.getRight() - 80.0f, (x0 + x1) * 0.5f), r.getBottom() - 16.0f });
        g.setColour (t.light ? Colours::white : t.panel);
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (t.accentA);
        g.drawRoundedRectangle (pill, 10.0f, 1.0f);
        g.setColour (t.text);
        g.setFont (sunFont (11.0f, true));
        g.drawText (label, pill, Justification::centred);

        // File name + zoom hint
        g.setColour (t.textDim);
        g.setFont (sunFont (11.5f, true));
        g.drawText (src->name, r.reduced (12.0f, 6.0f).removeFromTop (16.0f), Justification::topRight);
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto src = proc.getSource();
        if (src == nullptr) { if (onBrowse) onBrowse(); return; }

        const auto sl = proc.getSlice();
        const float x = (float) e.x, x0 = sampleToX (sl.getStart()), x1 = sampleToX (sl.getEnd());
        dragOffset = 0;
        if (std::abs (x - x0) < 9.0f)           mode = DragStart;
        else if (std::abs (x - x1) < 9.0f)      mode = DragEnd;
        else if (x > x0 && x < x1)            { mode = DragMove; dragOffset = xToSample (x) - sl.getStart(); }
        else                                  { mode = DragNew; anchor = xToSample (x); }
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (proc.getSource() == nullptr) return;
        const auto sl = proc.getSlice();
        const int s = xToSample ((float) e.x);
        switch (mode)
        {
            case DragStart: proc.setSlice ({ jmin (s, sl.getEnd() - 1), sl.getEnd() }); break;
            case DragEnd:   proc.setSlice ({ sl.getStart(), jmax (s, sl.getStart() + 1) }); break;
            case DragMove:  proc.setSlice (sl.movedToStartAt (jmax (0, s - dragOffset))); break;
            case DragNew:   proc.setSlice ({ jmin (anchor, s), jmax (anchor, s) }); break;
            case None:      break;
        }
        repaint();
    }

    void mouseUp (const MouseEvent&) override          { mode = None; }
    void mouseDoubleClick (const MouseEvent&) override { proc.autoDetectSlice(); }

    void mouseMove (const MouseEvent& e) override
    {
        if (proc.getSource() == nullptr) { setMouseCursor (MouseCursor::PointingHandCursor); return; }
        const auto sl = proc.getSlice();
        const float x = (float) e.x;
        const bool nearHandle = std::abs (x - sampleToX (sl.getStart())) < 9.0f || std::abs (x - sampleToX (sl.getEnd())) < 9.0f;
        setMouseCursor (nearHandle ? MouseCursor::LeftRightResizeCursor
                                   : (x > sampleToX (sl.getStart()) && x < sampleToX (sl.getEnd()) ? MouseCursor::DraggingHandCursor
                                                                                                  : MouseCursor::IBeamCursor));
    }

    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
    {
        auto src = proc.getSource();
        if (src == nullptr || exactlyEqual (w.deltaY, 0.0f)) return;
        const int n = src->audio.getNumSamples();
        viewEnd = jlimit (jmin (n, (int) (src->sampleRate * 0.1)), n, (int) (viewEnd * (w.deltaY > 0 ? 0.8 : 1.25)));
        peaksWidth = -1;
        repaint();
    }

private:
    void paintDropZone (Graphics& g, Rectangle<float> r, const Theme& t, bool replacing)
    {
        Path border;
        border.addRoundedRectangle (r.reduced (6.0f), 9.0f);
        Path dashed;
        const float dashes[] = { 7.0f, 5.0f };
        PathStrokeType (1.5f).createDashedStroke (dashed, border, dashes, 2);
        g.setColour (dragHover ? t.accentA : t.textDim.withAlpha (0.5f));
        g.fillPath (dashed);
        if (dragHover)
        {
            g.setColour (t.accentA.withAlpha (0.08f));
            g.fillRoundedRectangle (r.reduced (6.0f), 9.0f);
        }

        // little sun + arrow icon
        const auto c = Point<float> (r.getCentreX(), r.getCentreY() - 18.0f);
        g.setGradientFill (ColourGradient (t.accentC, c.x, c.y - 10.0f, t.accentA, c.x, c.y + 10.0f, false));
        g.fillEllipse (Rectangle<float> (20.0f, 20.0f).withCentre (c));
        Path arrow;
        arrow.addArrow ({ c.x, c.y + 6.0f, c.x, c.y - 6.0f }, 2.0f, 8.0f, 6.0f);
        g.setColour (Colours::white);
        g.fillPath (arrow);

        g.setColour (t.text);
        g.setFont (sunFont (17.0f, true));
        g.drawText (replacing ? "Drop to replace the sound" : "Drop a vocal or any sound here",
                    r.withTrimmedTop (r.getHeight() * 0.5f).removeFromTop (22.0f), Justification::centred);
        g.setColour (t.textDim);
        g.setFont (sunFont (12.5f));
        g.drawText ("or click to browse  -  SunRiser grabs the first syllable and builds a riser from it",
                    r.withTrimmedTop (r.getHeight() * 0.5f + 24.0f).removeFromTop (18.0f), Justification::centred);
    }

    void buildPeaks (const SourceSample& src)
    {
        const int w = getWidth();
        if (w == peaksWidth) return;
        peaksWidth = w;
        peaks.assign ((size_t) jmax (0, w), { 0.0f, 0.0f });
        const int n = jmin (viewEnd, src.audio.getNumSamples());
        const float* L = src.audio.getReadPointer (0);
        const float* R = src.audio.getReadPointer (1);
        float maxAbs = 1.0e-4f;
        for (int x = 0; x < w; ++x)
        {
            const int a = (int) ((int64) x * n / w), b = jmax (a + 1, (int) ((int64) (x + 1) * n / w));
            float lo = 0, hi = 0;
            for (int i = a; i < jmin (b, n); ++i)
            {
                const float v = 0.5f * (L[i] + R[i]);
                lo = jmin (lo, v); hi = jmax (hi, v);
            }
            peaks[(size_t) x] = { lo, hi };
            maxAbs = jmax (maxAbs, -lo, hi);
        }
        for (auto& p : peaks) { p.first /= maxAbs; p.second /= maxAbs; }
    }

    float sampleToX (int s) const { return viewEnd > 0 ? (float) s * (float) getWidth() / (float) viewEnd : 0.0f; }
    int xToSample (float x) const { return jmax (0, (int) ((double) x / jmax (1, getWidth()) * viewEnd)); }

    SunRiserProcessor& proc;
    std::vector<std::pair<float, float>> peaks;
    int peaksWidth = -1, shownSource = -1, viewEnd = 0;
    enum Mode { None, DragStart, DragEnd, DragMove, DragNew } mode = None;
    int dragOffset = 0, anchor = 0;
};

//==============================================================================
class RiserView : public Component, public SettableTooltipClient
{
public:
    explicit RiserView (SunRiserProcessor& p) : proc (p)
    {
        setTooltip ("Bars = stutter hits (height = volume). Glow = reverse swell. Bright line = pitch, dashed line = filter.");
    }

    void paint (Graphics& g) override
    {
        const auto& t = themeOf (*this);
        auto r = getLocalBounds().toFloat();
        g.setColour (wellColour (t));
        g.fillRoundedRectangle (r, 10.0f);

        auto render = proc.getLatestRender();
        if (render == nullptr || proc.getSource() == nullptr)
        {
            g.setColour (t.textDim);
            g.setFont (sunFont (14.0f, true));
            g.drawText ("Your riser will appear here", r, Justification::centred);
            return;
        }

        const auto& s = render->settings;
        const bool hasStutter = s.engine != engineReverse;
        const bool hasSwell   = ! render->swellEnvelope.empty();
        auto info = r.reduced (14.0f, 8.0f).removeFromTop (16.0f);
        auto plot = r.reduced (14.0f, 10.0f).withTrimmedTop (24.0f);
        const double totalQ = s.bars * render->beatsPerBar;
        const double qSec = 60.0 / s.bpm;
        const double total = jmax (0.001, render->lengthSec);
        auto xAt = [&] (double sec) { return plot.getX() + (float) (sec / total) * plot.getWidth(); };
        const float dropX = xAt (render->dropSec);

        // header line
        static const char* modeText[] = { "STUTTER", "REVERSE SWELL", "STUTTER + REVERSE" };
        g.setFont (sunFont (11.5f, true));
        g.setColour (t.textDim);
        const auto bpm = std::abs (s.bpm - std::round (s.bpm)) < 0.01 ? String (roundToInt (s.bpm)) : String (s.bpm, 1);
        String text = String (modeText[jlimit (0, 2, s.engine)]) + "   -   " + String (s.bars) + (s.bars == 1 ? " BAR" : " BARS")
                    + "   -   " + bpm + " BPM   -   " + String (s.beatsNum) + "/" + String (s.beatsDen);
        if (hasStutter) text << "   -   " << (int) render->grains.size() << " HITS";
        g.drawText (text, info, Justification::centredLeft);

        auto legend = info.removeFromRight (190.0f);
        if (hasStutter)
        {
            g.setColour (t.accentC);
            g.fillRoundedRectangle (legend.getX(), legend.getCentreY() - 1.5f, 16.0f, 3.0f, 1.5f);
            g.setColour (t.textDim);
            g.drawText ("PITCH", legend.withTrimmedLeft (22.0f), Justification::centredLeft);
            g.setColour (t.text.withAlpha (0.55f));
            for (int i = 0; i < 3; ++i) g.fillRect (legend.getX() + 86.0f + i * 6.0f, legend.getCentreY() - 0.75f, 3.5f, 1.5f);
            g.setColour (t.textDim);
            g.drawText ("FILTER", legend.withTrimmedLeft (110.0f), Justification::centredLeft);
        }

        // beat & bar grid (up to the drop)
        for (int q = 0; q <= (int) std::floor (totalQ + 1.0e-6); ++q)
        {
            const float x = xAt (q * qSec);
            const double barPos = q / render->beatsPerBar;
            const bool isBar = std::abs (barPos - std::round (barPos)) < 1.0e-6;
            g.setColour (t.textDim.withAlpha (isBar ? 0.35f : 0.10f));
            g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
            if (isBar && q < (int) std::round (totalQ))
            {
                g.setColour (t.textDim);
                g.setFont (sunFont (10.5f, true));
                g.drawText (String ((int) std::round (barPos) + 1), Rectangle<float> (x + 4.0f, plot.getY() + 2.0f, 20.0f, 12.0f), Justification::topLeft);
            }
        }

        // reverse swell (mirrored glow)
        if (hasSwell)
        {
            const auto& env = render->swellEnvelope;
            const int n = (int) env.size();
            const float mid = plot.getBottom() - plot.getHeight() * 0.5f;
            const float half = plot.getHeight() * 0.48f;
            Path p;
            p.startNewSubPath (plot.getX(), mid);
            for (int i = 0; i < n; ++i)
                p.lineTo (plot.getX() + ((float) i + 0.5f) / (float) n * plot.getWidth(), mid - env[(size_t) i] * half);
            p.lineTo (plot.getRight(), mid);
            for (int i = n - 1; i >= 0; --i)
                p.lineTo (plot.getX() + ((float) i + 0.5f) / (float) n * plot.getWidth(), mid + env[(size_t) i] * half);
            p.closeSubPath();
            const float alpha = hasStutter ? 0.45f : 0.9f;
            g.setGradientFill (ColourGradient (t.accentB.withAlpha (alpha * 0.4f), plot.getX(), 0.0f, t.accentA.withAlpha (alpha), dropX, 0.0f, false));
            g.fillPath (p);
            g.setColour (t.accentC.withAlpha (hasStutter ? 0.5f : 0.9f));
            g.strokePath (p, PathStrokeType (1.2f));
        }

        if (hasStutter)
        {
            // hits
            g.setGradientFill (ColourGradient (t.accentA, plot.getX(), 0.0f, t.accentB, dropX, 0.0f, false));
            for (const auto& gr : render->grains)
            {
                const float x0 = xAt (gr.startSec);
                const float w  = jmax (1.5f, xAt (gr.startSec + gr.lengthSec) - x0 - 1.0f);
                const float h  = plot.getHeight() * (0.15f + 0.65f * gr.gain);
                g.fillRoundedRectangle (x0, plot.getBottom() - h, w, h, jmin (2.0f, w * 0.5f));
            }
            g.setColour (t.accentC.withAlpha (0.9f));
            for (const auto& gr : render->grains)
            {
                const float x0 = xAt (gr.startSec);
                const float w  = jmax (1.5f, xAt (gr.startSec + gr.lengthSec) - x0 - 1.0f);
                const float h  = plot.getHeight() * (0.15f + 0.65f * gr.gain);
                g.fillRect (x0, plot.getBottom() - h, w, 2.0f);
            }

            // filter curve (dashed)
            if (s.sweep > 0.001f)
            {
                Path fp;
                for (int i = 0; i <= 120; ++i)
                {
                    const float p = (float) i / 120.0f;
                    const float norm = std::log (lowpassAt (p, s.sweep) / 20.0f) / std::log (1000.0f);
                    const Point<float> pt (xAt (p * render->dropSec), plot.getBottom() - norm * plot.getHeight() * 0.95f);
                    if (i == 0) fp.startNewSubPath (pt); else fp.lineTo (pt);
                }
                Path dashed;
                const float d[] = { 5.0f, 4.0f };
                PathStrokeType (1.4f).createDashedStroke (dashed, fp, d, 2);
                g.setColour (t.text.withAlpha (0.55f));
                g.fillPath (dashed);
            }

            // pitch curve
            Path pp;
            for (int i = 0; i <= 120; ++i)
            {
                const float p = (float) i / 120.0f;
                const Point<float> pt (xAt (p * render->dropSec),
                                       plot.getBottom() - 4.0f - (pitchAt (p, s.pitch) / 24.0f) * (plot.getHeight() - 8.0f));
                if (i == 0) pp.startNewSubPath (pt); else pp.lineTo (pt);
            }
            g.setColour (t.accentC.withAlpha (0.25f));
            g.strokePath (pp, PathStrokeType (6.0f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (t.accentC);
            g.strokePath (pp, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        // landing syllable after the drop
        if (render->landSec > 0.0)
        {
            auto land = Rectangle<float> (dropX + 2.0f, plot.getY() + plot.getHeight() * 0.3f,
                                          jmax (4.0f, xAt (render->dropSec + render->landSec) - dropX - 2.0f), plot.getHeight() * 0.4f);
            g.setColour (t.text.withAlpha (0.25f));
            g.fillRoundedRectangle (land, 4.0f);
            g.setColour (t.text);
            g.drawRoundedRectangle (land, 4.0f, 1.0f);
            g.setFont (sunFont (10.0f, true));
            g.drawText ("LAND", land, Justification::centred);
        }

        // drop marker
        g.setColour (t.accentB);
        g.fillRect (dropX - 1.0f, plot.getY(), 2.0f, plot.getHeight());
        auto dropPill = Rectangle<float> (dropX - 46.0f, plot.getY() + 1.0f, 40.0f, 15.0f);
        g.setColour (wellColour (t).withAlpha (0.85f));
        g.fillRoundedRectangle (dropPill, 7.0f);
        g.setColour (t.accentB);
        g.setFont (sunFont (10.5f, true));
        g.drawText ("DROP", dropPill, Justification::centred);

        // playhead
        const float prog = proc.getPlayProgress();
        if (prog >= 0.0f)
        {
            const float x = plot.getX() + prog * plot.getWidth();
            g.setColour (t.text.withAlpha (0.18f));
            g.fillRect (x - 4.0f, plot.getY(), 8.0f, plot.getHeight());
            g.setColour (t.text);
            g.fillRect (x - 1.0f, plot.getY(), 2.0f, plot.getHeight());
        }
    }

private:
    SunRiserProcessor& proc;
};

//==============================================================================
class PlayButton : public Button
{
public:
    PlayButton() : Button ("Preview") { setTooltip ("Hear the riser (it also plays from any MIDI note)"); }
    void setPlaying (bool p) { if (p != playing) { playing = p; repaint(); } }

    void paintButton (Graphics& g, bool over, bool down) override
    {
        const auto& t = themeOf (*this);
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float corner = r.getHeight() * 0.5f;
        g.setColour (t.light ? Colours::white : t.panel.brighter (0.12f));
        g.fillRoundedRectangle (r, corner);
        g.setGradientFill (ColourGradient (t.accentA, r.getTopLeft(), t.accentB, r.getBottomRight(), false));
        g.drawRoundedRectangle (r, corner, over ? 2.0f : 1.5f);
        if (down) { g.setColour (t.accentA.withAlpha (0.2f)); g.fillRoundedRectangle (r, corner); }

        auto icon = Rectangle<float> (12.0f, 12.0f).withCentre ({ r.getX() + 24.0f, r.getCentreY() });
        g.setColour (t.accentA);
        if (playing) g.fillRoundedRectangle (icon.reduced (1.0f), 2.0f);
        else
        {
            Path tri;
            tri.addTriangle (icon.getX() + 1.0f, icon.getY(), icon.getX() + 1.0f, icon.getBottom(), icon.getRight() + 1.0f, icon.getCentreY());
            g.fillPath (tri);
        }
        g.setColour (t.text);
        g.setFont (sunFont (13.0f, true));
        g.drawText (playing ? "STOP" : "PREVIEW", r.withTrimmedLeft (40.0f).withTrimmedRight (14.0f), Justification::centred);
    }

private:
    bool playing = false;
};

//==============================================================================
class DragOutButton : public Component, public SettableTooltipClient
{
public:
    explicit DragOutButton (SunRiserProcessor& p) : proc (p)
    {
        setTooltip ("Drag the rendered riser onto an audio track in your DAW - or click to save it as a WAV file");
        setMouseCursor (MouseCursor::DraggingHandCursor);
    }

    std::function<void()> onClick;

    void paint (Graphics& g) override
    {
        const auto& t = themeOf (*this);
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float corner = r.getHeight() * 0.5f;
        g.setGradientFill (ColourGradient (t.accentA, r.getTopLeft(), t.accentB, r.getBottomRight(), false));
        g.fillRoundedRectangle (r, corner);
        if (isMouseOver()) { g.setColour (Colours::white.withAlpha (0.12f)); g.fillRoundedRectangle (r, corner); }
        if (! isEnabled()) { g.setColour (t.bgTop.withAlpha (0.55f)); g.fillRoundedRectangle (r, corner); }

        // drag icon: arrow out of a tray
        const auto c = Point<float> (r.getX() + 24.0f, r.getCentreY());
        Path arrow;
        arrow.addArrow ({ c.x, c.y + 3.0f, c.x, c.y - 8.0f }, 2.0f, 8.0f, 5.0f);
        g.setColour (Colours::white);
        g.fillPath (arrow);
        g.fillRect (c.x - 7.0f, c.y + 5.0f, 14.0f, 2.0f);
        g.setFont (sunFont (13.0f, true));
        g.drawText ("DRAG RISER", r.withTrimmedLeft (40.0f).withTrimmedRight (14.0f), Justification::centred);
    }

    void mouseEnter (const MouseEvent&) override { repaint(); }
    void mouseExit (const MouseEvent&) override  { repaint(); }
    void mouseDown (const MouseEvent&) override  { dragging = false; }

    void mouseDrag (const MouseEvent& e) override
    {
        if (dragging || ! isEnabled() || e.getDistanceFromDragStart() < 5) return;
        dragging = true;
        const auto f = proc.exportRiserToTempFile();
        if (f.existsAsFile())
            DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
    }

    void mouseUp (const MouseEvent& e) override
    {
        if (! dragging && isEnabled() && getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
        dragging = false;
    }

private:
    SunRiserProcessor& proc;
    bool dragging = false;
};

//==============================================================================
class MainPanel : public Component, public FileDragAndDropTarget, private Timer
{
public:
    MainPanel (SunRiserProcessor& p, SunRiserEditor& ed)
        : proc (p), editor (ed), wave (p), riser (p), dragOut (p),
          lengthSeg (*p.apvts.getParameter (IDs::length),    { "1 BAR", "2 BARS", "4 BARS", "8 BARS" },
                     "How many bars the riser lasts. It ends exactly on the bar line - start your MIDI note this many bars before the drop.", 11.5f),
          feelSeg   (*p.apvts.getParameter (IDs::mode),      { "Smooth", "Steps" },
                     "Smooth: repeats speed up continuously.  Steps: classic 1/4 > 1/8 > 1/16 snare-roll doubling, locked to the grid."),
          fromSeg   (*p.apvts.getParameter (IDs::startRate), { "1/2", "1/4", "1/8" }, "Repeat rate at the start of the riser"),
          toSeg     (*p.apvts.getParameter (IDs::endRate),   { "1/16", "1/32", "1/64" }, "Repeat rate at the very end, just before the drop"),
          modeSeg   (*p.apvts.getParameter (IDs::engine),    { "STUTTER", "REVERSE", "BOTH" },
                     "Stutter: accelerating repeats.  Reverse: reversed-reverb swell into the drop.  Both: layer them.", 11.5f),
          curve  (p.apvts, IDs::curve,  "Curve",  "How late the acceleration kicks in. Higher = stays slow longer, then rushes into the drop"),
          chop   (p.apvts, IDs::chop,   "Chop",   "How much of each gap the syllable fills. Lower = tighter, choppier hits"),
          pitch  (p.apvts, IDs::pitch,  "Pitch",  "How far the syllable pitches up by the drop"),
          sweep  (p.apvts, IDs::sweep,  "Sweep",  "Filter sweep: starts dark and opens up, thinning the lows into the drop"),
          space  (p.apvts, IDs::space,  "Space",  "Reverb that builds as the riser grows"),
          swell  (p.apvts, IDs::swell,  "Swell",  "Volume fade-in, from quiet to full"),
          width  (p.apvts, IDs::width,  "Width",  "Stereo spread: hits ping-pong left/right more as it builds"),
          output (p.apvts, IDs::output, "Output", "Output level"),
          revSize (p.apvts, IDs::revSize, "Size",  "How much of the riser the reverse swell fills (reverb length follows the bars)"),
          revTone (p.apvts, IDs::revTone, "Tone",  "Darker or brighter reverb"),
          revGap  (p.apvts, IDs::revPre,  "Gap",   "Cut the swell this long before the drop, leaving a breath of silence"),
          blend   (p.apvts, IDs::blend,   "Blend", "Both mode: balance between the stutter and the reverse swell"),
          output2 (p.apvts, IDs::output,  "Output", "Output level")
    {
        for (auto* c : std::initializer_list<Component*> { &wave, &riser, &play, &dragOut, &themeBox, &loadBtn, &autoBtn,
                                                           &lengthSeg, &feelSeg, &fromSeg, &toSeg,
                                                           &curve, &chop, &pitch, &sweep, &space, &swell, &width, &output,
                                                           &modeSeg, &landBtn, &pageSeg, &revSize, &revTone, &revGap, &blend, &output2 })
            addAndMakeVisible (c);

        const auto& themes = getThemes();
        for (int i = 0; i < (int) themes.size(); ++i)
            themeBox.addItem (themes[(size_t) i].name, i + 1);
        themeBox.setSelectedId (jlimit (0, (int) themes.size() - 1, proc.getThemeIndex()) + 1, dontSendNotification);
        themeBox.setTooltip ("Colour theme");
        themeBox.onChange = [this] { editor.applyTheme (themeBox.getSelectedId() - 1); };

        play.onClick = [this] { if (proc.isVoicePlaying()) proc.stopPlayback(); else proc.startPreview(); };
        dragOut.onClick = [this] { saveRiser(); };
        wave.onBrowse = [this] { browse(); };

        loadBtn.setTooltip ("Load an audio file (WAV, AIFF, FLAC, MP3, OGG)");
        loadBtn.onClick = [this] { browse(); };
        autoBtn.setTooltip ("Find the first syllable again automatically (or double-click the waveform)");
        autoBtn.onClick = [this] { proc.autoDetectSlice(); };

        landBtn.setClickingTogglesState (true);
        landBtn.setTooltip ("Also play the original syllable right on the drop");
        landAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, IDs::land, landBtn);
        pageSeg.setTooltip ("Which engine's controls to show below");
        pageSeg.onSelect = [this] (int i) { page = i; updatePage(); };
        modeSeg.onChange = [this] { modeChanged(); };
        modeChanged();

        wave.refresh();
        startTimerHz (30);
    }

    //== drag & drop of audio files anywhere on the plugin ==
    bool isInterestedInFileDrag (const StringArray& files) override
    {
        for (auto& f : files)
            if (File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;caf;wma"))
                return true;
        return false;
    }
    void fileDragEnter (const StringArray&, int, int) override { wave.dragHover = true; wave.repaint(); }
    void fileDragExit (const StringArray&) override            { wave.dragHover = false; wave.repaint(); }
    void filesDropped (const StringArray& files, int, int) override
    {
        wave.dragHover = false;
        load (File (files[0]));
    }

    void paint (Graphics& g) override
    {
        const auto& t = themeOf (*this);
        const auto b = getLocalBounds().toFloat();

        g.setGradientFill (ColourGradient (t.bgTop, 0.0f, 0.0f, t.bgBottom, 0.0f, b.getHeight(), false));
        g.fillAll();
        g.setGradientFill (ColourGradient (t.accentA.withAlpha (t.light ? 0.16f : 0.22f), 46.0f, 36.0f,
                                           t.accentA.withAlpha (0.0f), 380.0f, 36.0f, true));
        g.fillRect (b);

        paintLogo (g, { 22.0f, 12.0f, 50.0f, 46.0f }, t);
        g.setGradientFill (ColourGradient (t.accentC, 80.0f, 0.0f, t.accentB, 260.0f, 0.0f, false));
        g.setFont (sunFont (30.0f, true));
        g.drawText ("SUNRISER", Rectangle<float> (82.0f, 12.0f, 300.0f, 32.0f), Justification::centredLeft);
        g.setColour (t.textDim);
        g.setFont (sunFont (12.0f, true));
        g.drawText ("FIRST-SYLLABLE RISER", Rectangle<float> (84.0f, 42.0f, 300.0f, 16.0f), Justification::centredLeft);

        section (g, t, sourceArea,  "SOURCE", "drop audio  -  drag the handles to fine-tune the syllable  -  scroll to zoom");
        section (g, t, riserArea,   "RISER",  "");
        if (page == 0)
        {
            section (g, t, rhythmArea,  "STUTTER RHYTHM", "");
            section (g, t, toneArea,    "TONE",   "");
            section (g, t, dynArea,     "DYNAMICS", "");

            g.setFont (sunFont (11.5f, true));
            for (auto [name, seg] : { std::pair<const char*, Component*> { "FEEL", &feelSeg }, { "FROM", &fromSeg }, { "TO", &toSeg } })
            {
                g.setColour (t.textDim);
                g.drawText (name, Rectangle<int> (rhythmArea.getX() + 14, seg->getY(), 60, seg->getHeight()), Justification::centredLeft);
            }
        }
        else
        {
            section (g, t, revArea, "REVERSE SWELL", "");
            section (g, t, mixArea, "MIX", "");

            auto how = Rectangle<float> ((float) revArea.getX() + 284.0f, (float) revArea.getY() + 44.0f, (float) revArea.getWidth() - 304.0f, 136.0f);
            g.setColour (t.accentA);
            g.setFont (sunFont (12.0f, true));
            g.drawText ("HOW IT WORKS", how.removeFromTop (18.0f), Justification::centredLeft);
            g.setColour (t.textDim);
            g.setFont (sunFont (12.5f));
            g.drawFittedText ("The first syllable is reversed, sent through a long reverb, then reversed again. "
                              "The reverb swells up out of nothing and peaks right on the drop - "
                              "the classic reverse-reverb riser, done for you.\n\n"
                              "Size = how much of the riser it fills.  Tone = dark to bright.  "
                              "Gap = a breath of silence before the drop.",
                              how.reduced (0.0f, 4.0f).toNearestInt(), Justification::topLeft, 8, 1.0f);

            if (modeSeg.getIndex() != 2)
            {
                g.setColour (t.textDim);
                g.setFont (sunFont (11.0f));
                g.drawText ("Blend works in BOTH mode", Rectangle<int> (mixArea.getX(), mixArea.getBottom() - 22, mixArea.getWidth(), 16), Justification::centred);
            }
        }

        g.setColour (statusUntil > Time::getMillisecondCounter() ? t.accentA : t.textDim);
        g.setFont (sunFont (12.0f, statusUntil > Time::getMillisecondCounter()));
        g.drawText (statusUntil > Time::getMillisecondCounter() ? status
                      : String ("1. Drop a sound     2. Shape the riser     3. Play any MIDI note at the start, or drag the riser onto an audio track"),
                    footerArea, Justification::centred);
    }

    void resized() override
    {
        const int W = getWidth();
        dragOut.setBounds (W - 22 - 170, 18, 170, 38);
        play.setBounds (dragOut.getX() - 12 - 136, 18, 136, 38);
        themeBox.setBounds (play.getX() - 12 - 150, 22, 150, 30);

        sourceArea = { 20, 76, W - 40, 166 };
        loadBtn.setBounds (sourceArea.getRight() - 12 - 80, sourceArea.getY() + 8, 80, 26);
        autoBtn.setBounds (loadBtn.getX() - 8 - 110, sourceArea.getY() + 8, 110, 26);
        wave.setBounds (sourceArea.reduced (12).withTrimmedTop (30));

        riserArea = { 20, 252, W - 40, 176 };
        riser.setBounds (riserArea.reduced (12).withTrimmedTop (28));
        lengthSeg.setBounds (riserArea.getX() + 92, riserArea.getY() + 7, 250, 27);
        modeSeg.setBounds (lengthSeg.getRight() + 12, riserArea.getY() + 7, 270, 27);
        landBtn.setBounds (modeSeg.getRight() + 12, riserArea.getY() + 7, 132, 27);
        pageSeg.setBounds (riserArea.getRight() - 12 - 214, riserArea.getY() + 7, 214, 27);

        rhythmArea = { 20, 438, 450, 196 };
        toneArea   = { 480, 438, 265, 196 };
        dynArea    = { 755, 438, W - 20 - 755, 196 };

        Segmented* segs[] = { &feelSeg, &fromSeg, &toSeg };
        for (int i = 0; i < 3; ++i)
            segs[i]->setBounds (rhythmArea.getX() + 66, rhythmArea.getY() + 50 + i * 46, 200, 32);

        revArea = { 20, 438, 640, 196 };
        mixArea = { 670, 438, W - 20 - 670, 196 };
        Knob* rev[] = { &revSize, &revTone, &revGap };
        for (int i = 0; i < 3; ++i)
            rev[i]->setBounds (revArea.getX() + 14 + i * 88, revArea.getY() + 46, 84, 136);
        blend.setBounds   (mixArea.getCentreX() - 100, mixArea.getY() + 40, 90, 136);
        output2.setBounds (mixArea.getCentreX() + 10,  mixArea.getY() + 40, 90, 136);

        curve.setBounds (rhythmArea.getX() + 278, rhythmArea.getY() + 46, 80, 136);
        chop.setBounds  (rhythmArea.getX() + 362, rhythmArea.getY() + 46, 80, 136);

        Knob* tone[] = { &pitch, &sweep, &space };
        Knob* dyn[]  = { &swell, &width, &output };
        for (int i = 0; i < 3; ++i)
        {
            tone[i]->setBounds (toneArea.getX() + 10 + i * 82, toneArea.getY() + 46, 80, 136);
            dyn[i]->setBounds  (dynArea.getX() + 10 + i * 82,  dynArea.getY() + 46, 80, 136);
        }
        footerArea = { 20, 640, W - 40, 22 };
    }

private:
    void modeChanged()
    {
        const int mode = modeSeg.getIndex();
        if (mode == 0) page = 0;
        if (mode == 1) page = 1;
        pageSeg.setIndex (page);
        pageSeg.setVisible (mode == 2);
        blend.setEnabled (mode == 2);
        updatePage();
    }

    void updatePage()
    {
        for (auto* c : std::initializer_list<Component*> { &feelSeg, &fromSeg, &toSeg, &curve, &chop, &pitch, &sweep, &space, &swell, &width, &output })
            c->setVisible (page == 0);
        for (auto* c : std::initializer_list<Component*> { &revSize, &revTone, &revGap, &blend, &output2 })
            c->setVisible (page == 1);
        repaint();
    }

    void timerCallback() override
    {
        if (proc.getSourceVersion() != lastSource || proc.getSliceVersion() != lastSlice)
        {
            lastSource = proc.getSourceVersion();
            lastSlice = proc.getSliceVersion();
            wave.refresh();
        }
        const bool playingNow = proc.isVoicePlaying();
        if (proc.getRenderVersion() != lastRender || playingNow || wasPlaying)
        {
            lastRender = proc.getRenderVersion();
            riser.repaint();
        }
        wasPlaying = playingNow;
        play.setPlaying (playingNow);
        dragOut.setEnabled (proc.getLatestRender() != nullptr && proc.getSource() != nullptr);

        if (statusUntil != 0 && statusUntil < Time::getMillisecondCounter())
        {
            statusUntil = 0;
            repaint (footerArea);
        }
    }

    void section (Graphics& g, const Theme& t, Rectangle<int> area, const String& title, const String& hint)
    {
        auto r = area.toFloat();
        g.setColour (t.panel.withAlpha (t.light ? 0.85f : 0.72f));
        g.fillRoundedRectangle (r, 14.0f);
        g.setColour (t.panelEdge.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.0f);

        g.setGradientFill (ColourGradient (t.accentA, r.getX() + 14.0f, 0.0f, t.accentB, r.getX() + 22.0f, 0.0f, false));
        g.fillRoundedRectangle (r.getX() + 14.0f, r.getY() + 14.0f, 8.0f, 8.0f, 4.0f);
        g.setColour (t.text);
        g.setFont (sunFont (12.5f, true));
        g.drawText (title, Rectangle<float> (r.getX() + 28.0f, r.getY() + 8.0f, 200.0f, 20.0f), Justification::centredLeft);
        if (hint.isNotEmpty())
        {
            g.setColour (t.textDim);
            g.setFont (sunFont (11.5f));
            g.drawText (hint, Rectangle<float> (r.getX() + 100.0f, r.getY() + 8.0f, 600.0f, 20.0f), Justification::centredLeft);
        }
    }

    static void paintLogo (Graphics& g, Rectangle<float> r, const Theme& t)
    {
        const float horizon = r.getY() + r.getHeight() * 0.72f;
        const Point<float> c (r.getCentreX(), horizon);
        const float rad = r.getWidth() * 0.30f;

        g.setColour (t.accentC.withAlpha (0.9f));
        for (int i = 0; i < 7; ++i)
        {
            const float a = MathConstants<float>::pi * (-0.5f + (float) (i - 3) * 0.24f);
            const Point<float> dir (std::cos (a), std::sin (a));
            g.drawLine ({ c + dir * (rad + 4.0f), c + dir * (rad + 10.0f) }, 2.2f);
        }
        {
            Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (Rectangle<float> (r.getX(), r.getY(), r.getWidth(), horizon - r.getY()).toNearestInt());
            g.setGradientFill (ColourGradient (t.accentC, c.x, c.y - rad, t.accentB, c.x, c.y, false));
            g.fillEllipse (Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (c));
        }
        g.setGradientFill (ColourGradient (t.accentA, r.getX(), 0.0f, t.accentB, r.getRight(), 0.0f, false));
        g.fillRoundedRectangle (r.getX(), horizon + 1.0f, r.getWidth(), 3.0f, 1.5f);
        g.fillRoundedRectangle (r.getX() + 8.0f, horizon + 7.0f, r.getWidth() - 16.0f, 2.5f, 1.25f);
    }

    void showStatus (const String& s)
    {
        status = s;
        statusUntil = Time::getMillisecondCounter() + 5000;
        repaint (footerArea);
    }

    void load (const File& f)
    {
        if (proc.loadAudioFile (f)) showStatus ("Loaded " + f.getFileName() + "  -  first syllable found");
        else                        showStatus (proc.getLastError());
        wave.refresh();
    }

    void browse()
    {
        chooser = std::make_unique<FileChooser> ("Load a sound for SunRiser", File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    }

    void saveRiser()
    {
        const auto temp = proc.exportRiserToTempFile();
        const auto suggested = File::getSpecialLocation (File::userDesktopDirectory).getChildFile (temp.getFileName().isNotEmpty() ? temp.getFileName() : "SunRiser.wav");
        chooser = std::make_unique<FileChooser> ("Save riser as WAV", suggested, "*.wav");
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                              [this] (const FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f == File()) return;
                                  f = f.withFileExtension ("wav");
                                  showStatus (proc.exportRiser (f).existsAsFile() ? "Saved " + f.getFullPathName() : String ("Couldn't save the file"));
                              });
    }

    SunRiserProcessor& proc;
    SunRiserEditor& editor;
    WaveformView wave;
    RiserView riser;
    PlayButton play;
    DragOutButton dragOut;
    ComboBox themeBox;
    TextButton loadBtn { "LOAD" }, autoBtn { "AUTO-DETECT" };
    Segmented lengthSeg, feelSeg, fromSeg, toSeg, modeSeg;
    Knob curve, chop, pitch, sweep, space, swell, width, output;
    Knob revSize, revTone, revGap, blend, output2;
    TextButton landBtn { "LAND ON DROP" };
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> landAttachment;
    LocalSegmented pageSeg { { "EDIT STUTTER", "EDIT REVERSE" }, 11.0f };
    int page = 0;
    std::unique_ptr<FileChooser> chooser;

    Rectangle<int> sourceArea, riserArea, rhythmArea, toneArea, dynArea, revArea, mixArea, footerArea;
    int lastSource = -1, lastSlice = -1, lastRender = -1;
    bool wasPlaying = false;
    String status;
    uint32 statusUntil = 0;
};

//==============================================================================
SunRiserEditor::SunRiserEditor (SunRiserProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    const auto& themes = getThemes();
    lnf.setTheme (themes[(size_t) jlimit (0, (int) themes.size() - 1, proc.getThemeIndex())]);
    setLookAndFeel (&lnf);

    panel = std::make_unique<MainPanel> (p, *this);
    addAndMakeVisible (*panel);
    tooltips = std::make_unique<TooltipWindow> (this, 700);
    sendLookAndFeelChange();

    setResizable (true, true);
    setResizeLimits (baseW * 7 / 10, baseH * 7 / 10, baseW * 2, baseH * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) baseW / (double) baseH);
    setSize (baseW, baseH);
}

SunRiserEditor::~SunRiserEditor()
{
    tooltips.reset();
    panel.reset();
    setLookAndFeel (nullptr);
}

void SunRiserEditor::applyTheme (int index)
{
    const auto& themes = getThemes();
    index = jlimit (0, (int) themes.size() - 1, index);
    proc.setThemeIndex (index);
    lnf.setTheme (themes[(size_t) index]);
    sendLookAndFeelChange();
    repaint();
}

void SunRiserEditor::paint (Graphics& g)
{
    g.fillAll (lnf.getTheme().bgTop);
}

void SunRiserEditor::resized()
{
    panel->setBounds (0, 0, baseW, baseH);
    panel->setTransform (AffineTransform::scale ((float) getWidth() / (float) baseW));
}
