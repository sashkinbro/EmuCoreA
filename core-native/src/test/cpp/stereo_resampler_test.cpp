// Exercises the PPSSPP resampler linked into the shipped native core.
#include "Core/Config.h"
#include "Core/HW/StereoResampler.h"

#include <cstdio>
#include <vector>

static bool TestUnderrunChannels() {
    g_Config.bExtraAudioBuffering = false;
    StereoResampler resampler;
    resampler.Clear();
    std::vector<int32_t> input(128 * 2);
    for (size_t i = 0; i < input.size(); i += 2) {
        input[i] = 12000;
        input[i + 1] = -6000;
    }
    resampler.PushSamples(input.data(), 128, 1.0f);
    std::vector<int16_t> output(512 * 2);
    resampler.Mix(output.data(), 512, false, 48000);
    for (size_t i = 0; i < output.size(); i += 2) {
        if (output[i] != 12000 || output[i + 1] != -6000) {
            std::fprintf(stderr, "Stereo underrun changed channels at frame %zu: L=%d R=%d\n",
                         i / 2, output[i], output[i + 1]);
            return false;
        }
    }
    // Empty subsequent callbacks must also retain the original channel order.
    resampler.Mix(output.data(), 512, false, 48000);
    if (output[0] != 12000 || output[1] != -6000) return false;
    return true;
}

static bool TestResetDiscardsAudioAndHeldTail() {
    StereoResampler resampler;
    std::vector<int32_t> input(256 * 2, 12000);
    std::vector<int16_t> output(512 * 2, -1);
    resampler.PushSamples(input.data(), 256, 1.0f);
    resampler.Mix(output.data(), 64, false, 48000);
    // Restarting a stream must discard both buffered audio and the previous
    // callback's non-zero underrun tail.
    resampler.ResetForOutput();
    resampler.Mix(output.data(), 512, false, 48000);
    for (int16_t sample : output) {
        if (sample != 0) {
            std::fprintf(stderr, "Output reset leaked a previous stream sample: %d\n", sample);
            return false;
        }
    }
    for (size_t i = 0; i < input.size(); i += 2) {
        input[i] = 7000;
        input[i + 1] = -3000;
    }
    resampler.PushSamples(input.data(), 256, 1.0f);
    resampler.Mix(output.data(), 512, false, 48000);
    for (size_t i = 0; i < output.size(); i += 2) {
        if (output[i] != 7000 || output[i + 1] != -3000) return false;
    }
    return true;
}

static bool TestDeviceRateKeeps44100HzSource(int outputRate, int minimum, int maximum) {
    StereoResampler resampler;
    resampler.ResetForOutput();
    std::vector<int32_t> input(2048 * 2);
    for (size_t i = 0; i < 2048; ++i) {
        input[i * 2] = static_cast<int32_t>(i * 10);
        input[i * 2 + 1] = -static_cast<int32_t>(i * 10);
    }
    resampler.PushSamples(input.data(), 2048, 1.0f);
    std::vector<int16_t> output(512 * 2);
    resampler.Mix(output.data(), 512, false, outputRate);
    // A ramp exposes which input frame was consumed. Broad intervals allow
    // the intentional small drift correction while distinguishing the 44.1k
    // source from an incorrectly substituted device sample rate.
    const int left = output[400 * 2];
    const int right = output[400 * 2 + 1];
    if (left < minimum || left > maximum || left + right < -1 || left + right > 1) {
        std::fprintf(stderr, "%d Hz device consumed wrong source position: L=%d R=%d\n", outputRate, left, right);
        return false;
    }
    return true;
}

int main() {
    g_Config.bExtraAudioBuffering = false;
    if (!TestUnderrunChannels()) return 1;
    if (!TestResetDiscardsAudioAndHeldTail()) return 2;
    if (!TestDeviceRateKeeps44100HzSource(44100, 3900, 4050)) return 3;
    if (!TestDeviceRateKeeps44100HzSource(48000, 3550, 3750)) return 4;
    if (!TestDeviceRateKeeps44100HzSource(96000, 1750, 1900)) return 5;
    std::puts("PPSSPP stereo channels, restart silence, and 44.1/48/96k device conversion: PASS");
    return 0;
}
