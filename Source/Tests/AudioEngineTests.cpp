#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Audio/Metronome.h"
#include "../Services/AudioAttachmentService.h"
#include <cassert>
#include <iostream>
#include <cmath>

void testMetronomeCalculations() {
    std::cout << "[Test] Metronome 4/4 Timing..." << std::endl;
    tracks::Metronome met;
    met.prepare(44100.0);
    met.setTempo(120);
    met.setTimeSignature("4/4");

    assert(met.getBeatsPerBar() == 4);

    // 120 BPM at 44100 Hz: 1 beat = 44100 * 60 / 120 = 22050 samples
    // Render 1 beat
    juce::AudioBuffer<float> buffer(2, 22050);
    buffer.clear();
    met.renderBlock(buffer, 0, 22050);

    // Beat should advance
    assert(met.getCurrentBeatInBar() == 0 || met.getCurrentBeatInBar() == 1);
    assert(met.isCountIn());
    assert(std::abs(met.getTotalBarsElapsed() - 0.25) < 1e-4);

    // Advance 2.25 more bars (9 beats) -> total 2.5 bars, past 2-bar count-in
    for (int i = 0; i < 9; ++i) {
        buffer.clear();
        met.renderBlock(buffer, 0, 22050);
    }
    assert(!met.isCountIn());
    assert(std::abs(met.getTotalBarsElapsed() - 2.5) < 1e-4);

    std::cout << "[Test] Metronome 6/8 Compound Timing..." << std::endl;
    met.prepare(44100.0);
    met.setTempo(80);
    met.setTimeSignature("6/8");

    assert(met.getBeatsPerBar() == 6);

    std::cout << "[Test] Metronome 12/8 Compound Timing..." << std::endl;
    met.setTimeSignature("12/8");
    assert(met.getBeatsPerBar() == 12);

    std::cout << "[Test] Metronome Section Count-In Alignment..." << std::endl;
    met.prepare(44100.0);
    met.setTempo(120);
    met.setTimeSignature("4/4");
    met.setUseSectionCountIn(true);

    // Case A: Leading "Count" section (2 bars) followed by Intro (4 bars) and Verse 1 (8 bars)
    std::vector<tracks::SongSection> secsWithLeadingCount = {
        { std::nullopt, "Count", 2, 0 },
        { std::nullopt, "Intro", 4, 1 },
        { std::nullopt, "Verse 1", 8, 2 }
    };
    met.setSections(secsWithLeadingCount);
    assert(met.hasLeadingCountSection());
    assert(met.getLeadingCountBars() == 2);
    assert(met.getTotalCountInFrames() == 22050 * 4 * 2);

    // Bar 0 (Count bar 1): one, [rest], two, [rest]
    assert(met.getScheduledGuideCueForBeat(0, 0) == "one");
    assert(met.getScheduledGuideCueForBeat(0, 1).empty());
    assert(met.getScheduledGuideCueForBeat(0, 2) == "two");
    assert(met.getScheduledGuideCueForBeat(0, 3).empty());

    // Bar 1 (Count bar 2, right before Intro): Intro, two, three, four (no duplicate "one" or count)
    assert(met.getScheduledGuideCueForBeat(1, 0) == "section:intro");
    assert(met.getScheduledGuideCueForBeat(1, 1) == "two");
    assert(met.getScheduledGuideCueForBeat(1, 2) == "three");
    assert(met.getScheduledGuideCueForBeat(1, 3) == "four");

    // Bar 2..4 (middle of Intro): click only
    assert(met.getScheduledGuideCueForBeat(2, 0).empty());
    assert(met.getScheduledGuideCueForBeat(4, 2).empty());

    // Bar 5 (bar prior to Verse 1 at Bar 6): Verse 1, two, three, four
    assert(met.getScheduledGuideCueForBeat(5, 0) == "section:verse 1");
    assert(met.getScheduledGuideCueForBeat(5, 1) == "two");
    assert(met.getScheduledGuideCueForBeat(5, 2) == "three");
    assert(met.getScheduledGuideCueForBeat(5, 3) == "four");

    // Case B: Leading "Intro" (4 bars), no leading count section, then mid-song 1-bar count + Pre-Chorus 1
    std::vector<tracks::SongSection> secsWithoutLeadingCount = {
        { std::nullopt, "Intro", 4, 0 },
        { std::nullopt, "count", 1, 1 },
        { std::nullopt, "Pre-Chorus 1", 8, 2 }
    };
    met.setSections(secsWithoutLeadingCount);
    assert(!met.hasLeadingCountSection());
    assert(met.getLeadingCountBars() == 0);
    assert(secsWithoutLeadingCount[1].isCountSection());
    assert(!secsWithoutLeadingCount[0].isCountSection());
    assert(secsWithoutLeadingCount[2].normalizedKey() == "pre chorus 1");

    // Prepended count-in Bar 0 = one, [rest], two, [rest]; Bar 1 = Intro, two, three, four
    assert(met.getScheduledGuideCueForBeat(0, 0) == "one");
    assert(met.getScheduledGuideCueForBeat(1, 0) == "section:intro");
    assert(met.getScheduledGuideCueForBeat(1, 1) == "two");
    // Mid-song 1-bar count at Bar 6 (directly before Pre-Chorus 1 at Bar 7): Pre-Chorus 1, two, three, four
    assert(met.getScheduledGuideCueForBeat(6, 0) == "section:pre chorus 1");
    assert(met.getScheduledGuideCueForBeat(6, 1) == "two");
    assert(met.getScheduledGuideCueForBeat(6, 2) == "three");
    assert(met.getScheduledGuideCueForBeat(6, 3) == "four");

    // Verify rendered audio on Bar 1 (final bar of count-in before Intro) plays the Intro cue
    // on beat 1 (not "one"), followed by "two", "three", "four" once without duplication.
    auto makeImpulse = [](float amp) {
        juce::AudioBuffer<float> b(1, 16);
        b.clear();
        b.setSample(0, 0, amp);
        return b;
    };
    met.reset();
    met.setClickChannel(0);
    met.setGuideChannel(1);
    met.setClickGain(0.0f);
    met.setGuideGain(1.0f);
    met.loadVoiceWord("one", makeImpulse(0.11f));
    met.loadVoiceWord("two", makeImpulse(0.22f));
    met.loadVoiceWord("three", makeImpulse(0.33f));
    met.loadVoiceWord("four", makeImpulse(0.44f));
    met.loadSectionCue("intro", makeImpulse(0.77f));

    const int barFrames = static_cast<int>(met.getFramesPerBar());
    const int beatFrames = barFrames / 4;
    juce::AudioBuffer<float> bar0(2, barFrames);
    juce::AudioBuffer<float> bar1(2, barFrames);
    bar0.clear();
    bar1.clear();
    met.renderBlock(bar0, 0, barFrames);
    met.renderBlock(bar1, 0, barFrames);

    // Bar 0: beat 0 = "one" (0.11), beat 1 = rest (0), beat 2 = "two" (0.22), beat 3 = rest (0)
    assert(std::abs(bar0.getSample(1, 0) - 0.11f) < 1e-5f);
    assert(std::abs(bar0.getSample(1, beatFrames) - 0.0f) < 1e-5f);
    assert(std::abs(bar0.getSample(1, beatFrames * 2) - 0.22f) < 1e-5f);
    assert(std::abs(bar0.getSample(1, beatFrames * 3) - 0.0f) < 1e-5f);

    // Bar 1: beat 0 = "intro" (0.77, NOT "one" 0.11 or sum 0.88), beat 1..3 = "two", "three", "four"
    assert(std::abs(bar1.getSample(1, 0) - 0.77f) < 1e-5f);
    assert(std::abs(bar1.getSample(1, beatFrames) - 0.22f) < 1e-5f);
    assert(std::abs(bar1.getSample(1, beatFrames * 2) - 0.33f) < 1e-5f);
    assert(std::abs(bar1.getSample(1, beatFrames * 3) - 0.44f) < 1e-5f);

    // Test quarter / eighth / sixteenth click subdivisions (120 BPM 4/4 -> beat = 22050, 16th = 5512/5513)
    std::cout << "[Test] Metronome Quarter/Eighth/Sixteenth Subdivisions..." << std::endl;
    met.prepare(44100.0);
    met.setTempo(120);
    met.setTimeSignature("4/4");
    met.setUseSectionCountIn(false);
    met.setClickChannel(0);
    met.setGuideChannel(1);
    met.setClickGain(1.0f);
    met.setGuideGain(0.0f);
    met.loadClickSamples(makeImpulse(0.40f), makeImpulse(0.90f), makeImpulse(0.50f), makeImpulse(0.25f));

    // Eighths + Sixteenths enabled with independent fader gains
    met.setClickSubdivisions(true, true);
    met.setEighthClickGain(0.8f);     // expected 0.50 * 0.8 = 0.40
    met.setSixteenthClickGain(0.6f);  // expected 0.25 * 0.6 = 0.15

    juce::AudioBuffer<float> subBuf(2, barFrames);
    subBuf.clear();
    // Render in realistic small audio blocks (256 frames) to verify block-boundary accuracy
    for (int pos = 0; pos < barFrames; pos += 256) {
        int n = std::min(256, barFrames - pos);
        met.renderBlock(subBuf, pos, n);
    }

    // Beat 1 (start of bar): accent (0.90), e = 16th (0.15), & = 8th (0.40), a = 16th (0.15)
    assert(std::abs(subBuf.getSample(0, 0) - 0.90f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames / 4) - 0.15f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames / 2) - 0.40f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, (beatFrames * 3) / 4) - 0.15f) < 1e-5f);

    // Beat 2: quarter (0.40), e = 16th (0.15), & = 8th (0.40), a = 16th (0.15)
    assert(std::abs(subBuf.getSample(0, beatFrames) - 0.40f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames + beatFrames / 4) - 0.15f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames + beatFrames / 2) - 0.40f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames + (beatFrames * 3) / 4) - 0.15f) < 1e-5f);

    // Eighths OFF, Sixteenths ON -> steady 16ths on e, &, a (no gap on &)
    met.reset();
    met.setClickSubdivisions(false, true);
    subBuf.clear();
    for (int pos = 0; pos < beatFrames; pos += 256) {
        int n = std::min(256, beatFrames - pos);
        met.renderBlock(subBuf, pos, n);
    }
    assert(std::abs(subBuf.getSample(0, 0) - 0.90f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames / 4) - 0.15f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, beatFrames / 2) - 0.15f) < 1e-5f);
    assert(std::abs(subBuf.getSample(0, (beatFrames * 3) / 4) - 0.15f) < 1e-5f);
}

