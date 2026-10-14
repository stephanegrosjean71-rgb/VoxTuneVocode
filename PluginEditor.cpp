#include "PluginEditor.h"

namespace
{
const juce::Colour kAccent (0xffe6a85a);
}

VxEditor::VxEditor (VxProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf);
    laf.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
    laf.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff35283a));
    laf.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    laf.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white.withAlpha (0.85f));
    laf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    laf.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
    laf.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    laf.setColour (juce::ToggleButton::tickColourId, kAccent);
    laf.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2a2036));
    laf.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    laf.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff5a4570));
    laf.setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff2a2036));
    laf.setColour (juce::PopupMenu::textColourId, juce::Colours::white);

    addSection ("AUTOTUNE",
                { { "key", "Key" }, { "scale", "Scale" } },
                { { "speed", "Retune Speed" }, { "amount", "Amount" }, { "refA", "Ref A4" } });

    addSection ("CARRIER  (synth that gets vocoded)",
                { { "carrierMode", "Carrier Source" } },
                { { "wave", "Saw > Square" }, { "noise", "Noise" }, { "sub", "Sub" }, { "harmony", "Harmony" },
                  { "widthCents", "Width" }, { "glide", "Glide" }, { "carRelease", "Release" } });

    addSection ("VOCODER  /  OUTPUT",
                {},
                { { "bandQ", "Sharpness" }, { "formant", "Formant" }, { "envAttack", "Env Attack" },
                  { "envRelease", "Env Release" }, { "sibilance", "Sibilance" }, { "dry", "Tuned Voice" },
                  { "vocoder", "Vocoder" }, { "master", "Master" } });

    addAndMakeVisible (tuneOn);
    tuneAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "tuneOn", tuneOn);

    addAndMakeVisible (presetBox);
    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };

    setSize (960, 620);
}

VxEditor::~VxEditor()
{
    setLookAndFeel (nullptr);
}

void VxEditor::addSection (const juce::String& title, const std::vector<Def>& comboDefs,
                           const std::vector<Def>& knobDefs)
{
    Section section;
    section.title = title;

    for (const auto& d : comboDefs)
    {
        auto c = std::make_unique<Combo>();
        c->label.setText (d.name, juce::dontSendNotification);
        c->label.setJustificationType (juce::Justification::centred);
        c->label.setFont (13.0f);

        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (d.id)))
            c->box.addItemList (cp->choices, 1);

        addAndMakeVisible (c->label);
        addAndMakeVisible (c->box);
        c->att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, d.id, c->box);

        Cell cell;
        cell.combo = c.get();
        section.cells.push_back (cell);
        combos.push_back (std::move (c));
    }

    for (const auto& d : knobDefs)
    {
        auto k = std::make_unique<Knob>();

        k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
        if (auto* prm = proc.apvts.getParameter (d.id))
            if (prm->getLabel().isNotEmpty())
                k->slider.setTextValueSuffix (" " + prm->getLabel());

        k->label.setText (d.name, juce::dontSendNotification);
        k->label.setJustificationType (juce::Justification::centred);
        k->label.setFont (13.0f);

        addAndMakeVisible (k->slider);
        addAndMakeVisible (k->label);
        k->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, d.id, k->slider);

        Cell cell;
        cell.knob = k.get();
        section.cells.push_back (cell);
        knobs.push_back (std::move (k));
    }

    sections.push_back (std::move (section));
}

void VxEditor::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff140d1c), 0.f, 0.f,
                                             juce::Colour (0xff2b1630), 0.f, (float) getHeight(), false));
    g.fillAll();

    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (24.0f);
    g.drawText ("VOXTUNE VOCODER", 22, 14, 420, 36, juce::Justification::centredLeft);

    for (const auto& s : sections)
    {
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (s.bounds.toFloat().reduced (2.f), 8.f, 1.f);

        g.setColour (kAccent.withAlpha (0.9f));
        g.setFont (12.0f);
        g.drawText (s.title, s.bounds.getX() + 12, s.bounds.getY() + 6, 460, 16,
                    juce::Justification::centredLeft);
    }
}

void VxEditor::resized()
{
    auto area = getLocalBounds().reduced (20);

    auto header = area.removeFromTop (50);
    presetBox.setBounds (header.removeFromRight (230).reduced (0, 10));
    header.removeFromRight (10);
    tuneOn.setBounds (header.removeFromRight (160).reduced (0, 10));
    area.removeFromTop (6);

    const int rowH = area.getHeight() / (int) sections.size();

    for (auto& s : sections)
    {
        auto row = area.removeFromTop (rowH);
        s.bounds = row;

        auto inner = row.withTrimmedTop (26);

        int numCombos = 0, numKnobs = 0;
        for (auto& c : s.cells)
        {
            if (c.combo != nullptr) ++numCombos;
            if (c.knob != nullptr) ++numKnobs;
        }

        const int comboW = 170;
        const int knobW = numKnobs > 0 ? (inner.getWidth() - comboW * numCombos) / numKnobs : 0;

        for (auto& c : s.cells)
        {
            if (c.combo != nullptr)
            {
                auto cell = inner.removeFromLeft (comboW);
                c.combo->label.setBounds (cell.removeFromTop (18));
                c.combo->box.setBounds (cell.withSizeKeepingCentre (comboW - 24, 28).withY (cell.getY() + 20));
            }
            else
            {
                auto cell = inner.removeFromLeft (knobW);
                c.knob->label.setBounds (cell.removeFromTop (18));
                c.knob->slider.setBounds (cell.reduced (4));
            }
        }
    }
}
