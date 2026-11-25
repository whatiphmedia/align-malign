#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <deque>

namespace {

// Session linking for the macro controls: with scope = Session, moving a control on one instance moves it on
// every linked instance (so a 12-track session is driven from any one plugin window).
struct LinkHub {
    struct Ev { uint64_t seq; int source; juce::String id; float v; };
    static LinkHub& get() { static LinkHub h; return h; }

    void publish(int source, const juce::String& id, float v) {
        const std::lock_guard<std::mutex> g(m);
        evs.push_back({ ++seq, source, id, v });
        latest[id] = evs.back();
        while (evs.size() > 512) evs.pop_front();
    }
    std::vector<Ev> since(uint64_t after, uint64_t& newest) {
        const std::lock_guard<std::mutex> g(m);
        std::vector<Ev> out;
        for (auto& e : evs) if (e.seq > after) out.push_back(e);
        newest = seq;
        return out;
    }
    std::vector<Ev> latestValues(uint64_t& newest) {
        const std::lock_guard<std::mutex> g(m);
        std::vector<Ev> out;
        for (auto& kv : latest) out.push_back(kv.second);
        newest = seq;
        return out;
    }
    std::mutex m;
    uint64_t seq = 0;
    std::deque<Ev> evs;
    std::map<juce::String, Ev> latest;
};

const char* kLinked[] = { "alignment", "match", "width", "motion", "mix", "flavour", "ab", "delta" };

juce::String floatsToBase64(const std::vector<float>& v) {
    juce::MemoryBlock mb(v.data(), v.size() * sizeof(float));
    return mb.toBase64Encoding();
}
std::vector<float> base64ToFloats(const juce::String& s) {
    juce::MemoryBlock mb;
    if (!mb.fromBase64Encoding(s)) return {};
    std::vector<float> v(mb.getSize() / sizeof(float));
    if (!v.empty()) std::memcpy(v.data(), mb.getData(), v.size() * sizeof(float));
    return v;
}

} // namespace

juce::StringArray AlignMalignProcessor::flavourNames() {
    return { "FUSE", "ERASE", "GHOST", "PHANTOM", "UNMASK", "ENVELOP", "CURVE", "DISPERSE" };
}

juce::AudioProcessorValueTreeState::ParameterLayout AlignMalignProcessor::createLayout() {
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "alignment", 1 }, "Align / Malign",
                                                NormalisableRange<float>(-100.f, 100.f, 0.1f), 0.f,
                                                AudioParameterFloatAttributes().withLabel("(-) align  (+) malign")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "match", 1 }, "Match", NormalisableRange<float>(0.f, 1.f), 1.f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "width", 1 }, "Width", NormalisableRange<float>(0.f, 1.f), 0.5f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "motion", 1 }, "Motion", NormalisableRange<float>(0.f, 1.f), 0.f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "mix", 1 }, "Mix", NormalisableRange<float>(0.f, 1.f), 1.f));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{ "flavour", 1 }, "Flavour", flavourNames(), 0));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{ "ab", 1 }, "A/B (hear original)", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{ "delta", 1 }, "Delta", false));
    return l;
}

AlignMalignProcessor::AlignMalignProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "AlignMalign", createLayout()) {
    pAlign_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("alignment"));
    pMatch_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("match"));
    pWidth_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("width"));
    pMotion_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("motion"));
    pMix_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("mix"));
    pFlavour_ = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("flavour"));
    pAB_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("ab"));
    pDelta_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("delta"));
    jassert(pAlign_ && pMatch_ && pWidth_ && pMotion_ && pMix_ && pFlavour_ && pAB_ && pDelta_);

    slot_ = am::SessionBus::get().join(0.0);
    slot_->onIR = [this](std::shared_ptr<const am::IRSet> s) { loadIRs(std::move(s)); };
    setLatencySamples(am::kIRLen / 2);
    startTimerHz(30);
}

AlignMalignProcessor::~AlignMalignProcessor() {
    stopTimer();
    am::SessionBus::get().leave(slot_); // clears onIR under the bus lock, so no callback can arrive after this
}