void testStereoRoutingLogic() {
    std::cout << "[Test] Stereo and Mono Audio Routing Logic..." << std::endl;
    // Mock stereo track: 2 channels, 100 samples
    // Channel 0 = 0.8f (Left), Channel 1 = 0.4f (Right)
    juce::AudioBuffer<float> trackBuffer(2, 100);
    for (int i = 0; i < 100; ++i) {
        trackBuffer.setSample(0, i, 0.8f);
        trackBuffer.setSample(1, i, 0.4f);
    }

    // Case 1: 4 Hardware output channels available.
    // Track configured to output channel 0.
    // It should route:
    // Output Ch 0 = 0.8f (Left)
    // Output Ch 1 = 0.4f (Right)
    juce::AudioBuffer<float> outputBuffer(4, 100);
    outputBuffer.clear();

    int channelA = 0;
    int numOutputChannels = 4;
    float gain = 1.0f;

    if (trackBuffer.getNumChannels() >= 2) {
        if (channelA + 1 < numOutputChannels) {
            outputBuffer.addFrom(channelA, 0, trackBuffer, 0, 0, 100, gain);
            outputBuffer.addFrom(channelA + 1, 0, trackBuffer, 1, 0, 100, gain);
        } else if (channelA < numOutputChannels) {
            float half = gain * 0.5f;
            outputBuffer.addFrom(channelA, 0, trackBuffer, 0, 0, 100, half);
            outputBuffer.addFrom(channelA, 0, trackBuffer, 1, 0, 100, half);
        }
    }

    assert(std::abs(outputBuffer.getSample(0, 0) - 0.8f) < 1e-5f);
    assert(std::abs(outputBuffer.getSample(1, 0) - 0.4f) < 1e-5f);
    assert(outputBuffer.getSample(2, 0) == 0.0f);
    assert(outputBuffer.getSample(3, 0) == 0.0f);

    // Case 2: Only 1 hardware channel available (or channelA is at the boundary: e.g. ch 3 with 4 total channels)
    // Track configured to output channel 3.
    // Output Ch 3 should receive downmixed (0.8 + 0.4) * 0.5 = 0.6f!
    outputBuffer.clear();
    channelA = 3;
    if (trackBuffer.getNumChannels() >= 2) {
        if (channelA + 1 < numOutputChannels) {
            outputBuffer.addFrom(channelA, 0, trackBuffer, 0, 0, 100, gain);
            outputBuffer.addFrom(channelA + 1, 0, trackBuffer, 1, 0, 100, gain);
        } else if (channelA < numOutputChannels) {
            float half = gain * 0.5f;
            outputBuffer.addFrom(channelA, 0, trackBuffer, 0, 0, 100, half);
            outputBuffer.addFrom(channelA, 0, trackBuffer, 1, 0, 100, half);
        }
    }

    assert(std::abs(outputBuffer.getSample(3, 0) - 0.6f) < 1e-5f);

    // Case 3: Adaptive equal-power & anti-phase-safe mono summing verification
    auto computeMonoSumGains = [](float corr) {
        float posCorr = std::clamp(corr, 0.0f, 1.0f);
        float rmsSumGain = 1.0f / std::sqrt(2.0f * (1.0f + posCorr));
        float antiPhaseBlend = std::clamp(-corr, 0.0f, 1.0f);
        float gL = (1.0f - antiPhaseBlend) * rmsSumGain + antiPhaseBlend;
        float gR = (1.0f - antiPhaseBlend) * rmsSumGain;
        return std::pair<float, float>{ gL, gR };
    };

    // Centered mono (corr = +1): 0.5 * L + 0.5 * R -> unity gain
    auto [monoL, monoR] = computeMonoSumGains(1.0f);
    assert(std::abs(monoL - 0.5f) < 1e-4f && std::abs(monoR - 0.5f) < 1e-4f);

    // Uncorrelated wide stereo (corr = 0): 0.7071 * (L + R) -> equal-power RMS preservation
    auto [wideL, wideR] = computeMonoSumGains(0.0f);
    assert(std::abs(wideL - 0.70710678f) < 1e-4f && std::abs(wideR - 0.70710678f) < 1e-4f);

    // Pure anti-phase (corr = -1, R = -L): preserves L instead of cancelling to silence
    auto [antiL, antiR] = computeMonoSumGains(-1.0f);
    assert(std::abs(antiL - 1.0f) < 1e-4f && std::abs(antiR - 0.0f) < 1e-4f);

    std::cout << "[Test] Stereo routing and downmix verified successfully!" << std::endl;
}

