#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Audio/Metronome.h"
#include "../Audio/StreamingAudioTrack.h"
#include "../Audio/MidiTempoImporter.h"
#include "../Audio/PitchShiftUtils.h"
#include "../Audio/SectionJumpUtils.h"
#include "../Models/AvailableSections.h"
#include "../Database/DatabaseService.h"
#include "../Services/AudioAttachmentService.h"
#include "../Services/LibraryBackupService.h"
#include <signalsmith-stretch.h>
#include <cassert>
#include <fstream>
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

        // Verify AvailableSections covers "Count" and every playback_female section cue file
        assert(tracks::isAvailableSectionName("Count"));
        assert(tracks::isAvailableSectionName("count"));
        assert(!tracks::isAvailableSectionName("Nonexistent Custom Section"));
        int sectionCueFiles = 0;
        for (const auto& entry : std::filesystem::directory_iterator(m4aDir)) {
            if (!entry.is_regular_file()) continue;
            std::string stem = entry.path().stem().string();
            if (stem.find("section-") != std::string::npos) {
                auto dash = stem.rfind('-');
                std::string secName = (dash != std::string::npos) ? stem.substr(dash + 1) : stem;
                assert(tracks::isAvailableSectionName(secName));
                ++sectionCueFiles;
            }
        }
        assert(static_cast<int>(tracks::getAvailableSections().size()) == sectionCueFiles + 1);

        // Verify filterAvailableSections prefix ranking & multi-token filtering
        assert(tracks::findCanonicalSectionName("pre-chorus 2") == "Pre Chorus 2");
        assert(tracks::findCanonicalSectionName("count") == "Count");
        auto chFiltered = tracks::filterAvailableSections("ch");
        assert(!chFiltered.empty() && chFiltered[0] == "Chorus");
        auto vamp1Filtered = tracks::filterAvailableSections("vamp 1");
        assert(!vamp1Filtered.empty());
        assert(tracks::filterAvailableSections("zzzzzz").empty());
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

