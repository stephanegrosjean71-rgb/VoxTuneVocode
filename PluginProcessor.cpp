#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
struct Preset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;
};

// Rappel : les menus ("key", "scale", "carrierMode") se règlent par leur numéro (0, 1, 2...).
// scale : 0 Chromatic, 1 Major, 2 Natural Minor, 3 Harmonic Minor, 4 Pent. Major, 5 Pent. Minor, 6 Blues, 7 Dorian, 8 Mixolydian
// carrierMode : 0 MIDI, 1 Voice Pitch
const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        { "Init (Robot Vocoder)", {} },

        { "Natural Tune (Soft)",
          { { "scale", 1.f }, { "speed", 180.f }, { "amount", 0.7f }, { "dry", 1.f }, { "vocoder", 0.f } } },

        { "Hard Tune Pop",
          { { "scale", 0.f }, { "speed", 0.f }, { "amount", 1.f }, { "dry", 1.f }, { "vocoder", 0.f } } },

        { "Classic Robot Vocoder",
          { { "speed", 0.f }, { "amount", 1.f }, { "wave", 0.1f }, { "noise", 0.35f }, { "sub", 0.2f },
            { "harmony", 0.f }, { "widthCents", 8.f }, { "bandQ", 4.f }, { "sibilance", 0.6f },
            { "dry", 0.f }, { "vocoder", 1.f } } },

        { "Choir Harmony Vocoder",
          { { "speed", 40.f }, { "wave", 0.5f }, { "noise", 0.2f }, { "sub", 0.2f }, { "harmony", 0.8f },
            { "widthCents", 25.f }, { "glide", 60.f }, { "bandQ", 3.5f },
            { "dry", 0.15f }, { "vocoder", 0.9f } } },

        { "Cyber Whisper",
          { { "speed", 0.f }, { "wave", 0.f }, { "noise", 1.f }, { "sub", 0.f }, { "harmony", 0.f },
            { "bandQ", 6.f }, { "sibilance", 0.9f }, { "dry", 0.f }, { "vocoder", 1.2f } } },

        { "Deep Robot Giant",
          { { "speed", 0.f }, { "wave", 0.4f }, { "sub", 0.8f }, { "harmony", 0.1f }, { "formant", -7.f },
            { "bandQ", 3.f }, { "dry", 0.f }, { "vocoder", 1.f } } },

        { "Chipmunk Android",
          { { "speed", 0.f }, { "wave", 0.2f }, { "sub", 0.f }, { "harmony", 0.4f }, { "formant", 6.f },
            { "bandQ", 4.5f }, { "dry", 0.f }, { "vocoder", 1.f } } },

        { "Square Wave Cyborg",
          { { "speed", 0.f }, { "wave", 1.f }, { "noise", 0.15f }, { "sub", 0.1f }, { "harmony", 0.f },
            { "widthCents", 3.f }, { "glide", 5.f }, { "bandQ", 5.f }, { "dry", 0.f }, { "vocoder", 1.f } } },

        { "Gospel Harmony (MIDI)",
          { { "carrierMode", 0.f }, { "speed", 150.f }, { "amount", 0.5f }, { "wave", 0.35f },
            { "sub", 0.3f }, { "harmony", 0.9f }, { "widthCents", 30.f }, { "carRelease", 1.2f },
            { "envRelease", 120.f }, { "dry", 0.1f }, { "vocoder", 1.f } } },

        { "Dreamy Tuned Voice",
          { { "scale", 4.f }, { "speed", 60.f }, { "amount", 1.f }, { "wave", 0.2f }, { "noise", 0.1f },
            { "sub", 0.1f }, { "harmony", 0.9f }, { "glide", 120.f }, { "sibilance", 0.3f },
            { "dry", 1.f }, { "vocoder", 0.35f } } },
    };
    return list;
}
} // namespace

//==============================================================================
VxProcessor::VxProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout VxProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto add = [&layout] (const char* id, const char* name, float lo, float hi, float def,
                          float centre, const char* unit)
    {
        juce::NormalisableRange<float> range (lo, hi);
        if (centre > 0.f)
            range.setSkewForCentre (centre);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, range, def,
            juce::AudioParameterFloatAttributes().withLabel (unit)));
    };

    // Autotune
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "tuneOn", 1 }, "Autotune On", true));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "key", 1 }, "Key",
        juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));

    juce::StringArray scales;
    for (int i = 0; i < vx::kNumScales; ++i)
        scales.add (vx::scaleName (i));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "scale", 1 }, "Scale", scales, 0));

    add ("speed",  "Retune Speed", 0.f, 500.f, 20.f, 80.f, "ms");
    add ("amount", "Amount",       0.f, 1.f, 1.f, 0.f, "");
    add ("refA",   "Reference A4", 430.f, 450.f, 440.f, 0.f, "Hz");

    // Porteuse
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "carrierMode", 1 }, "Carrier Source",
        juce::StringArray { "MIDI Notes", "Voice Pitch" }, 1));

    add ("wave",       "Wave (Saw>Square)", 0.f, 1.f, 0.3f, 0.f, "");
    add ("noise",      "Noise",    0.f, 1.f, 0.25f, 0.f, "");
    add ("sub",        "Sub",      0.f, 1.f, 0.25f, 0.f, "");
    add ("harmony",    "Harmony",  0.f, 1.f, 0.3f, 0.f, "");
    add ("widthCents", "Width",    0.f, 40.f, 12.f, 0.f, "ct");
    add ("glide",      "Glide",    1.f, 300.f, 25.f, 50.f, "ms");
    add ("carRelease", "Carrier Release", 0.02f, 2.f, 0.25f, 0.3f, "s");

    // Vocodeur
    add ("bandQ",      "Band Sharpness", 1.5f, 10.f, 4.f, 4.f, "");
    add ("formant",    "Formant Shift",  -12.f, 12.f, 0.f, 0.f, "st");
    add ("envAttack",  "Env Attack",     0.5f, 50.f, 4.f, 8.f, "ms");
    add ("envRelease", "Env Release",    5.f, 300.f, 40.f, 50.f, "ms");
    add ("sibilance",  "Sibilance",      0.f, 1.f, 0.5f, 0.f, "");

    // Sortie
    add ("dry",     "Tuned Voice", 0.f, 1.f, 0.5f, 0.f, "");
    add ("vocoder", "Vocoder",     0.f, 2.f, 0.8f, 0.f, "");
    add ("master",  "Master",      -24.f, 12.f, 0.f, 0.f, "dB");

    return layout;
}

