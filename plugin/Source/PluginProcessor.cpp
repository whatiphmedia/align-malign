#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"
#include <deque>

namespace {

// Session linking for the macro controls: with scope = Session, moving a control on one instance moves it on
// every linked instance (so a 12-track session is driven from any one plugin window).
struct LinkHub {
    struct Ev { uint64_t seq; int source; juce::String id; float v; juce::String group; bool session; };
    static LinkHub& get() { static LinkHub h; return h; }

    // session: the sender is in Session scope (everyone in Session scope follows). group: the sender's group
    // (its members follow, whatever their scope).
    void publish(int source, const juce::String& id, float v, const juce::String& group, bool session) {
        const std::lock_guard<std::mutex> g(m);
        evs.push_back({ ++seq, source, id, v, group, session });
        if (session) latest[id] = evs.back();
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

// Receives the Auto Malign Live control surface's reports (UDP, localhost): for every Auto Malign instance found in
// the set, the latency of everything in front of it on its track. One listener per process, alive while any
// instance exists (started by the first, stopped by the last; never torn down during DLL unload).
class LiveLink : private juce::Thread {
public:
    struct Info { int latency = 0, order = -1; juce::String track, kind; double when = 0; };
    static LiveLink& get() { static LiveLink l; return l; }

    void retain() { const std::lock_guard<std::mutex> g(m_); if (refs_++ == 0) startThread(); }
    void release() {
        bool stop = false;
        { const std::lock_guard<std::mutex> g(m_); stop = (--refs_ == 0); }
        if (stop) { signalThreadShouldExit(); stopThread(1500); }
    }
    bool lookup(int id, Info& out) const {
        const std::lock_guard<std::mutex> g(m_);
        auto it = map_.find(id);
        if (it == map_.end() || now() - it->second.when > 3000.0) return false;
        out = it->second;
        return true;
    }
    bool seen() const { const std::lock_guard<std::mutex> g(m_); return now() - lastPacket_ < 3000.0; }

private:
    LiveLink() : juce::Thread("Auto Malign Live link") {}
    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    void run() override {
        std::vector<char> buf(65536);
        while (!threadShouldExit()) {
            juce::DatagramSocket sock(false);
            if (!sock.bindToPort(47863, "127.0.0.1")) { wait(2000); continue; }   // port busy: try again later
            while (!threadShouldExit()) {
                if (sock.waitUntilReady(true, 200) != 1) continue;
                const int n = sock.read(buf.data(), (int)buf.size() - 1, false);
                if (n > 0) parse(juce::String::fromUTF8(buf.data(), n));
            }
        }
    }
    void parse(const juce::String& text) {
        const auto v = juce::JSON::parse(text);
        if (!v.isObject() || (int)v["am"] != 1) return;
        const std::lock_guard<std::mutex> g(m_);
        lastPacket_ = now();
        if (auto* arr = v["tracks"].getArray())
            for (auto& t : *arr) {
                Info i; i.latency = juce::jmax(0, (int)t["latency"]); i.track = t["track"].toString(); i.when = lastPacket_;
                i.kind = t["kind"].toString();
                i.order = t.hasProperty("order") ? (int)t["order"] : -1;
                map_[(int)t["id"]] = i;
            }
    }

    mutable std::mutex m_;
    int refs_ = 0;
    std::map<int, Info> map_;
    double lastPacket_ = -1e9;
};

const char* kLinked[] = { "alignment", "match", "width", "motion", "mix", "flavour", "ab", "delta", "famt", "pmono", "srot" };
// what a group shares even outside Session scope: everything that shapes the phase correction
bool groupLinked(const juce::String& id) { return id == "alignment" || id == "flavour" || id == "motion" || id == "match" || id == "famt" || id == "pmono" || id == "srot"; }

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
    return { "FUSE", "ERASE", "GHOST", "MALODICS", "UNMASK", "ENVELOP", "CURVE", "DISPERSE" };
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
    // Pro page: flavour depth and the side signal
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "famt", 1 }, "Flavour amount", NormalisableRange<float>(0.f, 1.f), 1.f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "pmono", 1 }, "Phase mono below", NormalisableRange<float>(0.f, 400.f, 1.f), 0.f,
                                                AudioParameterFloatAttributes().withLabel("Hz")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "srot", 1 }, "Side rotation", NormalisableRange<float>(-180.f, 180.f, 1.f), 0.f,
                                                AudioParameterFloatAttributes().withLabel("deg")));
    // Malodic notes (Refine flavour > MALODICS): MIDI-played pitches made of stereo phase
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{ "ppmode", 1 }, "Malodics mode", StringArray{ "Huggins", "Ghost" }, 0));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "ppassist", 1 }, "Malodics assist", NormalisableRange<float>(0.f, 1.f), 0.25f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "pppurity", 1 }, "Malodics purity", NormalisableRange<float>(0.f, 1.f), 0.7f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "ppcarrier", 1 }, "Malodics carrier", NormalisableRange<float>(0.f, 1.f), 0.35f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "ppstrength", 1 }, "Malodics strength", NormalisableRange<float>(0.f, 1.f), 0.5f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{ "ppfold", 1 }, "Malodics sweet-spot folding", true));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{ "ppfill", 1 }, "Malodics carrier fills gaps only", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "pptame", 1 }, "Malodics tame", NormalisableRange<float>(0.f, 1.f), 0.6f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{ "ppconst", 1 }, "Malodics constant", false));
    l.add(std::make_unique<AudioParameterInt>(ParameterID{ "pppartials", 1 }, "Malodics harmonics", 1, 6, 6));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "ppattack", 1 }, "Malodics attack", NormalisableRange<float>(1.f, 2000.f, 0.f, 0.4f), 40.f,
                                                AudioParameterFloatAttributes().withLabel("ms")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "pprelease", 1 }, "Malodics release", NormalisableRange<float>(5.f, 8000.f, 0.f, 0.4f), 700.f,
                                                AudioParameterFloatAttributes().withLabel("ms")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "ppglide", 1 }, "Malodics glide", NormalisableRange<float>(0.f, 1000.f, 0.f, 0.4f), 0.f,
                                                AudioParameterFloatAttributes().withLabel("ms")));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "ppbend", 1 }, "Malodics bend range", NormalisableRange<float>(0.f, 24.f, 1.f), 2.f,
                                                AudioParameterFloatAttributes().withLabel("st")));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{ "ppsource", 1 }, "Malodics source",
                                                 StringArray{ "This track + noise", "Sidechain input", "Session track" }, 0));
    // Latency in front of this instance (synths/plugins before it on the track): read from Live, or typed in.
    l.add(std::make_unique<AudioParameterBool>(ParameterID{ "latauto", 1 }, "Latency before: from Live", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "latms", 1 }, "Latency before (manual)",
                                                NormalisableRange<float>(0.f, 250.f, 0.01f), 0.f,
                                                AudioParameterFloatAttributes().withLabel("ms")));
    // Identifies this instance to the Live control surface (set automatically; not something to automate).
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{ "linkid", 1 }, "Link ID", NormalisableRange<float>(0.f, 1.f), 0.f));
    return l;
}

