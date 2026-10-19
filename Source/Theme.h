#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

struct Theme
{
    juce::String name;
    juce::Colour bgTop, bgBottom, panel, panelEdge, accentA, accentB, accentC, text, textDim;
    bool light = false;
};

inline const std::vector<Theme>& getThemes()
{
    using C = juce::Colour;
    static const std::vector<Theme> themes {
        //  name           bgTop          bgBottom       panel          edge           accentA        accentB        accentC        text           textDim
        { "Sunrise",     C (0xff1c0f22), C (0xff3b1631), C (0xff27142d), C (0xff4a2a4c), C (0xffff8a3d), C (0xffff3d7f), C (0xffffd166), C (0xfffff3e6), C (0xffb99db2) },
        { "Golden Hour", C (0xff1a1206), C (0xff3b2208), C (0xff2a1c0c), C (0xff5a3c16), C (0xffffb703), C (0xfffb5607), C (0xfffff1b5), C (0xfffff6e5), C (0xffc9a97a) },
        { "Dusk",        C (0xff0e0d22), C (0xff2a1746), C (0xff1b1737), C (0xff3a3266), C (0xff8b5cf6), C (0xffec4899), C (0xff22d3ee), C (0xfff1eefe), C (0xff9d95c9) },
        { "Ocean Dawn",  C (0xff061920), C (0xff0e3442), C (0xff0e2933), C (0xff1f4a5a), C (0xff2ec4b6), C (0xff3a86ff), C (0xffffbe0b), C (0xffe8fbff), C (0xff86abb6) },
        { "Daylight",    C (0xfffcf6ee), C (0xfff3e0cf), C (0xffffffff), C (0xffead6c4), C (0xffff6b35), C (0xffe63973), C (0xff1d3557), C (0xff2b2118), C (0xff8c7b6c), true },
    };
    return themes;
}