bool AlignMalignProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void AlignMalignProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    prepared_ = false;
    sampleRate_ = sampleRate;
    const int maxBlock = juce::jmax(16, samplesPerBlock);
    const juce::dsp::ProcessSpec mono{ sampleRate, (juce::uint32)maxBlock, 1 };
    for (auto* c : { &convMid_, &convSide_, &convMidM_, &convSideM_ }) {
        c->prepare(mono);
        c->reset();
    }
    // neutral impulse (a centred delta) until the session hands us a real one: keeps latency constant
    juce::AudioBuffer<float> delta(1, am::kIRLen);
    delta.clear();
    delta.setSample(0, am::kIRLen / 2, 1.f);
    for (auto* c : { &convMid_, &convSide_, &convMidM_, &convSideM_ }) {
        juce::AudioBuffer<float> copy(delta);
        c->loadImpulseResponse(std::move(copy), sampleRate, juce::dsp::Convolution::Stereo::no,
                               juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    }
    dryDelay_.prepare({ sampleRate, (juce::uint32)maxBlock, 2 });
    dryDelay_.setMaximumDelayInSamples(am::kIRLen);
    dryDelay_.setDelay((float)(am::kIRLen / 2));
    dryDelay_.reset();
    for (auto* b : { &mid_, &side_, &midM_, &sideM_, &dryM_, &dryS_ }) b->setSize(1, maxBlock, false, true, false);
    monoScratch_.assign((size_t)maxBlock, 0.f);
    setLatencySamples(am::kIRLen / 2);
    am::SessionBus::get().setSampleRate(sampleRate);
    am::SessionBus::get().controlsChanged(); // rebuild IRs for this sample rate
    prepared_ = true;
}

void AlignMalignProcessor::loadIRs(std::shared_ptr<const am::IRSet> set) {
    // Called on the session worker thread. Convolution::loadImpulseResponse is safe to call from any thread;
    // the swap happens on JUCE's background thread with a short crossfade.
    auto load = [this](juce::dsp::Convolution& c, const std::vector<float>& h) {
        if (h.empty()) return;
        juce::AudioBuffer<float> b(1, (int)h.size());
        b.copyFrom(0, 0, h.data(), (int)h.size());
        c.loadImpulseResponse(std::move(b), sampleRate_, juce::dsp::Convolution::Stereo::no,
                              juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    };
    if (!prepared_.load()) return; // prepareToPlay asks the bus for a fresh set anyway
    load(convMid_, set->mid);
    load(convSide_, set->side);
    load(convMidM_, set->midMirror);
    load(convSideM_, set->sideMirror);
}

void AlignMalignProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int total = buffer.getNumSamples(), nch = buffer.getNumChannels();
    if (!prepared_ || nch == 0 || total == 0) return;

    int64_t pos = -1;
    bool playing = false;
    double ppq = 0.0, bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto p = ph->getPosition()) {
            playing = p->getIsPlaying();
            if (auto t = p->getTimeInSamples()) pos = *t;
            if (auto b = p->getBpm()) bpm = juce::jmax(1.0, *b);
            if (auto q = p->getPpqPosition()) ppq = *q;
            else if (pos >= 0) ppq = (double)pos / sampleRate_ * bpm / 60.0;
        }
    const double ppqPerSample = bpm / 60.0 / sampleRate_;

    const bool learning = am::SessionBus::get().learning();
    const float width = pWidth_->get() * 2.f;     // 0.5 -> unchanged
    const float mix = pMix_->get();
    const float motion = pMotion_->get();
    const bool ghost = pFlavour_->getIndex() == 2;
    const bool ab = pAB_->get(), delta = pDelta_->get();
    const int maxBlock = mid_.getNumSamples();
    float inPeak = 0.f, outSq = 0.f;

    for (int off = 0; off < total; off += maxBlock) {
        const int n = juce::jmin(maxBlock, total - off);
        float* L = buffer.getWritePointer(0, off);
        float* R = nch > 1 ? buffer.getWritePointer(1, off) : nullptr;
        float* m = mid_.getWritePointer(0);
        float* s = side_.getWritePointer(0);
        for (int i = 0; i < n; ++i) {
            const float l = L[i], r = R ? R[i] : L[i];
            m[i] = 0.5f * (l + r);
            s[i] = 0.5f * (l - r);
            inPeak = juce::jmax(inPeak, std::abs(l), std::abs(r));
        }

        slot_->write(m, n, pos >= 0 ? pos + off : -1, playing, learning);

        // dry path, delayed to line up with the FIR output
        float* dm = dryM_.getWritePointer(0);
        float* ds = dryS_.getWritePointer(0);
        for (int i = 0; i < n; ++i) {
            dryDelay_.pushSample(0, m[i]); dm[i] = dryDelay_.popSample(0);
            dryDelay_.pushSample(1, s[i]); ds[i] = dryDelay_.popSample(1);
        }

        const bool useMotion = motion > 0.001f;
        if (useMotion) {
            midM_.copyFrom(0, 0, mid_, 0, 0, n);
            sideM_.copyFrom(0, 0, side_, 0, 0, n);
        }
        auto run = [n](juce::dsp::Convolution& c, juce::AudioBuffer<float>& b) {
            juce::dsp::AudioBlock<float> blk(b);
            auto sub = blk.getSubBlock(0, (size_t)n);
            c.process(juce::dsp::ProcessContextReplacing<float>(sub));
        };
        run(convMid_, mid_);
        run(convSide_, side_);
        if (useMotion) {
            run(convMidM_, midM_);
            run(convSideM_, sideM_);
            // Motion: breathe between this setting and its mirror (align <-> malign) every 2 bars
            const double ppqEnd = ppq + n * ppqPerSample;
            const float* mm = midM_.getReadPointer(0);
            const float* sm = sideM_.getReadPointer(0);
            for (int i = 0; i < n; ++i) {
                const double q = ppq + (ppqEnd - ppq) * i / n;
                const float lam = motion * (float)(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * q / 8.0));
                m[i] += lam * (mm[i] - m[i]);
                s[i] += lam * (sm[i] - s[i]);
            }
        }

        for (int i = 0; i < n; ++i) {
            float wm = m[i], ws = s[i];
            if (ghost) { wm -= dm[i]; ws -= ds[i]; }       // only the interaction between tracks
            ws *= width;
            float om, os;
            if (ab) { om = dm[i]; os = ds[i]; }              // B = the original, latency-matched
            else if (delta) { om = wm - dm[i]; os = ws - ds[i]; }
            else { om = dm[i] + mix * (wm - dm[i]); os = ds[i] + mix * (ws - ds[i]); }
            L[i] = om + os;
            if (R) R[i] = om - os;
            outSq += om * om;
            const int w = scopeWrite_.load(std::memory_order_relaxed);
            scopeRing_[(size_t)w] = om;
            scopeWrite_.store((w + 1) % kScope, std::memory_order_relaxed);
        }
        ppq += n * ppqPerSample;
    }
    inLevel_.store(inPeak);
    outLevel_.store(std::sqrt(outSq / (float)total));
}

