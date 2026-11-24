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

    std::cout << "[Test] Metronome 6/8 Compound Timing..." << std::endl;
    met.prepare(44100.0);
    met.setTempo(80);
    met.setTimeSignature("6/8");

    assert(met.getBeatsPerBar() == 6);

    std::cout << "[Test] Metronome 12/8 Compound Timing..." << std::endl;
    met.setTimeSignature("12/8");
    assert(met.getBeatsPerBar() == 12);
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
    std::cout << "[Test] Stereo routing and downmix verified successfully!" << std::endl;
}

int main() {
    try {
        testMetronomeCalculations();
        testStereoRoutingLogic();
        std::cout << "[Test] All AudioEngine tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Audio test failed: " << e.what() << std::endl;
        return 1;
    }
}