inline juce::Font sunFont (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
class SunLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SunLookAndFeel() { setTheme (getThemes().front()); }

    const Theme& getTheme() const { return theme; }

    void setTheme (const Theme& t)
    {
        theme = t;
        const auto onAccent = juce::Colours::white;
        setColour (juce::ResizableWindow::backgroundColourId, t.bgTop);
        setColour (juce::TextButton::buttonColourId, t.panel);
        setColour (juce::TextButton::buttonOnColourId, t.accentA);
        setColour (juce::TextButton::textColourOffId, t.text);
        setColour (juce::TextButton::textColourOnId, onAccent);
        setColour (juce::ComboBox::backgroundColourId, t.panel);
        setColour (juce::ComboBox::textColourId, t.text);
        setColour (juce::ComboBox::outlineColourId, t.panelEdge);
        setColour (juce::ComboBox::arrowColourId, t.text);
        setColour (juce::PopupMenu::backgroundColourId, t.panel);
        setColour (juce::PopupMenu::textColourId, t.text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, t.accentA);
        setColour (juce::PopupMenu::highlightedTextColourId, onAccent);
        setColour (juce::Slider::textBoxTextColourId, t.text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, t.accentA.withAlpha (0.4f));
        setColour (juce::Label::textColourId, t.text);
        setColour (juce::Label::textWhenEditingColourId, t.text);
        setColour (juce::Label::outlineWhenEditingColourId, t.accentA);
        setColour (juce::TextEditor::textColourId, t.text);
        setColour (juce::TextEditor::backgroundColourId, t.panel);
        setColour (juce::TextEditor::highlightColourId, t.accentA.withAlpha (0.4f));
        setColour (juce::CaretComponent::caretColourId, t.accentA);
        setColour (juce::TooltipWindow::backgroundColourId, t.panel.brighter (t.light ? 0.0f : 0.15f));
        setColour (juce::TooltipWindow::textColourId, t.text);
        setColour (juce::TooltipWindow::outlineColourId, t.panelEdge);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override
    {
        using namespace juce;
        const auto area = Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
        const float size = jmin (area.getWidth(), area.getHeight());
        const auto box = area.withSizeKeepingCentre (size, size);
        const float r = size * 0.5f, cx = box.getCentreX(), cy = box.getCentreY();
        const float trackW = jmax (3.0f, r * 0.13f);
        const float arcR = r - trackW * 0.5f;
        const float angle = startAngle + pos * (endAngle - startAngle);

        Path track;
        track.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (theme.textDim.withAlpha (theme.light ? 0.22f : 0.18f));
        g.strokePath (track, PathStrokeType (trackW, PathStrokeType::curved, PathStrokeType::rounded));

        if (pos > 0.001f)
        {
            Path value;
            value.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
            g.setGradientFill (ColourGradient (theme.accentA, box.getBottomLeft(), theme.accentB, box.getTopRight(), false));
            g.strokePath (value, PathStrokeType (trackW, PathStrokeType::curved, PathStrokeType::rounded));
        }

        // knob body with a warm glow that grows with the value
        const float kr = r - trackW * 2.1f;
        g.setColour (theme.accentA.withAlpha (0.10f + 0.22f * pos));
        g.fillEllipse (cx - kr - 3.0f, cy - kr - 3.0f, (kr + 3.0f) * 2.0f, (kr + 3.0f) * 2.0f);
        g.setGradientFill (ColourGradient (theme.light ? Colours::white : theme.panel.brighter (0.35f), cx, cy - kr,
                                           theme.light ? theme.bgBottom : theme.panel.darker (0.4f), cx, cy + kr, false));
        g.fillEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f);
        g.setColour (theme.panelEdge);
        g.drawEllipse (cx - kr, cy - kr, kr * 2.0f, kr * 2.0f, 1.0f);

        const Point<float> dir (std::sin (angle), -std::cos (angle));
        g.setColour (theme.text);
        g.drawLine ({ cx + dir.x * kr * 0.3f, cy + dir.y * kr * 0.3f, cx + dir.x * kr * 0.78f, cy + dir.y * kr * 0.78f }, 2.5f);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const float corner = juce::jmin (8.0f, r.getHeight() * 0.5f);
        if (b.getToggleState())
        {
            g.setGradientFill (juce::ColourGradient (theme.accentA, r.getTopLeft(), theme.accentB, r.getBottomRight(), false));
            g.fillRoundedRectangle (r, corner);
        }
        else
        {
            auto c = theme.light ? theme.bgTop : theme.panel.brighter (0.12f);
            if (over) c = c.overlaidWith (theme.accentA.withAlpha (0.12f));
            if (down) c = c.overlaidWith (theme.accentA.withAlpha (0.25f));
            g.setColour (c);
            g.fillRoundedRectangle (r, corner);
            g.setColour (over ? theme.accentA.withAlpha (0.7f) : theme.panelEdge);
            g.drawRoundedRectangle (r, corner, 1.0f);
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override   { return sunFont (juce::jmin (13.0f, h * 0.5f), true); }
    juce::Font getComboBoxFont (juce::ComboBox&) override               { return sunFont (13.0f, true); }
    juce::Font getPopupMenuFont() override                              { return sunFont (14.0f); }
    juce::Font getLabelFont (juce::Label& l) override                   { return l.getFont(); }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (sunFont (12.5f, true));
        l->setJustificationType (juce::Justification::centred);
        return l;
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<int> (0, 0, w, h).toFloat().reduced (1.0f);
        g.setColour (theme.light ? theme.bgTop : theme.panel.brighter (0.12f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (box.isMouseOver (true) ? theme.accentA.withAlpha (0.7f) : theme.panelEdge);
        g.drawRoundedRectangle (r, 8.0f, 1.0f);

        juce::Path arrow;
        const float ax = (float) w - 18.0f, ay = (float) h * 0.5f;
        arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
        arrow.lineTo (ax, ay + 2.5f);
        arrow.lineTo (ax + 4.0f, ay - 2.0f);
        g.setColour (theme.text);
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

private:
    Theme theme;
};