AlignMalignProcessor::AlignMalignProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), false)),   // Phantom notes source
      apvts(*this, nullptr, "AlignMalign", createLayout()) {
    pAlign_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("alignment"));
    pMatch_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("match"));
    pWidth_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("width"));
    pMotion_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("motion"));
    pMix_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("mix"));
    pFlavour_ = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("flavour"));
    pAB_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("ab"));
    pDelta_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("delta"));
    pFAmt_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("famt"));
    pPMono_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pmono"));
    pSRot_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("srot"));
    jassert(pFAmt_ && pPMono_ && pSRot_);
    pPpMode_ = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("ppmode"));
    pPpSource_ = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("ppsource"));
    pPpAssist_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ppassist"));
    pPpCarrier_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ppcarrier"));
    pPpPurity_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pppurity"));
    jassert(pPpPurity_);
    pPpStrength_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ppstrength"));
    pPpAttack_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ppattack"));
    pPpRelease_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pprelease"));
    pPpGlide_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ppglide"));
    pPpBend_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("ppbend"));
    pPpFold_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("ppfold"));
    pPpFill_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("ppfill"));
    jassert(pPpFill_);
    pPpTame_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pptame"));
    pPpConst_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("ppconst"));
    jassert(pPpTame_ && pPpConst_);
    pPpPartials_ = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter("pppartials"));
    jassert(pPpMode_ && pPpSource_ && pPpAssist_ && pPpCarrier_ && pPpStrength_ && pPpAttack_ && pPpRelease_ && pPpGlide_ && pPpBend_ && pPpFold_ && pPpPartials_);
    for (auto& n : ppSounding_) n.store(-1);
    pLatAuto_ = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("latauto"));
    pLatMs_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("latms"));
    pLinkId_ = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("linkid"));
    jassert(pAlign_ && pMatch_ && pWidth_ && pMotion_ && pMix_ && pFlavour_ && pAB_ && pDelta_ && pLatAuto_ && pLatMs_ && pLinkId_);
    linkId_ = juce::Random::getSystemRandom().nextInt(juce::Range<int>(1, 99999));
    LiveLink::get().retain();

    for (auto* c : { &convMid_, &convSide_, &convMidM_, &convSideM_ }) {
        *c = std::make_unique<am::PartitionedConvolver>(am::kIRLen);
        std::vector<float> delta((size_t)am::kIRLen, 0.f);
        delta[(size_t)am::kIRLen / 2] = 1.f;                     // neutral until the session sends a map
        (*c)->setKernel((*c)->makeKernel(delta));
    }
    slot_ = am::SessionBus::get().join(0.0);
    ownRing_ = am::AudioShare::get().publish(slot_->id());
    slot_->onIR = [this](std::shared_ptr<const am::IRSet> s) { loadIRs(std::move(s)); };
    setLatencySamples(latency());
    startTimerHz(30);
}