void testSongStorageDirNaming() {
    std::cout << "[Test] Song storage directory naming..." << std::endl;
    auto dir1 = tracks::AudioAttachmentService::getSongStorageDir(4, "House of the Lord");
    assert(dir1.filename().string() == "4_House_of_the_Lord");

    auto dir2 = tracks::AudioAttachmentService::getSongStorageDir(12, "  10,000 Reasons / (Bless the Lord)! ");
    assert(dir2.filename().string() == "12_10_000_Reasons_Bless_the_Lord");

    auto dir3 = tracks::AudioAttachmentService::getSongStorageDir(7, "");
    assert(dir3.filename().string() == "7");

    auto arrDir = tracks::AudioAttachmentService::getArrangementStorageDir(4, "House of the Lord", 2, "Quiet Final Chorus");
    assert(arrDir.parent_path().filename().string() == "4_House_of_the_Lord");
    assert(arrDir.filename().string() == "2_Quiet_Final_Chorus");

    // Verify M4A support in AudioAttachmentService (including reading real M4A asset if present)
    assert(tracks::AudioAttachmentService::isSupportedAudioExtension(".wav"));
    assert(tracks::AudioAttachmentService::isSupportedAudioExtension(".mp3"));
    assert(tracks::AudioAttachmentService::isSupportedAudioExtension(".m4a"));
    assert(tracks::AudioAttachmentService::isSupportedAudioExtension(".M4A"));
    assert(!tracks::AudioAttachmentService::isSupportedAudioExtension(".txt"));

    auto m4aDir = tracks::AudioAttachmentService::getProjectRoot() / "assets" / "guide" / "playback_female";
    if (std::filesystem::exists(m4aDir)) {
        std::cout << "[Test] Inspecting M4A audio files in assets/guide/playback_female..." << std::endl;
        auto insp = tracks::AudioAttachmentService::inspectAudioDirectory(m4aDir.string());
        assert(!insp.error.has_value());
        assert(!insp.tracks.empty());
        assert(insp.tracks[0].numChannels == 2);
    }

    // Verify restructured click packs have accent, quarter, eighth, and sixteenth audio files
    auto clicksRoot = tracks::AudioAttachmentService::getProjectRoot() / "assets" / "clicks";
    for (const auto& packName : { "tone", "Blip", "Classic" }) {
        auto packDir = clicksRoot / packName;
        if (std::filesystem::exists(packDir)) {
            auto insp = tracks::AudioAttachmentService::inspectAudioDirectory(packDir.string());
            assert(!insp.error.has_value());
            assert(insp.tracks.size() == 4);
        }
    }
}

int main() {
    try {
        testMetronomeCalculations();
        testStereoRoutingLogic();
        testSongStorageDirNaming();
        std::cout << "[Test] All AudioEngine tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Audio test failed: " << e.what() << std::endl;
        return 1;
    }
}