void testPitchShiftLogic() {
    std::cout << "[Test] Pitch shift semitone math, Drum/FX exclusions, and Signalsmith Stretch alignment..." << std::endl;

    // 1. Key-to-semitone shortest interval wrapping
    assert(tracks::computeSemitoneShift("C", "C") == 0);
    assert(tracks::computeSemitoneShift("C", "D") == 2);
    assert(tracks::computeSemitoneShift("C", "B") == -1);
    assert(tracks::computeSemitoneShift("Bb", "C") == 2);
    assert(tracks::computeSemitoneShift("C#", "Db") == 0);
    assert(tracks::computeSemitoneShift("G", "Bb") == 3);
    assert(tracks::computeSemitoneShift("Am", "Cm") == 3);
    assert(tracks::computeSemitoneShift("F#", "E") == -2);

    // 1b. Major vs Minor key detection
    assert(!tracks::isMinorKey("C"));
    assert(!tracks::isMinorKey("Bb"));
    assert(!tracks::isMinorKey("F#"));
    assert(!tracks::isMinorKey("C maj"));
    assert(tracks::isMinorKey("Am"));
    assert(tracks::isMinorKey("Bbm"));
    assert(tracks::isMinorKey("F#m"));
    assert(tracks::isMinorKey("Ebm"));
    assert(tracks::isMinorKey("G minor"));

    // 2. Drum & FX channel exclusion checks
    assert(tracks::shouldExcludeFromPitchShift("Drums"));
    assert(tracks::shouldExcludeFromPitchShift("Live Drums"));
    assert(tracks::shouldExcludeFromPitchShift("Drum Loop"));
    assert(tracks::shouldExcludeFromPitchShift("Kick"));
    assert(tracks::shouldExcludeFromPitchShift("Snare"));
    assert(tracks::shouldExcludeFromPitchShift("Hi-Hat"));
    assert(tracks::shouldExcludeFromPitchShift("Overheads"));
    assert(tracks::shouldExcludeFromPitchShift("OH L"));
    assert(tracks::shouldExcludeFromPitchShift("Percussion"));
    assert(tracks::shouldExcludeFromPitchShift("Shaker"));
    assert(tracks::shouldExcludeFromPitchShift("Tambourine"));
    assert(tracks::shouldExcludeFromPitchShift("Claps"));
    assert(tracks::shouldExcludeFromPitchShift("FX"));
    assert(tracks::shouldExcludeFromPitchShift("FX 1"));
    assert(tracks::shouldExcludeFromPitchShift("FX1"));
    assert(tracks::shouldExcludeFromPitchShift("SFX"));
    assert(tracks::shouldExcludeFromPitchShift("Synth FX"));
    assert(tracks::shouldExcludeFromPitchShift("Vocal Effects"));
    assert(tracks::shouldExcludeFromPitchShift("Riser"));
    assert(tracks::shouldExcludeFromPitchShift("Click"));
    assert(tracks::shouldExcludeFromPitchShift("Guide"));

    // Harmonic / melodic stems MUST NOT be excluded
    assert(!tracks::shouldExcludeFromPitchShift("Piano"));
    assert(!tracks::shouldExcludeFromPitchShift("Keys"));
    assert(!tracks::shouldExcludeFromPitchShift("Acoustic Guitar"));
    assert(!tracks::shouldExcludeFromPitchShift("EG 1"));
    assert(!tracks::shouldExcludeFromPitchShift("Bass"));
    assert(!tracks::shouldExcludeFromPitchShift("Sub Bass"));
    assert(!tracks::shouldExcludeFromPitchShift("Synth"));
    assert(!tracks::shouldExcludeFromPitchShift("Pad"));
    assert(!tracks::shouldExcludeFromPitchShift("Organ"));
    assert(!tracks::shouldExcludeFromPitchShift("Strings"));
    assert(!tracks::shouldExcludeFromPitchShift("BGV"));

    // 3. Signalsmith Stretch latency-compensated alignment & pitch shift verification
    constexpr float kSampleRate = 44100.0f;
    constexpr int kTotalSamples = 22050; // 0.5s
    constexpr int kImpulsePos = 10000;

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(1, kSampleRate);
    stretch.setTransposeSemitones(0.0f);

    const int totalLat = stretch.inputLatency() + stretch.outputLatency();
    std::vector<float> input(kTotalSamples + totalLat + 512, 0.0f);
    input[kImpulsePos] = 1.0f;

    // Prime with [0 .. totalLat - 1]
    std::vector<float> primeOut(static_cast<size_t>(totalLat), 0.0f);
    float* pIn[1] = { input.data() };
    float* pOut[1] = { primeOut.data() };
    stretch.process(pIn, totalLat, pOut, totalLat);

    // Process in 512-sample blocks feeding from [pos + totalLat]
    std::vector<float> output(kTotalSamples, 0.0f);
    for (int pos = 0; pos < kTotalSamples; pos += 512) {
        int n = std::min(512, kTotalSamples - pos);
        float* inPtr[1] = { input.data() + pos + totalLat };
        float* outPtr[1] = { output.data() + pos };
        stretch.process(inPtr, n, outPtr, n);
    }

    int peakIdx = 0;
    float peakVal = 0.0f;
    for (int i = 0; i < kTotalSamples; ++i) {
        if (std::abs(output[i]) > peakVal) {
            peakVal = std::abs(output[i]);
            peakIdx = i;
        }
    }
    // Verify impulse stays sample-aligned with drums/click (zero ~50ms lag)
    assert(peakVal > 0.4f);
    assert(std::abs(peakIdx - kImpulsePos) <= 128);

    // Verify +12 semitones doubles a 440 Hz sine wave to ~880 Hz (via zero-crossing count)
    stretch.reset();
    stretch.setTransposeSemitones(12.0f);
    std::vector<float> sineIn(kTotalSamples + totalLat + 512, 0.0f);
    for (size_t i = 0; i < sineIn.size(); ++i) {
        sineIn[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / kSampleRate);
    }
    pIn[0] = sineIn.data();
    stretch.process(pIn, totalLat, pOut, totalLat);
    std::vector<float> sineOut(kTotalSamples, 0.0f);
    for (int pos = 0; pos < kTotalSamples; pos += 512) {
        int n = std::min(512, kTotalSamples - pos);
        float* inPtr[1] = { sineIn.data() + pos + totalLat };
        float* outPtr[1] = { sineOut.data() + pos };
        stretch.process(inPtr, n, outPtr, n);
    }
    int crossings = 0;
    for (int i = 4000; i < 18000; ++i) {
        if (sineOut[i - 1] <= 0.0f && sineOut[i] > 0.0f) ++crossings;
    }
    double measuredHz = static_cast<double>(crossings) * kSampleRate / 14000.0;
    assert(std::abs(measuredHz - 880.0) < 35.0);

    // 4. Tempo stretch UI +/-10 BPM limits & wide DSP speed ratio support
    std::cout << "[Test] Tempo Stretch Limits, Metronome Continuity, and Time-Stretch Alignment..." << std::endl;
    {
        auto [lo120, hi120] = tracks::getAllowedTrackTempoRange(120);
        assert(lo120 == 110 && hi120 == 130);
        assert(tracks::clampTrackTempoForUi(145, 120) == 130);
        assert(tracks::clampTrackTempoForUi(95, 120) == 110);
        assert(tracks::clampTrackTempoForUi(124, 120) == 124);

        auto [lo25, hi25] = tracks::getAllowedTrackTempoRange(25);
        assert(lo25 == 20 && hi25 == 35);
        auto [lo296, hi296] = tracks::getAllowedTrackTempoRange(296);
        assert(lo296 == 286 && hi296 == 300);

        // Underlying DSP engine ratio supports far beyond +/-10 BPM (0.25x to 4.0x)
        assert(std::abs(tracks::computeTempoSpeedRatio(120, 180) - 1.5) < 1e-6);
        assert(std::abs(tracks::computeTempoSpeedRatio(120, 60) - 0.5) < 1e-6);
        assert(std::abs(tracks::computeTempoSpeedRatio(120, 240) - 2.0) < 1e-6);
    }

    // 5. Metronome live tempo change preserves exact bar position (zero jump)
    {
        tracks::Metronome m;
        m.prepare(44100.0);
        m.setTempo(120);
        m.setTimeSignature("4/4");
        const int64_t fpb120 = m.getFramesPerBar();
        assert(m.getFramesPerBarForTempo(120) == fpb120);

        // Advance to bar 4.5
        juce::AudioBuffer<float> tmp(2, static_cast<int>(fpb120 / 2));
        for (int i = 0; i < 9; ++i) {
            tmp.clear();
            m.renderBlock(tmp, 0, tmp.getNumSamples());
        }
        assert(std::abs(m.getTotalBarsElapsed() - 4.5) < 1e-4);

        // Change tempo live to 128 BPM -> bar position must remain 4.5!
        m.setTempo(128);
        assert(std::abs(m.getTotalBarsElapsed() - 4.5) < 1e-3);
        assert(m.getFramesPerBarForTempo(120) == fpb120);
        assert(m.getFramesPerBar() < fpb120);
    }

    // 6. Signalsmith Stretch time-stretch impulse alignment at 1.25x faster & 0.80x slower (beyond +/-10 BPM!)
    auto verifyTimeStretchRate = [](double exactRate, float transposeSemitones) {
        constexpr float kSr = 44100.0f;
        constexpr int kOutTotal = 24000;
        constexpr int kImpulseOutTarget = 12000;
        const int kImpulseSrcPos = static_cast<int>(std::llround(kImpulseOutTarget * exactRate));
        const int kSrcLen = static_cast<int>(std::ceil((kOutTotal + 8000) * exactRate)) + 16384;

        std::vector<float> src(static_cast<size_t>(kSrcLen), 0.0f);
        src[static_cast<size_t>(kImpulseSrcPos)] = 1.0f;

        signalsmith::stretch::SignalsmithStretch<float> st;
        st.presetDefault(1, kSr, true /* splitComputation */);
        st.setTransposeSemitones(transposeSemitones);

        const int inLat = st.inputLatency();
        const int outLat = st.outputLatency();
        const int outLatSrc = static_cast<int>(std::round(exactRate * outLat));
        const int totalLatSrc = inLat + outLatSrc;
        const int primeIn = inLat + outLatSrc;
        const int primeOut = static_cast<int>(std::round(static_cast<double>(inLat) / exactRate)) + outLat;

        std::vector<float> pOutBuf(static_cast<size_t>(primeOut), 0.0f);
        float* pIn[1] = { src.data() };
        float* pOut[1] = { pOutBuf.data() };
        st.process(pIn, primeIn, pOut, primeOut);

        int64_t inCursor = primeIn;
        std::vector<float> out(static_cast<size_t>(kOutTotal), 0.0f);
        for (int outPos = 0; outPos < kOutTotal; outPos += 512) {
            int avail = std::min(512, kOutTotal - outPos);
            int64_t blockInEnd = static_cast<int64_t>(std::llround(static_cast<double>(outPos + avail) * exactRate + totalLatSrc));
            int inCount = std::max(0, static_cast<int>(blockInEnd - inCursor));
            float* inPtr[1] = { src.data() + inCursor };
            float* outPtr[1] = { out.data() + outPos };
            st.process(inPtr, inCount, outPtr, avail);
            inCursor += inCount;
        }

        int bestIdx = 0;
        float bestVal = 0.0f;
        for (int i = 0; i < kOutTotal; ++i) {
            if (std::abs(out[static_cast<size_t>(i)]) > bestVal) {
                bestVal = std::abs(out[static_cast<size_t>(i)]);
                bestIdx = i;
            }
        }
        assert(bestVal > 0.35f);
        assert(std::abs(bestIdx - kImpulseOutTarget) <= 128);
    };

    verifyTimeStretchRate(1.25, 0.0f);  // +25% faster (e.g. 120 -> 150 BPM)
    verifyTimeStretchRate(0.80, 0.0f);  // -20% slower (e.g. 120 -> 96 BPM)
    verifyTimeStretchRate(1.08, 3.0f);  // +8% faster + +3 semitones simultaneously
}

