#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// The plugin window is the Align/Malign HTML interface running in the system web view (WebView2 on Windows),
// served from binary data inside the plugin. A small event bridge connects it to the processor:
//   UI -> plugin : "param" {id, v}, "action" {name}
//   plugin -> UI : "state" {params, session status, meters, 9 spectrum bands} at 30 fps
class AlignMalignEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit AlignMalignEditor(AlignMalignProcessor&);
    ~AlignMalignEditor() override;
    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff050505)); }

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);
    juce::Array<juce::var> spectrumBands();

    static constexpr int designW = 1376, designH = 1118;   // the interface's design size (landscape)
    bool fixingAspect_ = false;
    AlignMalignProcessor& proc_;
    std::unique_ptr<juce::WebBrowserComponent> web_;
    juce::dsp::FFT fft_{ 11 };                         // 2048 points
    std::vector<float> fftBuf_ = std::vector<float>(4096, 0.f);
    std::vector<float> window_ = std::vector<float>(2048, 0.f);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlignMalignEditor)
};