AlignMalignProcessor::~AlignMalignProcessor() {
    stopTimer();
    LiveLink::get().release();
    am::AudioShare::get().withdraw(slot_->id());
    am::SessionBus::get().leave(slot_); // clears onIR under the bus lock, so no callback can arrive after this
}

bool AlignMalignProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    if (layouts.inputBuses.size() > 1) {                 // the optional sidechain: off, mono or stereo
        const auto sc = layouts.getChannelSet(true, 1);
        if (!sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void AlignMalignProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    prepared_ = false;
    sampleRate_ = sampleRate;
    const int maxBlock = juce::jmax(16, samplesPerBlock);
    // Convolvers keep their kernels across prepare; only their input history is cleared.
    for (auto* c : { convMid_.get(), convSide_.get(), convMidM_.get(), convSideM_.get() }) c->reset();
    dryDelay_.prepare({ sampleRate, (juce::uint32)maxBlock, 2 });
    dryDelay_.setMaximumDelayInSamples(am::kIRLen * 2);
    dryDelay_.setDelay((float)latency());
    dryDelay_.reset();
    for (auto* b : { &mid_, &side_, &midM_, &sideM_, &dryM_, &dryS_ }) b->setSize(1, maxBlock, false, true, false);
    monoScratch_.assign((size_t)maxBlock, 0.f);
    srcBuf_.setSize(2, maxBlock, false, true, false);
    scDelayed_.setSize(2, maxBlock, false, true, false);
    scDelay_.prepare({ sampleRate, (juce::uint32)maxBlock, 2 });
    scDelay_.setMaximumDelayInSamples(am::kIRLen * 2);
    scDelay_.setDelay((float)latency());
    scDelay_.reset();
    phantom_.prepare(sampleRate);
    updatePhantomParams();
    setLatencySamples(latency());
    am::SessionBus::get().setSampleRate(sampleRate);
    am::SessionBus::get().controlsChanged(); // rebuild IRs for this sample rate (sent only if they changed)
    prepared_ = true;
}

void AlignMalignProcessor::loadIRs(std::shared_ptr<const am::IRSet> set) {
    // Called on the session worker thread. The kernel is transformed here; the audio thread picks it up at its next
    // block and crossfades into it with full input history (no dropout).
    auto load = [](am::PartitionedConvolver& c, const std::vector<float>& h) {
        if (!h.empty()) c.setKernel(c.makeKernel(h));
    };
    load(*convMid_, set->mid);
    load(*convSide_, set->side);
    load(*convMidM_, set->midMirror.empty() ? set->mid : set->midMirror);
    load(*convSideM_, set->sideMirror.empty() ? set->side : set->sideMirror);
}

void AlignMalignProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int total = buffer.getNumSamples(), nch = juce::jmin(buffer.getNumChannels(), getMainBusNumOutputChannels());
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
    float inPeak = 0.f, outSq = 0.f, outPeak = 0.f;

    // Phantom notes: MIDI is scheduled one plugin latency later, so after the host's delay compensation each note
    // lands exactly where it sits on the timeline.
    const bool ppOn = pFlavour_->getIndex() == 3;          // MALODICS
    if (ppOn != phantomOn_.load(std::memory_order_relaxed)) { if (!ppOn) phantom_.allOff(); phantomOn_.store(ppOn); }
    const int64_t clock0 = phantom_.clock();
    for (const auto meta : midi) {
        if (!ppOn) break;
        const auto& mm = meta.getMessage();
        const int64_t at = clock0 + meta.samplePosition + latency();
        if (mm.isNoteOn()) phantom_.noteOn(mm.getNoteNumber(), mm.getFloatVelocity(), at);
        else if (mm.isNoteOff()) phantom_.noteOff(mm.getNoteNumber(), at);
        else if (mm.isPitchWheel()) phantom_.pitchBend((mm.getPitchWheelValue() - 8192) / 8192.f, at);
        else if (mm.isSustainPedalOn()) phantom_.sustainPedal(true, at);
        else if (mm.isSustainPedalOff()) phantom_.sustainPedal(false, at);
        else if (mm.isAllNotesOff() || mm.isAllSoundOff()) phantom_.allOff();
    }
    const int ppSource = pPpSource_->getIndex();
    auto scBus = getBusCount(true) > 1 ? getBusBuffer(buffer, true, 1) : juce::AudioBuffer<float>();
    const bool scOn = getBusCount(true) > 1 && getBus(true, 1)->isEnabled() && scBus.getNumChannels() > 0;

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
        if (playing && pos >= 0) ownRing_->write(L, R, n, pos + off);   // for other tracks' Phantom notes

        // Phantom notes source, aligned to this chunk's output (= input one latency ago)
        const float *srcL = nullptr, *srcR = nullptr;
        if (scOn) {                                          // the sidechain is always delayed, so switching is seamless
            const float* a = scBus.getReadPointer(0, off);
            const float* b = scBus.getReadPointer(juce::jmin(1, scBus.getNumChannels() - 1), off);
            float* da = scDelayed_.getWritePointer(0);
            float* db = scDelayed_.getWritePointer(1);
            for (int i = 0; i < n; ++i) { scDelay_.pushSample(0, a[i]); da[i] = scDelay_.popSample(0); scDelay_.pushSample(1, b[i]); db[i] = scDelay_.popSample(1); }
            if (ppSource == 1) { srcL = da; srcR = db; }
        }
        if (ppSource == 2 && playing && pos >= 0)
            if (auto* ring = srcRingRaw_.load(std::memory_order_acquire))
                if (ring->read(pos + off - latency(), n, srcBuf_.getWritePointer(0), srcBuf_.getWritePointer(1))) {
                    srcL = srcBuf_.getReadPointer(0); srcR = srcBuf_.getReadPointer(1);
                }

        // dry path, delayed to line up with the FIR output
        float* dm = dryM_.getWritePointer(0);
        float* ds = dryS_.getWritePointer(0);
        for (int i = 0; i < n; ++i) {
            dryDelay_.pushSample(0, m[i]); dm[i] = dryDelay_.popSample(0);
            dryDelay_.pushSample(1, s[i]); ds[i] = dryDelay_.popSample(1);
        }

        // The mirror convolvers always run so their history is warm when Motion is turned up.
        const bool useMotion = motion > 0.001f;
        midM_.copyFrom(0, 0, mid_, 0, 0, n);
        sideM_.copyFrom(0, 0, side_, 0, 0, n);
        convMid_->process(m, n);
        convSide_->process(s, n);
        convMidM_->process(midM_.getWritePointer(0), n);
        convSideM_->process(sideM_.getWritePointer(0), n);
        if (useMotion) {
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
            outPeak = juce::jmax(outPeak, std::abs(L[i]), R ? std::abs(R[i]) : 0.f);
            const int w = scopeWrite_.load(std::memory_order_relaxed);
            scopeRing_[(size_t)w] = om;
            scopeWrite_.store((w + 1) % kScope, std::memory_order_relaxed);
        }
        // Phantom notes on the final output (keeps its clock running even when idle)
        phantom_.process(L, R, srcL, srcR, n);
        if (phantom_.busy())
            for (int i = 0; i < n; ++i) outPeak = juce::jmax(outPeak, std::abs(L[i]), R ? std::abs(R[i]) : 0.f);
        ppq += n * ppqPerSample;
    }
    inLevel_.store(inPeak);
    for (auto* h : { &inPeakHold_, &outPeakHold_ }) {          // hold the highest peak until the window reads it
        const float v = h == &inPeakHold_ ? inPeak : outPeak;
        float cur = h->load(std::memory_order_relaxed);
        while (v > cur && !h->compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }
    outLevel_.store(std::sqrt(outSq / (float)total));
}