void testQueuedSectionSkipLogic() {
    std::cout << "[Test] Queued Section Skip Navigation & Metronome Jump..." << std::endl;

    // 1. Empty sections -> does nothing
    {
        auto emptySpans = tracks::buildSectionSpans({}, false);
        auto ctx = tracks::computeSectionContext(emptySpans, 2);
        assert(!tracks::canSkipSectionInDirection(0, ctx, -1, -1));
        assert(!tracks::canSkipSectionInDirection(0, ctx, -1, 1));
        assert(tracks::computeNextQueuedSection(0, ctx, -1, -1) == -1);
        assert(tracks::computeNextQueuedSection(0, ctx, -1, 1) == -1);
    }

    // 2. Multi-section song without leading "count" section (2-bar prepended count-in)
    std::vector<tracks::SongSection> sections = {
        { std::nullopt, "Intro", 4, 0 },    // bars [2, 6)
        { std::nullopt, "Verse 1", 8, 1 },  // bars [6, 14)
        { std::nullopt, "Chorus 1", 8, 2 }, // bars [14, 22)
        { std::nullopt, "Verse 2", 8, 3 },  // bars [22, 30)
        { std::nullopt, "Chorus 2", 8, 4 }, // bars [30, 38)
        { std::nullopt, "Outro", 4, 5 }     // bars [38, 42)
    };
    const int n = static_cast<int>(sections.size());
    auto spans = tracks::buildSectionSpans(sections, false);
    assert(spans.size() == 6);
    assert(spans[0].startBar == 2 && spans[0].endBar == 6);
    assert(spans[1].startBar == 6 && spans[1].endBar == 14);
    assert(spans[5].startBar == 38 && spans[5].endBar == 42);

    // Currently in Verse 1 (bar 8): naturalNext = Chorus 1 (2), boundaryBar = 14
    auto ctxV1 = tracks::computeSectionContext(spans, 8);
    assert(ctxV1.currentSectionIndex == 1);
    assert(ctxV1.naturalNextIndex == 2);
    assert(ctxV1.boundaryBar == 14);

    // Skip forward multiple sections: Chorus 1 (natural) -> Verse 2 (3) -> Chorus 2 (4) -> Outro (5) -> clamped at 5
    int q = -1;
    q = tracks::computeNextQueuedSection(n, ctxV1, q, 1);
    assert(q == 3); // Skips Chorus 1 -> Verse 2
    q = tracks::computeNextQueuedSection(n, ctxV1, q, 1);
    assert(q == 4); // Chorus 2
    q = tracks::computeNextQueuedSection(n, ctxV1, q, 1);
    assert(q == 5); // Outro
    assert(!tracks::canSkipSectionInDirection(n, ctxV1, q, 1));
    q = tracks::computeNextQueuedSection(n, ctxV1, q, 1);
    assert(q == 5); // Still Outro

    // Skip back through queued sections back to naturalNext (clears queue), then repeat current & go to Intro
    q = tracks::computeNextQueuedSection(n, ctxV1, q, -1);
    assert(q == 4); // Chorus 2
    q = tracks::computeNextQueuedSection(n, ctxV1, q, -1);
    assert(q == 3); // Verse 2
    q = tracks::computeNextQueuedSection(n, ctxV1, q, -1);
    assert(q == -1); // Back to naturalNext (Chorus 1) -> clears queue!
    q = tracks::computeNextQueuedSection(n, ctxV1, q, -1);
    assert(q == 1); // Repeat current section (Verse 1)!
    q = tracks::computeNextQueuedSection(n, ctxV1, q, -1);
    assert(q == 0); // Intro
    assert(!tracks::canSkipSectionInDirection(n, ctxV1, q, -1));
    q = tracks::computeNextQueuedSection(n, ctxV1, q, -1);
    assert(q == 0); // Clamped at Intro

    // In Outro (last section, bar 39): naturalNext = 6 (song end)
    auto ctxOutro = tracks::computeSectionContext(spans, 39);
    assert(ctxOutro.currentSectionIndex == 5);
    assert(ctxOutro.naturalNextIndex == 6);
    assert(ctxOutro.boundaryBar == 42);
    assert(!tracks::canSkipSectionInDirection(n, ctxOutro, -1, 1));
    assert(tracks::canSkipSectionInDirection(n, ctxOutro, -1, -1));
    int qOutro = tracks::computeNextQueuedSection(n, ctxOutro, -1, -1);
    assert(qOutro == 5); // Repeat Outro
    qOutro = tracks::computeNextQueuedSection(n, ctxOutro, qOutro, -1);
    assert(qOutro == 4); // Go back to Chorus 2
    qOutro = tracks::computeNextQueuedSection(n, ctxOutro, qOutro, 1);
    assert(qOutro == 5); // Back to Outro
    qOutro = tracks::computeNextQueuedSection(n, ctxOutro, qOutro, 1);
    assert(qOutro == -1); // Back to natural song end (clears queue)

    // In prepended 2-bar count-in (bar 0): naturalNext = 0 (Intro)
    auto ctxCount = tracks::computeSectionContext(spans, 0);
    assert(ctxCount.currentSectionIndex == -1);
    assert(ctxCount.naturalNextIndex == 0);
    assert(ctxCount.boundaryBar == 2);
    assert(!tracks::canSkipSectionInDirection(n, ctxCount, -1, -1));
    assert(tracks::computeNextQueuedSection(n, ctxCount, -1, 1) == 1); // Skip Intro -> Verse 1
    assert(!tracks::canRepeatCurrentSection(n, ctxCount));
    assert(tracks::computeToggleRepeatCurrentSection(n, ctxCount, -1) == -1);

    // Repeat current section toggle: activate -> cancel -> activate while another section was queued
    assert(tracks::canRepeatCurrentSection(n, ctxV1));
    int rep = tracks::computeToggleRepeatCurrentSection(n, ctxV1, -1);
    assert(rep == 1); // Activates repeat of Verse 1
    rep = tracks::computeToggleRepeatCurrentSection(n, ctxV1, rep);
    assert(rep == -1); // Clicking again cancels
    rep = tracks::computeToggleRepeatCurrentSection(n, ctxV1, 4);
    assert(rep == 1); // Overrides queued Chorus 2 with repeat of Verse 1
    assert(tracks::canRepeatCurrentSection(n, ctxOutro));
    assert(tracks::computeToggleRepeatCurrentSection(n, ctxOutro, -1) == 5); // Repeats Outro
    assert(tracks::computeToggleRepeatCurrentSection(n, ctxOutro, 5) == -1); // Cancels Outro repeat

    // 3. Metronome queued announcement override & jumpToFrame continuity
    tracks::Metronome met;
    met.prepare(44100.0);
    met.setTempo(120);
    met.setTimeSignature("4/4");
    met.setUseSectionCountIn(true);
    met.setSections(sections);

    // Normally bar 13 (last bar of Verse 1) announces Chorus 1
    assert(met.getScheduledGuideCueForBeat(13, 0) == "section:chorus 1");
    // Override bar 13 to announce Chorus 2 instead
    met.setQueuedAnnouncementOverride(13, "chorus 2");
    assert(met.getScheduledGuideCueForBeat(13, 0) == "section:chorus 2");
    assert(met.getScheduledGuideCueForBeat(13, 1) == "two");
    assert(met.getScheduledGuideCueForBeat(13, 2) == "three");
    assert(met.getScheduledGuideCueForBeat(13, 3) == "four");
    met.clearQueuedAnnouncementOverride();
    assert(met.getScheduledGuideCueForBeat(13, 0) == "section:chorus 1");

    // Verify jumpToFrame preserves ringing active sample tails across the jump without cutting off
    auto makeTailSample = [](int len, float amp) {
        juce::AudioBuffer<float> b(1, len);
        for (int i = 0; i < len; ++i) b.setSample(0, i, amp);
        return b;
    };
    met.reset();
    met.setClickChannel(0);
    met.setGuideChannel(1);
    met.setClickGain(0.0f);
    met.setGuideGain(1.0f);
    met.loadVoiceWord("one", makeTailSample(100, 0.5f));

    // Render 40 samples (60 samples of "one" still ringing)
    juce::AudioBuffer<float> preJump(2, 40);
    preJump.clear();
    met.renderBlock(preJump, 0, 40);
    assert(std::abs(preJump.getSample(1, 0) - 0.5f) < 1e-5f);

    // Jump to bar 30 (Chorus 2 start)
    const int64_t barFrames = met.getFramesPerBar();
    met.jumpToFrame(30 * barFrames);
    assert(met.getMetronomeFrame() == 30 * barFrames);
    assert(met.getCurrentBar() == 30);

    // Next 60 samples should still contain the ringing tail of "one", then drop to 0
    juce::AudioBuffer<float> postJump(2, 80);
    postJump.clear();
    met.renderBlock(postJump, 0, 80);
    assert(std::abs(postJump.getSample(1, 0) - 0.5f) < 1e-5f);
    assert(std::abs(postJump.getSample(1, 59) - 0.5f) < 1e-5f);
    assert(std::abs(postJump.getSample(1, 60) - 0.0f) < 1e-5f);

    // Pre-play start section selection (stopped mode):
    // With leading Count: [0: Count, 1: Intro, 2: Verse 1, 3: Chorus 1, 4: Bridge, 5: Chorus 2]
    assert(tracks::getDefaultStartSectionIndex(spans) == 1);
    assert(!tracks::canStepStartSection(spans, -1, -1));
    assert(tracks::canStepStartSection(spans, -1, 1));
    int startSel = tracks::computeNextStartSection(spans, -1, 1);
    assert(startSel == 2); // Verse 1
    startSel = tracks::computeNextStartSection(spans, startSel, 1);
    assert(startSel == 3); // Chorus 1
    startSel = tracks::computeNextStartSection(spans, startSel, 1);
    assert(startSel == 4); // Bridge
    startSel = tracks::computeNextStartSection(spans, startSel, 1);
    assert(startSel == 5); // Chorus 2 (last)
    assert(!tracks::canStepStartSection(spans, startSel, 1));
    assert(tracks::canStepStartSection(spans, startSel, -1));
    // Step back down to default (-1 = start from beginning)
    startSel = tracks::computeNextStartSection(spans, 2, -1);
    assert(startSel == -1);

    // Without leading Count: [0: Intro, 1: Verse 1, 2: Outro]
    std::vector<tracks::SongSection> noLeadSecs = {
        { std::nullopt, "Intro", 4, 0 },
        { std::nullopt, "Verse 1", 8, 1 },
        { std::nullopt, "Outro", 4, 2 }
    };
    auto noLeadSpans = tracks::buildSectionSpans(noLeadSecs, false);
    assert(tracks::getDefaultStartSectionIndex(noLeadSpans) == 0);
    assert(tracks::computeNextStartSection(noLeadSpans, -1, 1) == 1); // Verse 1
    assert(tracks::computeNextStartSection(noLeadSpans, 1, 1) == 2);  // Outro
    assert(tracks::computeNextStartSection(noLeadSpans, 1, -1) == -1); // Back to beginning

    // Verify jumping Metronome to (targetSpan.startBar - 1) gives the normal 1-bar section cue on beat 1
    met.reset();
    met.setSections(noLeadSecs);
    met.setUseSectionCountIn(true);
    const auto& outroSpan = noLeadSpans[2]; // Outro starts at timeline bar 14 (2 prepended + 4 + 8)
    assert(outroSpan.startBar == 14);
    const int cueBar = outroSpan.startBar - 1;
    assert(met.getScheduledGuideCueForBeat(cueBar, 0) == "section:outro");
    assert(met.getScheduledGuideCueForBeat(cueBar, 1) == "two");
    assert(met.getScheduledGuideCueForBeat(cueBar, 2) == "three");
    assert(met.getScheduledGuideCueForBeat(cueBar, 3) == "four");

    // Song end & crossfade transition planning tests (1..4 bar crossfade)
    {
        auto pStay = tracks::computeSongTransitionPlan(2, 32, tracks::SongTransitionMode::StayInCurrentSong, 2, true);
        assert(pStay.endBar == 34 && pStay.triggerBar == 34 && !pStay.isCrossfade);

        auto pCue = tracks::computeSongTransitionPlan(2, 32, tracks::SongTransitionMode::CueNextSong, 2, true);
        assert(pCue.endBar == 34 && pCue.triggerBar == 34 && !pCue.isCrossfade);

        auto pAuto = tracks::computeSongTransitionPlan(2, 32, tracks::SongTransitionMode::AutoplayNextSong, 2, true);
        assert(pAuto.endBar == 34 && pAuto.triggerBar == 34 && !pAuto.isCrossfade);

        auto pXf1 = tracks::computeSongTransitionPlan(2, 32, tracks::SongTransitionMode::Crossfade, 1, true);
        assert(pXf1.endBar == 34 && pXf1.triggerBar == 33 && pXf1.isCrossfade);

        auto pXf3 = tracks::computeSongTransitionPlan(0, 68, tracks::SongTransitionMode::Crossfade, 3, true);
        assert(pXf3.endBar == 68 && pXf3.triggerBar == 65 && pXf3.isCrossfade);

        // Values > 4 clamp to max 4 bars; values < 1 clamp to min 1 bar
        auto pXfMax = tracks::computeSongTransitionPlan(0, 68, tracks::SongTransitionMode::Crossfade, 9, true);
        assert(pXfMax.endBar == 68 && pXfMax.triggerBar == 64 && pXfMax.isCrossfade);
        auto pXfMin = tracks::computeSongTransitionPlan(0, 68, tracks::SongTransitionMode::Crossfade, 0, true);
        assert(pXfMin.endBar == 68 && pXfMin.triggerBar == 67 && pXfMin.isCrossfade);

        // Last song in set (hasNextSong = false): never crossfades early
        auto pLast = tracks::computeSongTransitionPlan(0, 68, tracks::SongTransitionMode::Crossfade, 4, false);
        assert(pLast.endBar == 68 && pLast.triggerBar == 68 && !pLast.isCrossfade);

        // Very short 2-bar song with Crossfade 4 bars clamps cleanly so at least 1 bar plays first
        auto pShort = tracks::computeSongTransitionPlan(2, 2, tracks::SongTransitionMode::Crossfade, 4, true);
        assert(pShort.endBar == 4 && pShort.triggerBar == 3 && pShort.isCrossfade);
    }
}

