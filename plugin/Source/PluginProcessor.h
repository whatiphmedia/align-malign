#pragma once
#include <JuceHeader.h>
#include "am/session.h"
#include "am/convolver.h"
#include "am/phantom.h"
#include "am/audioshare.h"
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
    void installLiveScript();
    void setGroup(int slotId, const juce::String& group);   // any window can set any linked track's group
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
    float lastFAmt_ = -1.f, lastPMono_ = -1.f, lastSRot_ = -999.f;

    // linked-instance parameter following (message thread)
    uint64_t linkSeqSeen_ = 0;
    std::map<juce::String, float> lastPublished_;
    bool applyingLinked_ = false, adopted_ = false;

    // meters + spectrum for the UI (written by the audio thread)
    std::atomic<float> inLevel_{0.f}, outLevel_{0.f}, inPeakHold_{0.f}, outPeakHold_{0.f};
    static constexpr int kScope = 2048;
    std::array<float, kScope> scopeRing_{};
    std::atomic<int> scopeWrite_{0};

    juce::AudioParameterFloat *pAlign_, *pMatch_, *pWidth_, *pMotion_, *pMix_;
    juce::AudioParameterChoice* pFlavour_;
    juce::AudioParameterBool *pAB_, *pDelta_, *pLatAuto_;
    juce::AudioParameterFloat *pLatMs_, *pLinkId_, *pFAmt_ = nullptr, *pPMono_ = nullptr, *pSRot_ = nullptr;
    // Malodic notes (Refine flavour > MALODICS)
    am::PhantomNotes phantom_;
    std::atomic<bool> phantomOn_{ false };
    int lastFlavourIdx_ = -1;
    juce::AudioParameterChoice *pPpMode_ = nullptr, *pPpSource_ = nullptr;
    juce::AudioParameterFloat *pPpAssist_ = nullptr, *pPpCarrier_ = nullptr, *pPpPurity_ = nullptr, *pPpStrength_ = nullptr, *pPpAttack_ = nullptr,
                              *pPpRelease_ = nullptr, *pPpGlide_ = nullptr, *pPpBend_ = nullptr, *pPpTame_ = nullptr;
    juce::AudioParameterBool *pPpFold_ = nullptr, *pPpFill_ = nullptr, *pPpConst_ = nullptr;
    juce::AudioParameterInt* pPpPartials_ = nullptr;
    std::shared_ptr<am::SharedRing> ownRing_;          // this track's input, for other tracks' Phantom notes
    std::shared_ptr<am::SharedRing> srcRing_, srcRingPrev_;   // the chosen source track (kept alive one extra tick)
    std::atomic<am::SharedRing*> srcRingRaw_{ nullptr };
    std::atomic<int> ppSourceId_{ -1 };                 // slot id of the session track used as the source
    juce::AudioBuffer<float> srcBuf_, scDelayed_;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> scDelay_{ am::kIRLen * 2 };
    std::array<std::atomic<int>, 8> ppSounding_{};      // notes sounding, for the interface (-1 = none)
    void updatePhantomParams();
    juce::String pendingSourceName_;                   // restored source track, matched by name once it appears
    int linkId_ = 0;                                   // identifies this instance to the Live control surface
    juce::String latSource_ = "none", latTrack_, msg_; // shown on the Pro page
    double latUsedMs_ = 0;
    bool msgOk_ = true;

    friend class AlignMalignEditor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlignMalignProcessor)
};