// ---------------------------------------------------------------- message thread
void AlignMalignProcessor::updatePhantomParams() {
    am::PhantomParams p;
    p.huggins = pPpMode_->getIndex() == 0;
    p.assist = pPpAssist_->get();
    p.carrier = pPpCarrier_->get();
    p.purity = pPpPurity_->get();
    p.strength = pPpStrength_->get();
    p.fold = pPpFold_->get();
    p.fillGaps = pPpFill_->get();
    p.tame = pPpTame_->get();
    p.constant = pPpConst_->get();
    p.partials = pPpPartials_->get();
    p.attackMs = pPpAttack_->get();
    p.releaseMs = pPpRelease_->get();
    p.glideMs = pPpGlide_->get();
    p.bendRange = pPpBend_->get();
    phantom_.setParams(p);   // plain values, read once per sample by the audio thread (worst case: one stale sample)
}

void AlignMalignProcessor::timerCallback() {
    auto& bus = am::SessionBus::get();
    updatePhantomParams();
    {   // the session track used as the Phantom source (kept alive one extra tick, so the audio thread never sees it freed)
        const int id = ppSourceId_.load();
        auto want = id >= 0 && id != slot_->id() ? am::AudioShare::get().find(id) : nullptr;
        if (want != srcRing_) { srcRingPrev_ = srcRing_; srcRing_ = want; srcRingRaw_.store(want.get(), std::memory_order_release); }
        else srcRingPrev_.reset();
        if (pendingSourceName_.isNotEmpty())               // a restored source: find it once its track has joined
            for (auto& t : bus.tracks())
                if (t.id != slot_->id() && juce::String(t.name) == pendingSourceName_) { ppSourceId_.store(t.id); pendingSourceName_.clear(); break; }
        const auto notes = phantom_.sounding();
        for (size_t i = 0; i < ppSounding_.size(); ++i) ppSounding_[i].store(i < notes.size() ? notes[i] : -1);
    }
    // 0) publish this instance's Link ID (a duplicated track copies the old value, so it is re-asserted) and work out
    //    how late this track's audio arrives: from Live's control surface, else the manual value, else zero.
    const float wantId = (float)linkId_ / 100000.f;
    if (std::abs(pLinkId_->get() - wantId) > 1e-7f) pLinkId_->setValueNotifyingHost(pLinkId_->convertTo0to1(wantId));
    {
        LiveLink::Info info;
        int lat = 0;
        const bool fromLive = LiveLink::get().lookup(linkId_, info);
        if (fromLive) {                                     // Live knows this track's name and where it sits
            bus.setPlace(slot_, info.kind.toStdString(), info.order);
            if (info.track.isNotEmpty() && info.track != trackName_) { trackName_ = info.track; bus.setName(slot_, trackName_.toStdString()); }
        }
        if (pLatAuto_->get() && fromLive) { lat = info.latency; latSource_ = "live"; latTrack_ = info.track; }
        else if (pLatMs_->get() > 0.f) { lat = juce::roundToInt(pLatMs_->get() * 0.001 * sampleRate_); latSource_ = "manual"; }
        else latSource_ = "none";
        latUsedMs_ = lat * 1000.0 / sampleRate_;
        slot_->setUpstreamLatency(lat);
    }
    // 1) controls -> session bus (it rebuilds this instance's IRs and the shared match EQ)
    const float knob = pAlign_->get() / 100.f;   // -100 (align) .. +100 (malign) -> -1 .. +1
    const float match = pMatch_->get();
    const int flav = pFlavour_->getIndex();
    const bool motionOn = pMotion_->get() > 0.001f;
    const float famt = pFAmt_->get(), pmono = pPMono_->get(), srot = pSRot_->get();
    if (std::abs(knob - lastKnob_) > 1e-4f || std::abs(match - lastMatch_) > 1e-4f || flav != lastFlavour_ || motionOn != lastMotionOn_
        || famt != lastFAmt_ || pmono != lastPMono_ || srot != lastSRot_) {
        slot_->controls.flavourAmt = famt;
        slot_->controls.monoBelowHz = pmono;
        slot_->controls.sideRotDeg = srot;
        lastFAmt_ = famt; lastPMono_ = pmono; lastSRot_ = srot;
        slot_->controls.knob = knob;
        slot_->controls.match = match;
        slot_->controls.flavour = flav;
        slot_->controls.motionOn = motionOn;
        lastKnob_ = knob; lastMatch_ = match; lastFlavour_ = flav; lastMotionOn_ = motionOn;
        bus.controlsChanged();
    }

    // 2) linking of the macro controls: Session scope links every instance in Session scope; a group links its
    //    members whatever their scope (so kick in and kick out always turn together).
    auto& hub = LinkHub::get();
    const std::string g = bus.group(slot_);
    const juce::String grp = (g.empty() || g == "off") ? juce::String() : juce::String(g);
    const bool session = scope_ == "session";
    if (!session && grp.isEmpty()) { lastPublished_.clear(); hub.since(UINT64_MAX, linkSeqSeen_); adopted_ = true; return; }
    std::vector<LinkHub::Ev> evs;
    if (!adopted_) {
        adopted_ = true;
        if (stateRestored_ || !session) hub.since(UINT64_MAX, linkSeqSeen_);   // a saved instance keeps its own values
        else evs = hub.latestValues(linkSeqSeen_);                             // a new instance adopts the session's values
    } else {
        evs = hub.since(linkSeqSeen_, linkSeqSeen_);
    }
    applyingLinked_ = true;
    for (auto& e : evs) {
        if (e.source == slot_->id()) continue;
        const bool follow = (session && e.session) || (grp.isNotEmpty() && e.group == grp && groupLinked(e.id));
        if (!follow) continue;
        if (auto* p = apvts.getParameter(e.id)) {
            if (std::abs(p->getValue() - e.v) > 1e-5f) p->setValueNotifyingHost(e.v);
            lastPublished_[e.id] = e.v;
        }
    }
    applyingLinked_ = false;
    for (const char* id : kLinked) {
        if (!session && !groupLinked(id)) continue;
        auto* p = apvts.getParameter(id);
        const float v = p->getValue();
        auto it = lastPublished_.find(id);
        if (it == lastPublished_.end()) { lastPublished_[id] = v; continue; }   // first sight: don't broadcast defaults
        if (std::abs(it->second - v) > 1e-5f) { it->second = v; hub.publish(slot_->id(), id, v, grp, session); }
    }
}