// ---------------------------------------------------------------- message thread
void AlignMalignProcessor::timerCallback() {
    auto& bus = am::SessionBus::get();
    // 1) controls -> session bus (it rebuilds this instance's IRs and the shared match EQ)
    const float knob = pAlign_->get() / 100.f;   // -100 (align) .. +100 (malign) -> -1 .. +1
    const float match = pMatch_->get();
    const int flav = pFlavour_->getIndex();
    const bool motionOn = pMotion_->get() > 0.001f;
    if (std::abs(knob - lastKnob_) > 1e-4f || std::abs(match - lastMatch_) > 1e-4f || flav != lastFlavour_ || motionOn != lastMotionOn_) {
        slot_->controls.knob = knob;
        slot_->controls.match = match;
        slot_->controls.flavour = flav;
        slot_->controls.motionOn = motionOn;
        lastKnob_ = knob; lastMatch_ = match; lastFlavour_ = flav; lastMotionOn_ = motionOn;
        bus.controlsChanged();
    }

    // 2) session linking of the macro controls
    auto& hub = LinkHub::get();
    if (scope_ != "session") { lastPublished_.clear(); hub.since(UINT64_MAX, linkSeqSeen_); adopted_ = true; return; }
    std::vector<LinkHub::Ev> evs;
    if (!adopted_) {
        adopted_ = true;
        if (stateRestored_) hub.since(UINT64_MAX, linkSeqSeen_);   // a saved instance keeps its own values
        else evs = hub.latestValues(linkSeqSeen_);                 // a new instance adopts the session's values
    } else {
        evs = hub.since(linkSeqSeen_, linkSeqSeen_);
    }
    applyingLinked_ = true;
    for (auto& e : evs) {
        if (e.source == slot_->id()) continue;
        if (auto* p = apvts.getParameter(e.id)) {
            if (std::abs(p->getValue() - e.v) > 1e-5f) p->setValueNotifyingHost(e.v);
            lastPublished_[e.id] = e.v;
        }
    }
    applyingLinked_ = false;
    for (const char* id : kLinked) {
        auto* p = apvts.getParameter(id);
        const float v = p->getValue();
        auto it = lastPublished_.find(id);
        if (it == lastPublished_.end()) { lastPublished_[id] = v; continue; }   // first sight: don't broadcast defaults
        if (std::abs(it->second - v) > 1e-5f) { it->second = v; hub.publish(slot_->id(), id, v); }
    }
}

