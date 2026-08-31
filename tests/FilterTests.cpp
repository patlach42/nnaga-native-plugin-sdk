#include "nnaga/native_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

float normalized_log(float value, float minimum, float maximum) {
    return std::log(value / minimum) / std::log(maximum / minimum);
}

float rms(const std::vector<float>& data, uint32_t offset) {
    double total = 0.0;
    for (uint32_t i = offset; i < data.size(); ++i) total += static_cast<double>(data[i]) * data[i];
    return static_cast<float>(std::sqrt(total / static_cast<double>(data.size() - offset)));
}

void fill_sine(std::vector<float>& out, float frequency) {
    for (uint32_t i = 0; i < out.size(); ++i)
        out[i] = std::sin(2.0f * 3.14159265358979323846f * frequency * static_cast<float>(i) / 48000.0f);
}

float run_gain(const NnagaPluginDescriptorV1* descriptor, NnagaPluginHandle handle,
               float type, float cutoff, float signal_frequency) {
    descriptor->set_parameter(handle, 4, type);
    descriptor->set_parameter(handle, 5, normalized_log(cutoff, 20.0f, 20000.0f));
    descriptor->set_parameter(handle, 6, normalized_log(0.70710678f, 0.1f, 18.0f));
    std::vector<float> input(48000), output(48000);
    fill_sine(input, signal_frequency);
    NnagaProcessContextV1 context{sizeof(context), 0, 0, 0, 48000.0, 120.0, 1, 0, 0, 0.0, 0, 0.0, 0.0, 4.0f, 4};
    descriptor->process(handle, input.data(), input.data(), output.data(), output.data(),
                        static_cast<uint32_t>(input.size()), &context);
    return rms(output, 24000) / rms(input, 24000);
}
}

int main() {
    require(nnaga_plugin_entry(0) == nullptr, "bad ABI must reject");
    const NnagaPluginLibraryV1* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->plugin_count == 1 && library->get_plugin, "library descriptor");
    const NnagaPluginDescriptorV1* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->parameter_count == 3 && descriptor->audio_inputs == 2 && descriptor->audio_outputs == 2,
            "filter descriptor");
    NnagaPluginHandle handle = descriptor->create();
    require(handle && descriptor->activate(handle, 48000.0, 512), "filter activate");

    const float lp_low = run_gain(descriptor, handle, 0.0f, 1000.0f, 100.0f);
    descriptor->reset(handle);
    const float lp_high = run_gain(descriptor, handle, 0.0f, 1000.0f, 10000.0f);
    descriptor->reset(handle);
    const float hp_low = run_gain(descriptor, handle, 1.0f, 1000.0f, 100.0f);
    descriptor->reset(handle);
    const float hp_high = run_gain(descriptor, handle, 1.0f, 1000.0f, 10000.0f);
    require(lp_low >= 0.94f && lp_high <= 0.02f, "low-pass response");
    require(hp_low <= 0.02f && hp_high >= 0.94f, "high-pass response");

    std::vector<float> left(257, 0.0f), right(257, 0.0f), in_place_left, in_place_right;
    left[0] = 1.0f;
    right[0] = -1.0f;
    in_place_left = left;
    in_place_right = right;
    std::vector<float> out_left(257), out_right(257);
    NnagaProcessContextV1 context{sizeof(context), 0, 0, 0, 48000.0, 120.0, 1, 0, 0, 0.0, 0, 0.0, 0.0, 4.0f, 4};
    descriptor->set_parameter(handle, 5, 0.0f);
    descriptor->set_parameter(handle, 6, 1.0f);
    require(descriptor->activate(handle, 48000.0, 512), "re-activate extreme filter");
    descriptor->process(handle, left.data(), right.data(), out_left.data(), out_right.data(), 257, &context);
    descriptor->reset(handle);
    descriptor->process(handle, in_place_left.data(), in_place_right.data(), in_place_left.data(), in_place_right.data(), 257, &context);
    for (uint32_t i = 0; i < 257; ++i) {
        require(std::isfinite(out_left[i]) && std::isfinite(out_right[i]), "finite extremes");
        require(std::fabs(out_left[i] - in_place_left[i]) < 1.0e-6f && std::fabs(out_right[i] - in_place_right[i]) < 1.0e-6f,
                "in-place parity");
    }
    descriptor->deactivate(handle);
    descriptor->destroy(handle);
    return 0;
}