//==============================================================================
void VxProcessor::prepareToPlay (double sampleRate, int)
{
    tuner.prepare (sampleRate);
    carrier.prepare (sampleRate);
    vocoder.prepare (sampleRate);
    setLatencySamples (tuner.getLatency());

    dryGain.reset (sampleRate, 0.03);
    vocGain.reset (sampleRate, 0.03);
    masterGain.reset (sampleRate, 0.03);
    dryGain.setCurrentAndTargetValue (get ("dry"));
    vocGain.setCurrentAndTargetValue (get ("vocoder"));
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (get ("master")));
}

bool VxProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void VxProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (buffer.getNumChannels() < 2)
        return;

    const int numSamples = buffer.getNumSamples();

    tuner.setParams (get ("tuneOn") > 0.5f, (int) (get ("key") + 0.5f), (int) (get ("scale") + 0.5f),
                     get ("speed"), get ("amount"), get ("refA"));

    vx::Carrier::Params cp;
    cp.wave = get ("wave");
    cp.noise = get ("noise");
    cp.sub = get ("sub");
    cp.harmony = get ("harmony");
    cp.widthCents = get ("widthCents");
    cp.glideMs = get ("glide");
    cp.release = get ("carRelease");
    carrier.setParams (cp);

    vocoder.setParams (get ("bandQ"), get ("formant"), get ("envAttack"), get ("envRelease"), get ("sibilance"));

    const bool autoMode = ((int) (get ("carrierMode") + 0.5f)) == 1;

    dryGain.setTargetValue (get ("dry"));
    vocGain.setTargetValue (get ("vocoder"));
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (get ("master")));

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    auto softLimit = [] (float x)
    {
        const float a = std::fabs (x);
        if (a <= 0.8f)
            return x;
        const float y = 0.8f + 0.2f * std::tanh ((a - 0.8f) / 0.2f);
        return x < 0.f ? -y : y;
    };

    auto it = midi.begin();

    for (int i = 0; i < numSamples; ++i)
    {
        while (it != midi.end() && (*it).samplePosition <= i)
        {
            const auto m = (*it).getMessage();
            if (m.isNoteOn())                                  carrier.noteOn (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())                            carrier.noteOff (m.getNoteNumber());
            else if (m.isAllNotesOff() || m.isAllSoundOff())   carrier.allOff();
            ++it;
        }

        const float in = 0.5f * (L[i] + R[i]);
        const float tuned = tuner.process (in);

        float cl, cr;
        carrier.process (autoMode, tuner.correctedFreq(), tuner.isVoiced(), cl, cr);

        float vl, vr;
        vocoder.process (tuned, cl, cr, vl, vr);

        const float d = dryGain.getNextValue();
        const float v = vocGain.getNextValue();
        const float g = masterGain.getNextValue();

        L[i] = softLimit ((tuned * d + vl * v) * g);
        R[i] = softLimit ((tuned * d + vr * v) * g);
    }
}

//==============================================================================
juce::AudioProcessorEditor* VxProcessor::createEditor() { return new VxEditor (*this); }

int VxProcessor::getNumPrograms() { return (int) presets().size(); }

const juce::String VxProcessor::getProgramName (int index)
{
    const auto& list = presets();
    return (index >= 0 && index < (int) list.size()) ? juce::String (list[(size_t) index].name) : juce::String();
}

void VxProcessor::setCurrentProgram (int index)
{
    const auto& list = presets();
    if (index < 0 || index >= (int) list.size())
        return;

    currentProgram = index;

    for (auto* prm : getParameters())
        prm->setValueNotifyingHost (prm->getDefaultValue());

    for (const auto& kv : list[(size_t) index].values)
        if (auto* rp = apvts.getParameter (kv.first))
            rp->setValueNotifyingHost (rp->convertTo0to1 (kv.second));
}

void VxProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VxProcessor(); }