void testLibraryBackupAndImportRoundTrip() {
    std::cout << "[Test] Library Backup Export & Import Round-Trip..." << std::endl;
    namespace fs = std::filesystem;

    auto baseTmp = fs::temp_directory_path() / "tracks_backup_unit_test";
    std::error_code ec;
    fs::remove_all(baseTmp, ec);
    fs::create_directories(baseTmp);

    auto rootA = baseTmp / "machineA";
    auto rootB = baseTmp / "machineB";
    auto tracksA = rootA / "uploaded_tracks";
    auto tracksB = rootB / "uploaded_tracks";
    fs::create_directories(tracksA / "1_Test_Song" / "1_Main" / "tracks");
    fs::create_directories(rootB);

    // Create a mock audio file under machineA/uploaded_tracks/...
    auto stemPathA = tracksA / "1_Test_Song" / "1_Main" / "tracks" / "Keys.wav";
    const std::string stemBytes = "RIFF_MOCK_AUDIO_PAYLOAD_1234567890";
    {
        std::ofstream ofs(stemPathA, std::ios::binary);
        ofs << stemBytes;
    }

    auto zipFile = baseTmp / "test_library_backup.zip";
    {
        tracks::DatabaseService dbA((rootA / "tracks_library.sqlite3").string());
        tracks::Song song;
        song.title = "Test Song";
        song.artist = "Test Band";
        song.tempo = 124;
        song.key = "G";
        song.timeSignature = "4/4";

        tracks::SongArrangement arr;
        arr.name = "Main";
        arr.tempo = 124;
        arr.key = "G";
        arr.timeSignature = "4/4";
        arr.clickEighths = true;
        tracks::SongTrack trk;
        trk.name = "Keys";
        trk.filePath = fs::absolute(stemPathA).string();
        trk.outputChannel = 2;
        trk.stereoOutput = true;
        trk.gain = 0.75f;
        arr.tracks.push_back(trk);
        arr.sections.push_back({ std::nullopt, "Intro", 4, 0 });
        arr.sections.push_back({ std::nullopt, "Chorus 1", 8, 1 });
        song.arrangements.push_back(arr);

        auto savedSong = dbA.saveSong(song);
        assert(savedSong.id.has_value());

        tracks::Setlist sl;
        sl.name = "Sunday Morning";
        sl.setDate = "2026-12-01";
        tracks::SetlistEntry entry;
        entry.songId = *savedSong.id;
        entry.orderIndex = 0;
        entry.overrideTempo = 128;
        entry.overrideKey = "A";
        sl.entries.push_back(entry);
        dbA.saveSetlist(sl);
        dbA.setAppSetting("audio.click_pack", "Classic");

        auto expRes = tracks::LibraryBackupService::exportBackup(dbA, zipFile.string(), tracksA);
        assert(expRes.success);
        assert(expRes.songCount == 1);
        assert(expRes.setlistCount == 1);
        assert(expRes.audioFileCount == 1);
        assert(fs::exists(zipFile));
    }

    // Import into a completely separate directory (machineB) and verify full restore + path remapping
    {
        tracks::DatabaseService dbB((rootB / "tracks_library.sqlite3").string());
        bool beforeReplaceCalled = false;
        auto impRes = tracks::LibraryBackupService::importBackup(
            dbB,
            zipFile.string(),
            tracksB,
            [&]() { beforeReplaceCalled = true; });
        assert(impRes.success);
        assert(beforeReplaceCalled);
        assert(impRes.songCount == 1);
        assert(impRes.setlistCount == 1);
        assert(impRes.audioFileCount == 1);

        auto songs = dbB.listSongs();
        assert(songs.size() == 1);
        assert(songs[0].title == "Test Song");
        assert(songs[0].arrangements.size() == 1);
        assert(songs[0].arrangements[0].clickEighths == true);
        assert(songs[0].arrangements[0].sections.size() == 2);
        assert(songs[0].arrangements[0].tracks.size() == 1);

        const std::string restoredPath = songs[0].arrangements[0].tracks[0].filePath;
        assert(restoredPath.find("machineB") != std::string::npos);
        assert(fs::exists(restoredPath));
        std::ifstream ifs(restoredPath, std::ios::binary);
        std::string restoredBytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        assert(restoredBytes == stemBytes);

        auto setlists = dbB.listSetlists();
        assert(setlists.size() == 1);
        assert(setlists[0].name == "Sunday Morning");
        auto loadedSet = dbB.loadSetlist(*setlists[0].id);
        assert(loadedSet.has_value() && loadedSet->entries.size() == 1);
        assert(loadedSet->entries[0].overrideTempo.value_or(0) == 128);
        assert(loadedSet->entries[0].overrideKey.value_or("") == "A");
        assert(dbB.getAppSetting("audio.click_pack") == "Classic");

        // Verify importing an invalid file fails cleanly without wiping existing library
        auto badZip = baseTmp / "not_a_backup.zip";
        {
            std::ofstream ofs(badZip);
            ofs << "invalid zip bytes";
        }
        auto badRes = tracks::LibraryBackupService::importBackup(dbB, badZip.string(), tracksB);
        assert(!badRes.success);
        assert(dbB.listSongs().size() == 1);
    }

    fs::remove_all(baseTmp, ec);
}

