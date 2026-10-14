#pragma once

#include "PluginProcessor.h"

class VxEditor : public juce::AudioProcessorEditor
{
public:
    explicit VxEditor (VxProcessor&);
    ~VxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Def { const char* id; const char* name; };

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };

    struct Combo
    {
        juce::Label label;
        juce::ComboBox box;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> att;
    };

    struct Cell
    {
        Knob* knob = nullptr;
        Combo* combo = nullptr;
    };

    struct Section
    {
        juce::String title;
        std::vector<Cell> cells;
        juce::Rectangle<int> bounds;
    };

    void addSection (const juce::String& title, const std::vector<Def>& combos, const std::vector<Def>& knobDefs);

    VxProcessor& proc;
    juce::LookAndFeel_V4 laf;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Combo>> combos;
    std::vector<Section> sections;

    juce::ToggleButton tuneOn { "Autotune ON" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tuneAtt;
    juce::ComboBox presetBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VxEditor)
};