void AlignMalignProcessor::setParamFromUI(const juce::String& id, const juce::var& value) {
    if (id == "scope") { scope_ = value.toString() == "session" ? "session" : "track"; return; }
    // session-wide analysis settings (one Learn, one solve): stored on the bus, every window shows them
    if (id == "loHz" || id == "hiHz" || id == "smooth" || id == "stiff" || id == "guard" || id == "anchor") {
        auto& bus = am::SessionBus::get();
        auto st = bus.settings();
        if (id == "loHz") st.loHz = (double)value;
        else if (id == "hiHz") st.hiHz = (double)value;
        else if (id == "smooth") st.smoothIdx = (int)value;
        else if (id == "stiff") st.stiffness = (double)value;
        else if (id == "guard") st.guard = (bool)value;
        else st.anchorId = (int)value;
        bus.setSettings(st);
        return;
    }
    if (id == "ppSourceTrack") { ppSourceId_.store((int)value); return; }
    if (id == "latAuto") { pLatAuto_->setValueNotifyingHost((bool)value ? 1.f : 0.f); return; }
    if (id == "latMs") { pLatMs_->setValueNotifyingHost(pLatMs_->convertTo0to1((float)(double)value)); return; }
    auto* p = apvts.getParameter(id);
    if (p == nullptr) return;
    float norm = 0.f;
    if (id == "flavour") norm = p->convertTo0to1((float)juce::jmax(0, flavourNames().indexOf(value.toString())));
    else if (id == "ab" || id == "delta") norm = (bool)value ? 1.f : 0.f;
    else if (auto* pf = dynamic_cast<juce::AudioParameterFloat*>(p)) norm = pf->convertTo0to1(juce::jlimit(pf->range.start, pf->range.end, (float)(double)value));
    else if (auto* pc = dynamic_cast<juce::AudioParameterChoice*>(p)) norm = pc->convertTo0to1((float)juce::jlimit(0, pc->choices.size() - 1, (int)value));
    else if (auto* pi = dynamic_cast<juce::AudioParameterInt*>(p)) norm = pi->convertTo0to1((float)juce::jlimit(pi->getRange().getStart(), pi->getRange().getEnd(), (int)value));
    else if (dynamic_cast<juce::AudioParameterBool*>(p)) norm = (bool)value ? 1.f : 0.f;
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
    juce::Array<juce::var> names, tracks;
    auto list = am::SessionBus::get().tracks();
    std::stable_sort(list.begin(), list.end(), [](const am::TrackInfo& a, const am::TrackInfo& b) {
        return (a.order < 0 ? 1 << 30 : a.order) < (b.order < 0 ? 1 << 30 : b.order);   // Live's track order when known
    });
    for (auto& t : list) {
        names.add(juce::String(t.name));
        auto* r = new juce::DynamicObject();
        r->setProperty("id", t.id);
        r->setProperty("name", juce::String(t.name));
        r->setProperty("group", juce::String(t.group));
        r->setProperty("kind", juce::String(t.kind));
        r->setProperty("excluded", t.excluded);
        r->setProperty("self", t.id == slot_->id());
        tracks.add(juce::var(r));
    }
    o->setProperty("names", names);
    o->setProperty("tracks", tracks);
    {   // Phantom notes
        auto* pp = new juce::DynamicObject();
        pp->setProperty("mode", pPpMode_->getIndex());
        pp->setProperty("assist", pPpAssist_->get());
        pp->setProperty("carrier", pPpCarrier_->get());
        pp->setProperty("purity", pPpPurity_->get());
        pp->setProperty("strength", pPpStrength_->get());
        pp->setProperty("fold", pPpFold_->get());
        pp->setProperty("fill", pPpFill_->get());
        pp->setProperty("tame", pPpTame_->get());
        pp->setProperty("constant", pPpConst_->get());
        pp->setProperty("partials", pPpPartials_->get());
        pp->setProperty("attack", pPpAttack_->get());
        pp->setProperty("release", pPpRelease_->get());
        pp->setProperty("glide", pPpGlide_->get());
        pp->setProperty("bend", pPpBend_->get());
        pp->setProperty("source", pPpSource_->getIndex());
        pp->setProperty("sourceTrack", ppSourceId_.load());
        pp->setProperty("sidechain", getBusCount(true) > 1 && getBus(true, 1)->isEnabled());
        juce::Array<juce::var> held;
        for (auto& n : ppSounding_) if (n.load() >= 0) held.add(n.load());
        pp->setProperty("notes", held);
        o->setProperty("phantom", juce::var(pp));
    }
    o->setProperty("famt", pFAmt_->get());
    o->setProperty("pmono", pPMono_->get());
    o->setProperty("srot", pSRot_->get());
    {
        const auto st = am::SessionBus::get().settings();
        auto* a = new juce::DynamicObject();
        a->setProperty("loHz", st.loHz);
        a->setProperty("hiHz", st.hiHz);
        a->setProperty("smooth", st.smoothIdx);
        a->setProperty("stiff", st.stiffness);
        a->setProperty("guard", st.guard);
        a->setProperty("anchor", st.anchorId);
        o->setProperty("analysis", juce::var(a));
    }
    // meters: peaks since the last update, in dBFS
    o->setProperty("inDb", juce::Decibels::gainToDecibels(inPeakHold_.exchange(0.f), -100.f));
    o->setProperty("outDb", juce::Decibels::gainToDecibels(outPeakHold_.exchange(0.f), -100.f));
    o->setProperty("latAuto", pLatAuto_->get());
    o->setProperty("latMs", pLatMs_->get());
    o->setProperty("latSource", latSource_);
    o->setProperty("latUsedMs", latUsedMs_);
    o->setProperty("latTrack", latTrack_);
    o->setProperty("liveSeen", LiveLink::get().seen());
    o->setProperty("msg", msg_);
    o->setProperty("msgOk", msgOk_);
    o->setProperty("inLevel", inLevel_.load());
    o->setProperty("level", juce::jmin(1.f, outLevel_.load() * 3.5f));
    return juce::var(o);
}