void AlignMalignProcessor::setParamFromUI(const juce::String& id, const juce::var& value) {
    if (id == "scope") { scope_ = value.toString(); return; }
    auto* p = apvts.getParameter(id);
    if (p == nullptr) return;
    float norm = 0.f;
    if (id == "flavour") norm = p->convertTo0to1((float)juce::jmax(0, flavourNames().indexOf(value.toString())));
    else if (id == "ab" || id == "delta") norm = (bool)value ? 1.f : 0.f;
    else if (id == "alignment") norm = p->convertTo0to1((float)(double)value);
    else norm = juce::jlimit(0.f, 1.f, (float)(double)value);
    if (std::abs(p->getValue() - norm) > 1e-6f) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(norm);
        p->endChangeGesture();
    }
}

void AlignMalignProcessor::toggleLearn() {
    auto& bus = am::SessionBus::get();
    bus.setLearning(!bus.learning());
}

juce::var AlignMalignProcessor::makeUIState() {
    auto* o = new juce::DynamicObject();
    o->setProperty("alignment", pAlign_->get());
    o->setProperty("match", pMatch_->get());
    o->setProperty("width", pWidth_->get());
    o->setProperty("motion", pMotion_->get());
    o->setProperty("mix", pMix_->get());
    o->setProperty("flavour", flavourNames()[pFlavour_->getIndex()]);
    o->setProperty("ab", pAB_->get());
    o->setProperty("delta", pDelta_->get());
    o->setProperty("scope", scope_);
    const auto st = am::SessionBus::get().status();
    o->setProperty("linked", st.linked);
    o->setProperty("learning", st.learning);
    o->setProperty("seconds", st.seconds);
    o->setProperty("solved", st.solved);
    o->setProperty("track", trackName_);
    juce::Array<juce::var> names;
    for (auto& n : am::SessionBus::get().trackNames()) names.add(juce::String(n));
    o->setProperty("names", names);
    o->setProperty("inLevel", inLevel_.load());
    o->setProperty("level", juce::jmin(1.f, outLevel_.load() * 3.5f));
    return juce::var(o);
}

void AlignMalignProcessor::updateTrackProperties(const TrackProperties& props) {
    if (props.name.has_value() && props.name->isNotEmpty()) trackName_ = *props.name;
    am::SessionBus::get().setName(slot_, trackName_.toStdString());
}

// ---------------------------------------------------------------- state
void AlignMalignProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts.copyState();
    state.setProperty("scope", scope_, nullptr);
    state.setProperty("track", trackName_, nullptr);
    am::Maps maps;
    std::vector<float> match;
    am::SessionBus::get().snapshot(slot_, maps, match);
    if (maps.K > 0 && !maps.alignStd.empty()) {
        juce::ValueTree mt("Maps");
        mt.setProperty("K", maps.K, nullptr);
        mt.setProperty("binHz", maps.binHz, nullptr);
        mt.setProperty("alignStd", floatsToBase64(maps.alignStd[0]), nullptr);
        mt.setProperty("malignStd", floatsToBase64(maps.malignStd[0]), nullptr);
        mt.setProperty("alignExt", floatsToBase64(maps.alignExt[0]), nullptr);
        mt.setProperty("malignExt", floatsToBase64(maps.malignExt[0]), nullptr);
        mt.setProperty("match", floatsToBase64(match), nullptr);
        state.appendChild(mt, nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void AlignMalignProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml(*xml);
    if (!state.isValid()) return;
    scope_ = state.getProperty("scope", "session").toString();
    trackName_ = state.getProperty("track", trackName_).toString();
    auto mt = state.getChildWithName("Maps");
    if (mt.isValid()) {
        am::Maps maps;
        maps.K = (int)mt.getProperty("K", 0);
        maps.binHz = (double)mt.getProperty("binHz", 0.0);
        maps.alignStd = { base64ToFloats(mt["alignStd"].toString()) };
        maps.malignStd = { base64ToFloats(mt["malignStd"].toString()) };
        maps.alignExt = { base64ToFloats(mt["alignExt"].toString()) };
        maps.malignExt = { base64ToFloats(mt["malignExt"].toString()) };
        auto match = base64ToFloats(mt["match"].toString());
        const bool ok = maps.K > 0 && (int)maps.alignStd[0].size() == maps.K && (int)maps.malignStd[0].size() == maps.K
                        && (int)maps.alignExt[0].size() == maps.K && (int)maps.malignExt[0].size() == maps.K;
        if (ok) am::SessionBus::get().restore(slot_, std::move(maps), std::move(match));
        state.removeChild(mt, nullptr);
    }
    apvts.replaceState(state);
    stateRestored_ = true;
    lastKnob_ = 99.f; // force a push of the restored controls
}

juce::AudioProcessorEditor* AlignMalignProcessor::createEditor() { return new AlignMalignEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AlignMalignProcessor(); }
