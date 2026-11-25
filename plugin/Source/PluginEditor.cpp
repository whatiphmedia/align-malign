#include "PluginEditor.h"
#include "BinaryData.h"

namespace {
std::vector<std::byte> toBytes(const char* data, int size) {
    std::vector<std::byte> v((size_t)size);
    std::memcpy(v.data(), data, (size_t)size);
    return v;
}
} // namespace

AlignMalignEditor::AlignMalignEditor(AlignMalignProcessor& p) : juce::AudioProcessorEditor(&p), proc_(p) {
    for (int i = 0; i < 2048; ++i)
        window_[(size_t)i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float)i / 2048.f);

    auto opts = juce::WebBrowserComponent::Options{}
                    .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
                    .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}
                                                .withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory)
                                                                        .getChildFile("AlignMalignWebView2"))
                                                .withBackgroundColour(juce::Colour(0xff050505)))
                    .withNativeIntegrationEnabled()
                    .withKeepPageLoadedWhenBrowserIsHidden()
                    .withResourceProvider([this](const juce::String& url) { return getResource(url); })
                    .withEventListener("param", [this](juce::var v) {
                        if (auto* o = v.getDynamicObject())
                            proc_.setParamFromUI(o->getProperty("id").toString(), o->getProperty("v"));
                    })
                    .withEventListener("action", [this](juce::var v) {
                        const auto name = v.getDynamicObject() ? v.getDynamicObject()->getProperty("name").toString() : v.toString();
                        if (name == "learn") proc_.toggleLearn();
                        else if (name == "solve") am::SessionBus::get().requestSolve();
                    });

    web_ = std::make_unique<juce::WebBrowserComponent>(opts);
    addAndMakeVisible(*web_);
    web_->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable(true, true);
    setResizeLimits(900, 700, 2400, 1800);
    setSize(1240, 940);
    startTimerHz(30);
}

AlignMalignEditor::~AlignMalignEditor() { stopTimer(); }

void AlignMalignEditor::resized() { if (web_) web_->setBounds(getLocalBounds()); }

std::optional<juce::WebBrowserComponent::Resource> AlignMalignEditor::getResource(const juce::String& url) {
    const auto path = url.fromFirstOccurrenceOf(juce::WebBrowserComponent::getResourceProviderRoot(), false, false)
                          .upToFirstOccurrenceOf("?", false, false);
    if (path.isEmpty() || path == "/" || path == "index.html" || path == "/index.html")
        return juce::WebBrowserComponent::Resource{ toBytes(BinaryData::index_html, BinaryData::index_htmlSize), "text/html" };
    return std::nullopt;
}

// 9 log-spaced bands like the UI's own analyser (0..1, ~ -100..-30 dB), from the output scope ring.
juce::Array<juce::var> AlignMalignEditor::spectrumBands() {
    const int w = proc_.scopeWrite_.load(std::memory_order_relaxed);
    for (int i = 0; i < 2048; ++i)
        fftBuf_[(size_t)i] = proc_.scopeRing_[(size_t)((w + i) % AlignMalignProcessor::kScope)] * window_[(size_t)i];
    std::fill(fftBuf_.begin() + 2048, fftBuf_.end(), 0.f);
    fft_.performFrequencyOnlyForwardTransform(fftBuf_.data());
    const int nb = 1024, NL = 9;
    juce::Array<juce::var> out;
    for (int l = 0; l < NL; ++l) {
        const int lo = (int)std::floor(2.0 * std::pow(nb / 2.0, (double)l / NL));
        const int hi = juce::jmax(lo + 1, (int)std::floor(2.0 * std::pow(nb / 2.0, (double)(l + 1) / NL)));
        double s = 0;
        for (int j = lo; j < hi && j < nb; ++j) {
            const double mag = fftBuf_[(size_t)j] / 512.0;
            const double db = 20.0 * std::log10(mag + 1e-9);
            s += juce::jlimit(0.0, 1.0, (db + 100.0) / 70.0);
        }
        out.add(std::pow(s / (hi - lo), 1.4));
    }
    return out;
}

void AlignMalignEditor::timerCallback() {
    if (!web_ || !isShowing()) return;
    auto state = proc_.makeUIState();
    if (auto* o = state.getDynamicObject()) o->setProperty("bands", spectrumBands());
    web_->emitEventIfBrowserIsVisible("state", state);
}