void AlignMalignProcessor::setGroup(int slotId, const juce::String& group) {
    const juce::String g = (group == "A" || group == "B" || group == "C" || group == "D" || group == "off") ? group : juce::String();
    am::SessionBus::get().setGroup(slotId, g.toStdString());
}

void AlignMalignProcessor::updateTrackProperties(const TrackProperties& props) {
    if (props.name.isNotEmpty()) trackName_ = props.name;
    am::SessionBus::get().setName(slot_, trackName_.toStdString());
}

// ---------------------------------------------------------------- state
void AlignMalignProcessor::getStateInformation(juce::MemoryBlock& dest) {
    auto state = apvts.copyState();
    state.setProperty("scope", scope_, nullptr);
    state.setProperty("track", trackName_, nullptr);
    state.setProperty("group", juce::String(am::SessionBus::get().group(slot_)), nullptr);
    {   // the Phantom source track is remembered by name (slot ids change every time the set loads)
        juce::String srcName;
        for (auto& t : am::SessionBus::get().tracks()) if (t.id == ppSourceId_.load()) srcName = juce::String(t.name);
        state.setProperty("ppSourceName", srcName, nullptr);
    }
    {   // the session's analysis settings travel with every copy; the anchor is remembered by the copy that is it
        const auto st = am::SessionBus::get().settings();
        state.setProperty("aLo", st.loHz, nullptr);
        state.setProperty("aHi", st.hiHz, nullptr);
        state.setProperty("aSmooth", st.smoothIdx, nullptr);
        state.setProperty("aStiff", st.stiffness, nullptr);
        state.setProperty("aGuard", st.guard, nullptr);
        state.setProperty("aIsAnchor", st.anchorId == slot_->id(), nullptr);
    }
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
    scope_ = state.getProperty("scope", "session").toString() == "session" ? "session" : "track";   // old Bus/Master = this track
    trackName_ = state.getProperty("track", trackName_).toString();
    am::SessionBus::get().setName(slot_, trackName_.toStdString());
    setGroup(slot_->id(), state.getProperty("group", "").toString());
    pendingSourceName_ = state.getProperty("ppSourceName", "").toString();
    if (state.hasProperty("aLo")) {
        auto& bus = am::SessionBus::get();
        auto st = bus.settings();
        st.loHz = (double)state.getProperty("aLo");
        st.hiHz = (double)state.getProperty("aHi");
        st.smoothIdx = (int)state.getProperty("aSmooth");
        st.stiffness = (double)state.getProperty("aStiff");
        st.guard = (bool)state.getProperty("aGuard");
        if ((bool)state.getProperty("aIsAnchor", false)) st.anchorId = slot_->id();
        bus.setSettings(st);
    }
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

void AlignMalignProcessor::installLiveScript() {
    // Copies the bundled control surface into Live's User Library (Live 11+ loads user Remote Scripts from there).
    const char* data = nullptr;
    int size = 0;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        if (juce::String(BinaryData::originalFilenames[i]) == "__init__.py")
            data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size);
    const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                         .getChildFile("Ableton").getChildFile("User Library").getChildFile("Remote Scripts").getChildFile("AutoMalign");
    const bool ok = data != nullptr && dir.createDirectory() && dir.getChildFile("__init__.py").replaceWithData(data, (size_t)size);
    msgOk_ = ok;
    msg_ = ok ? "Live script installed. Restart Live, then pick AutoMalign under Settings > Link, Tempo & MIDI > Control Surface."
              : "Couldn't write to " + dir.getFullPathName() + ". Copy the 'Ableton Live control surface' folder from the download there.";
}

juce::AudioProcessorEditor* AlignMalignProcessor::createEditor() { return new AlignMalignEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AlignMalignProcessor(); }