void testTempoMapAndMidiImport() {
    std::cout << "[Test] TempoMap Automation, Ritardando Click Timing & MIDI Import..." << std::endl;

    // 1. Exact closed-form ramp math and round-trip invertibility
    tracks::TempoPoint slowDown;
    slowDown.bar = 3;      // Audio Bar 3 (timeline Bar 5 when prependedCountBars = 2)
    slowDown.beat = 1.0;
    slowDown.bpm = 60.0;
    slowDown.rampBars = 2.0; // Ramp 120 -> 60 BPM over 2 bars (8 beats)

    tracks::TempoMap map;
    map.rebuild(44100.0, 120.0, 1.0, "4/4", 2, { slowDown });
    assert(map.hasTempoAutomation());

    // Timeline bars 0..4 (16 beats) at constant 120 BPM => 8.0 seconds exactly
    assert(std::abs(map.pulseToSeconds(16.0) - 8.0) < 1e-9);

    // Ramp from pulse 16 to pulse 24 (8 beats, 120 -> 60 BPM):
    // Exact duration = 60 * 8 / (60 - 120) * ln(60 / 120) = 8 * ln(2) seconds
    const double expectedRampSec = 8.0 * std::log(2.0);
    assert(std::abs((map.pulseToSeconds(24.0) - 8.0) - expectedRampSec) < 1e-9);

    // Post-ramp bar (pulse 24 -> 28, 4 beats at 60 BPM => 4.0 seconds)
    assert(std::abs((map.pulseToSeconds(28.0) - map.pulseToSeconds(24.0)) - 4.0) < 1e-9);

    // Round-trip frame -> pulse -> frame across constant, ramp, and post-ramp regions
    for (double p : { 0.0, 4.0, 16.0, 18.5, 21.25, 24.0, 27.5 }) {
        int64_t f = map.pulseToFrame(p);
        double backP = map.frameToPulse(f);
        assert(std::abs(backP - p) < 1e-4);
    }

    // 2. Global tempoScale (e.g. -6 BPM playback override on 120 BPM = 0.95x):
    // Every frame position must scale by exactly 1 / 0.95 everywhere on the timeline!
    tracks::TempoMap scaledMap;
    scaledMap.rebuild(44100.0, 120.0, 114.0 / 120.0, "4/4", 2, { slowDown });
    for (double p : { 8.0, 16.0, 20.0, 24.0, 32.0 }) {
        double origF = map.pulseToFrameDouble(p);
        double scaledF = scaledMap.pulseToFrameDouble(p);
        assert(std::abs(scaledF * (114.0 / 120.0) - origF) < 1e-6);
    }

    // 3. Compound 6/8 tempo map (60 BPM dotted-quarter = 3 eighth pulses per second)
    tracks::TempoMap map68;
    map68.rebuild(44100.0, 60.0, 1.0, "6/8", 0, {});
    assert(map68.isCompound());
    assert(map68.getPulseCountPerBar() == 6);
    assert(std::abs(map68.pulseToSeconds(6.0) - 2.0) < 1e-9);

    // 4. Metronome click impulse rendering across a ritardando (120 -> 60 BPM over 2 bars starting Audio Bar 2)
    tracks::Metronome met;
    met.prepare(44100.0);
    met.setTimeSignature("4/4");
    met.setTempo(120);
    met.setClickChannel(0);
    met.setClickGain(1.0f);
    met.setGuideGain(0.0f);
    met.setUseSectionCountIn(true);
    met.setSections({ { std::nullopt, "Count", 1, 0 }, { std::nullopt, "Outro", 4, 1 } });

    juce::AudioBuffer<float> clickImpulse(1, 4);
    clickImpulse.clear();
    clickImpulse.setSample(0, 0, 1.0f);
    met.loadClickSamples(clickImpulse, clickImpulse);

    tracks::TempoPoint rit;
    rit.bar = 2; // Start slowing on Audio Bar 2 (timeline bar 1 since leading Count = 1 bar)
    rit.beat = 1.0;
    rit.bpm = 60.0;
    rit.rampBars = 2.0;
    met.setTempoMap(120, { rit });
    met.reset();

    // Render 5 bars (timeline bars 0..5) and collect every click frame
    const int64_t totalFrames = met.getFrameForBar(5.0) + 1024;
    std::vector<int64_t> clickFrames;
    juce::AudioBuffer<float> block(1, 512);
    int64_t rendered = 0;
    while (rendered < totalFrames) {
        int n = static_cast<int>(std::min<int64_t>(512, totalFrames - rendered));
        block.clear();
        met.renderBlock(block, 0, n);
        for (int i = 0; i < n; ++i) {
            if (block.getSample(0, i) > 0.5f) {
                clickFrames.push_back(rendered + i);
            }
        }
        rendered += n;
    }

    assert(clickFrames.size() >= 20);
    // Bar 0 (beats 0..3): constant 120 BPM = 22050 samples per beat
    for (int i = 0; i < 4; ++i) {
        int64_t dt = clickFrames[static_cast<size_t>(i + 1)] - clickFrames[static_cast<size_t>(i)];
        assert(std::abs(dt - 22050) <= 1);
    }
    // Bars 1..2 (beats 4..11): ritardando -> intervals strictly increase each beat
    int64_t prevDt = 22050;
    for (int i = 4; i < 12; ++i) {
        int64_t dt = clickFrames[static_cast<size_t>(i + 1)] - clickFrames[static_cast<size_t>(i)];
        assert(dt > prevDt);
        prevDt = dt;
    }
    // Bars 3..4 (beats 12..15): settled at 60 BPM = 44100 samples per beat
    for (int i = 12; i < 16; ++i) {
        int64_t dt = clickFrames[static_cast<size_t>(i + 1)] - clickFrames[static_cast<size_t>(i)];
        assert(std::abs(dt - 44100) <= 1);
    }

    // 5. MIDI Tempo Map parser test using synthetic Standard MIDI File (480 TPQ)
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(480);
    juce::MidiMessageSequence tempoTrack;
    tempoTrack.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0.0);               // Bar 1 Beat 1: 120 BPM
    tempoTrack.addEvent(juce::MidiMessage::tempoMetaEvent(600000), 480.0 * 4.0 * 4.0); // Bar 5 Beat 1: 100 BPM
    tempoTrack.addEvent(juce::MidiMessage::tempoMetaEvent(750000), 480.0 * (4.0 * 8.0 + 2.0)); // Bar 9 Beat 3: 80 BPM
    midi.addTrack(tempoTrack);

    auto midiRes = tracks::parseMidiTempoMap(midi, "4/4", 120);
    assert(midiRes.success);
    assert(midiRes.detectedBaseTempo.has_value() && *midiRes.detectedBaseTempo == 120);
    assert(midiRes.points.size() == 2);
    assert(midiRes.points[0].bar == 5 && std::abs(midiRes.points[0].beat - 1.0) < 1e-3 &&
           std::abs(midiRes.points[0].bpm - 100.0) < 1e-2);
    assert(midiRes.points[1].bar == 9 && std::abs(midiRes.points[1].beat - 3.0) < 1e-3 &&
           std::abs(midiRes.points[1].bpm - 80.0) < 1e-2);
}

