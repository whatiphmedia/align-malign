#pragma once
#include <JuceHeader.h>
#include "am/session.h"
#include "am/convolver.h"
#include <array>
#include <map>

// One instance per track. Every instance in the host process joins the same am::SessionBus, so all of them
// are analysed together and each receives its own phase map. Signal path (per block):
//
//   L/R -> Mid/Side -> [learn: Mid to the session bus] -> FIR (Mid) + FIR (Side)  (+ mirrored FIRs for Motion)
//       -> Ghost / Width / Delta / A-B / Mix -> L/R
//
// The FIRs are linear-phase and centred, so every instance reports the same latency (kIRLen/2 + the convolver's
// 512-sample block) and the host's delay compensation keeps tracks aligned with each other.
class AlignMalignProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    AlignMalignProcessor();
    ~AlignMalignProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Auto Malign"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void updateTrackProperties(const TrackProperties&) override;

    // ---- used by the editor (message thread)
    juce::AudioProcessorValueTreeState apvts;
    void setParamFromUI(const juce::String& id, const juce::var& value);
    void toggleLearn();
    juce::var makeUIState();               // params + session status + meters for the web UI
    juce::String scope() const { return scope_; }

    static juce::StringArray flavourNames();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;          // pushes control changes to the bus; follows linked instances
    void loadIRs(std::shared_ptr<const am::IRSet> set);
    void publishToLinked(const juce::String& id, float normalised);

    std::shared_ptr<am::Slot> slot_;
    juce::String trackName_ = "Track", scope_ = "session";

    std::unique_ptr<am::PartitionedConvolver> convMid_, convSide_, convMidM_, convSideM_;
    static int latency() { return am::kIRLen / 2 + am::PartitionedConvolver::P; }  // FIR centre + convolver block
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay_{ am::kIRLen * 2 };
    juce::AudioBuffer<float> mid_, side_, midM_, sideM_, dryM_, dryS_;
    std::vector<float> monoScratch_;
    double sampleRate_ = 48000.0;
    std::atomic<bool> prepared_{ false };
    bool stateRestored_ = false;

    // last values pushed to the bus (message thread)
    float lastKnob_ = 99.f, lastMatch_ = -1.f;
    int lastFlavour_ = -1;
    bool lastMotionOn_ = false;

    // linked-instance parameter following (message thread)
    uint64_t linkSeqSeen_ = 0;
    std::map<juce::String, float> lastPublished_;
    bool applyingLinked_ = false, adopted_ = false;

    // meters + spectrum for the UI (written by the audio thread)
    std::atomic<float> inLevel_{0.f}, outLevel_{0.f};
    static constexpr int kScope = 2048;
    std::array<float, kScope> scopeRing_{};
    std::atomic<int> scopeWrite_{0};

    juce::AudioParameterFloat *pAlign_, *pMatch_, *pWidth_, *pMotion_, *pMix_;
    juce::AudioParameterChoice* pFlavour_;
    juce::AudioParameterBool *pAB_, *pDelta_;

    friend class AlignMalignEditor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlignMalignProcessor)
};
