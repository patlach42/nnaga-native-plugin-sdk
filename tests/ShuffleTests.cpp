#include "nnaga/native_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr uint32_t kLoopFrames = 48000;
constexpr uint32_t kTotalFrames = kLoopFrames * 2;
constexpr uint32_t kSliceFrames = kLoopFrames / 8;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

struct RunResult {
    std::vector<float> input_left;
    std::vector<float> input_right;
    std::vector<float> output_left;
    std::vector<float> output_right;
};

RunResult run_shuffle(float seed, float enabled) {
    const NnagaPluginLibraryV1* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->plugin_count == 1 && library->get_plugin, "shuffle library descriptor");
    const NnagaPluginDescriptorV1* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->create && descriptor->destroy && descriptor->activate && descriptor->process,
            "shuffle callbacks");

    NnagaPluginHandle handle = descriptor->create();
    require(handle != nullptr, "shuffle create");
    require(descriptor->activate(handle, 48000.0, 512) != 0, "shuffle activate");

    descriptor->set_parameter(handle, 4, enabled);
    descriptor->set_parameter(handle, 5, 0.5f);
    descriptor->set_parameter(handle, 6, 1.0f);
    descriptor->set_parameter(handle, 7, seed);
    descriptor->set_parameter(handle, 8, 1.0f);

    RunResult result;
    result.input_left.resize(kTotalFrames);
    result.input_right.resize(kTotalFrames);
    result.output_left.resize(kTotalFrames);
    result.output_right.resize(kTotalFrames);
    for (uint32_t frame = 0; frame < kTotalFrames; ++frame) {
        const float slice_value = static_cast<float>((frame % kLoopFrames) / kSliceFrames + 1);
        result.input_left[frame] = slice_value;
        result.input_right[frame] = -slice_value;
    }

    NnagaProcessContextV1 context{
        sizeof(context), 0, 0, kLoopFrames, 48000.0, 120.0, 1, 1, 0,
        0.0, 0, 0.0, 0.0, 4.0f, 4,
    };
    descriptor->process(handle, result.input_left.data(), result.input_right.data(),
                        result.output_left.data(), result.output_right.data(), kTotalFrames, &context);
    descriptor->destroy(handle);
    return result;
}
}

int main() {
    require(nnaga_plugin_entry(0) == nullptr, "bad ABI must reject");

    const RunResult first = run_shuffle(0.0f, 1.0f);
    const RunResult repeat = run_shuffle(0.0f, 1.0f);
    require(first.output_left == repeat.output_left && first.output_right == repeat.output_right,
            "same seed is exactly deterministic");

    const RunResult alternate = run_shuffle(0.5f, 1.0f);
    bool permutation_changed = false;
    for (uint32_t frame = kLoopFrames; frame < kTotalFrames; ++frame) {
        if (first.output_left[frame] != alternate.output_left[frame] ||
            first.output_right[frame] != alternate.output_right[frame]) {
            permutation_changed = true;
            break;
        }
    }
    require(permutation_changed, "different seed changes permutation");

    for (uint32_t frame = 0; frame < kTotalFrames; ++frame) {
        require(std::isfinite(first.output_left[frame]) && std::isfinite(first.output_right[frame]),
                "shuffle output remains finite");
        require(std::fabs(first.output_left[frame] + first.output_right[frame]) < 1.0e-5f,
                "stereo channels remain aligned");
    }

    const RunResult disabled = run_shuffle(0.0f, 0.0f);
    require(disabled.output_left == disabled.input_left && disabled.output_right == disabled.input_right,
            "disabled shuffle is an exact passthrough");
    return 0;
}