void testStreamingAudioTrack() {
    std::cout << "[Test] StreamingAudioTrack Chunked Disk Streaming, Boundaries & Eviction..." << std::endl;
    namespace fs = std::filesystem;

    auto require = [](bool cond, const char* msg) {
        if (!cond) {
            throw std::runtime_error(std::string("StreamingAudioTrack test failed: ") + msg);
        }
    };

    auto tmpDir = fs::temp_directory_path() / "tracks_streaming_unit_test";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir);

    // 1. Write a multi-chunk stereo 16-bit WAV (~2.7 chunks = 176,400 samples = 4.0s @ 44.1kHz)
    //    with distinct amplitude profile (quiet intro, loud middle, silent tail) and deterministic sine waves.
    const int totalSamples = 176400;
    const double sr = 44100.0;
    juce::AudioBuffer<float> refBuf(2, totalSamples);
    for (int i = 0; i < totalSamples; ++i) {
        float amp = 0.0f;
        if (i < 44100) amp = 0.25f;
        else if (i < 132300) amp = 0.85f;
        else amp = 0.0f;
        const float phaseL = static_cast<float>(2.0 * 3.14159265358979323846 * 440.0 * i / sr);
        const float phaseR = static_cast<float>(2.0 * 3.14159265358979323846 * 660.0 * i / sr);
        refBuf.setSample(0, i, amp * std::sin(phaseL));
        refBuf.setSample(1, i, amp * std::cos(phaseR));
    }

    auto wavPath = tmpDir / "stream_test.wav";
    {
        juce::WavAudioFormat wavFormat;
        juce::File outFile(wavPath.string());
        std::unique_ptr<juce::FileOutputStream> fos(outFile.createOutputStream());
        require(fos != nullptr && fos->openedOk(), "createOutputStream");
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(fos.get(), sr, 2, 16, {}, 0));
        require(writer != nullptr, "createWriterFor");
        fos.release(); // owned by writer now
        const bool wroteOk = writer->writeFromAudioSampleBuffer(refBuf, 0, totalSamples);
        require(wroteOk, "writeFromAudioSampleBuffer");
    }

    auto stream = tracks::StreamingAudioTrack::createFromFile(wavPath.string());
    require(stream != nullptr, "createFromFile wav");
    require(stream->getNumChannels() == 2, "numChannels == 2");
    require(stream->getTotalSamples() == totalSamples, "totalSamples match");
    require(stream->getTotalChunks() == 3, "totalChunks == 3");

    // Only Chunk 0 should be resident initially (low RAM!)
    require(stream->hasChunk(0), "chunk 0 pinned");
    require(!stream->hasChunk(1), "chunk 1 not loaded initially");
    require(!stream->hasChunk(2), "chunk 2 not loaded initially");

    // Waveform envelope should capture quiet intro, loud middle, and silent tail
    require(stream->getWaveformBin(64) > 0.12f && stream->getWaveformBin(64) < 0.35f, "quiet intro bin");
    require(stream->getWaveformBin(256) > 0.50f, "loud middle bin");
    require(stream->getWaveformBin(480) < 0.01f, "silent tail bin");

    // Stream chunks 1 and 2 via reader, then verify sample-accurate reads across chunk boundaries
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(juce::File(wavPath.string())));
    require(reader != nullptr, "open wav reader");
    juce::AudioBuffer<float> readScratch(2, tracks::StreamingAudioTrack::kChunkSamples);

    int loadedCount = 0;
    while (true) {
        int64_t nextChunk = stream->findNextChunkToLoadAndEvict(true);
        if (nextChunk <= 0) break;
        const bool ok = stream->loadChunkFromReader(nextChunk, *reader, readScratch);
        require(ok, "loadChunkFromReader");
        ++loadedCount;
    }
    require(loadedCount == 2, "loaded 2 chunks");
    require(stream->hasChunk(1) && stream->hasChunk(2), "chunks 1 and 2 resident");

    // Verify reads straddling Chunk 0->1 boundary (65536) and Chunk 1->2 boundary (131072)
    auto verifyRange = [&](int64_t startF, int count) {
        int seen = 0;
        stream->forEachSlice(startF, count, [&](const int16_t* sL, const int16_t* sR, int off, int len) {
            require(sL != nullptr && sR != nullptr, "non-null stereo pointers");
            for (int i = 0; i < len; ++i) {
                const int idx = static_cast<int>(startF) + off + i;
                const float gotL = static_cast<float>(sL[i]) / 32768.0f;
                const float gotR = static_cast<float>(sR[i]) / 32768.0f;
                require(std::abs(gotL - refBuf.getSample(0, idx)) < 0.002f, "left sample match");
                require(std::abs(gotR - refBuf.getSample(1, idx)) < 0.002f, "right sample match");
            }
            seen += len;
        });
        require(seen == count, "full slice coverage");
    };

    verifyRange(0, 512);
    verifyRange(tracks::StreamingAudioTrack::kChunkSamples - 128, 512);
    verifyRange(tracks::StreamingAudioTrack::kChunkSamples * 2 - 64, 256);

    // Trimming on stop frees non-zero chunks while keeping Chunk 0 pinned
    stream->trimToIntroOnly();
    require(stream->hasChunk(0), "chunk 0 stays pinned after trim");
    require(!stream->hasChunk(1), "chunk 1 evicted on trim");
    require(!stream->hasChunk(2), "chunk 2 evicted on trim");

    // Verify compressed M4A streaming creation works cleanly too
    auto guideDir = fs::current_path() / "assets" / "guide" / "playback_female";
    if (fs::exists(guideDir)) {
        for (const auto& entry : fs::directory_iterator(guideDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".m4a") {
                auto m4aStream = tracks::StreamingAudioTrack::createFromFile(entry.path().string());
                require(m4aStream != nullptr, "createFromFile m4a");
                require(m4aStream->getTotalSamples() > 0, "m4a totalSamples > 0");
                require(m4aStream->hasChunk(0), "m4a chunk 0 pinned");
                break;
            }
        }
    }

    fs::remove_all(tmpDir, ec);
}

int main() {
    try {
        testMetronomeCalculations();
        testStereoRoutingLogic();
        testSongStorageDirNaming();
        testPitchShiftLogic();
        testQueuedSectionSkipLogic();
        testLibraryBackupAndImportRoundTrip();
        testTempoMapAndMidiImport();
        testStreamingAudioTrack();
        std::cout << "[Test] All AudioEngine tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Audio test failed: " << e.what() << std::endl;
        return 1;
    }
}
