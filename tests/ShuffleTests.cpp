#include "nnaga/native_plugin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kRate = 48000;
constexpr uint32_t kBarFrames = 96000;
constexpr uint32_t kBlockFrames = 512;

void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}
struct RunResult { std::vector<float> in_l, in_r, out_l, out_r; };

RunResult run_shuffle(uint32_t bars, float grid, float seed, float enabled = 1.0f, bool playing = true,
                      float start = -1.0f, float end = -1.0f, uint32_t cycles = 2) {
    const auto* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->plugin_count == 1, "shuffle library descriptor");
    const auto* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->parameter_count == 8, "bars and range parameter descriptor");
    NnagaPluginHandle handle = descriptor->create();
    require(handle && descriptor->activate(handle, kRate, kBlockFrames), "shuffle activate");
    descriptor->set_parameter(handle, 4, enabled);
    descriptor->set_parameter(handle, 5, grid);
    descriptor->set_parameter(handle, 6, 1.0f);
    descriptor->set_parameter(handle, 7, seed);
    descriptor->set_parameter(handle, 8, 1.0f);
    descriptor->set_parameter(handle, 9, static_cast<float>(bars - 1) / 7.0f);
    if (start >= 0.0f) descriptor->set_parameter(handle, 10, start);
    if (end >= 0.0f) descriptor->set_parameter(handle, 11, end);

    const uint32_t cycle = bars * kBarFrames;
    const uint32_t total = cycle * cycles;
    RunResult result;
    result.in_l.resize(total); result.in_r.resize(total); result.out_l.resize(total); result.out_r.resize(total);
    for (uint32_t frame = 0; frame < total; ++frame) {
        const uint32_t cycle_index = frame / cycle;
        const uint32_t bar_index = (frame % cycle) / kBarFrames;
        const float value = cycle_index == 0 ? static_cast<float>(bar_index + 1)
                                             : static_cast<float>(100 * cycle_index);
        result.in_l[frame] = value; result.in_r[frame] = -value;
    }
    for (uint32_t offset = 0; offset < total; offset += kBlockFrames) {
        const uint32_t frames = std::min(kBlockFrames, total - offset);
        NnagaProcessContextV1 context{sizeof(context), offset, offset, 0, static_cast<double>(kRate), 120.0,
            static_cast<uint8_t>(playing), 0, 0, 0.0, 0, 0.0, 0.0, 4.0f, 4};
        descriptor->process(handle, result.in_l.data() + offset, result.in_r.data() + offset,
                            result.out_l.data() + offset, result.out_r.data() + offset, frames, &context);
    }
    descriptor->destroy(handle);
    return result;
}

RunResult run_identity_ramp() {
    const auto* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->plugin_count == 1, "shuffle library descriptor");
    const auto* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->parameter_count == 8, "shuffle parameter descriptor");
    NnagaPluginHandle handle = descriptor->create();
    require(handle && descriptor->activate(handle, kRate, kBlockFrames), "shuffle activate");
    descriptor->set_parameter(handle, 4, 1.0f);
    descriptor->set_parameter(handle, 5, 0.5f);
    descriptor->set_parameter(handle, 6, 0.0f);
    descriptor->set_parameter(handle, 7, 0.0f);
    descriptor->set_parameter(handle, 8, 1.0f);
    descriptor->set_parameter(handle, 9, 0.0f);

    const uint32_t total = 2 * kBarFrames;
    RunResult result;
    result.in_l.resize(total); result.in_r.resize(total); result.out_l.resize(total); result.out_r.resize(total);
    for (uint32_t frame = 0; frame < total; ++frame) {
        result.in_l[frame] = static_cast<float>(frame);
        result.in_r[frame] = -static_cast<float>(frame);
    }
    for (uint32_t offset = 0; offset < total; offset += kBlockFrames) {
        const uint32_t frames = std::min(kBlockFrames, total - offset);
        require(frames == kBlockFrames, "identity ramp uses fixed host block size");
        NnagaProcessContextV1 context{sizeof(context), offset, offset, 0, static_cast<double>(kRate), 120.0,
            1, 0, 0, 0.0, 0, 0.0, 0.0, 4.0f, 4};
        require(context.sample_rate == static_cast<double>(kRate) &&
                context.transport_frame == offset, "identity ramp transport timing");
        descriptor->process(handle, result.in_l.data() + offset, result.in_r.data() + offset,
                            result.out_l.data() + offset, result.out_r.data() + offset, frames, &context);
    }
    descriptor->destroy(handle);
    return result;
}

void assert_identity_ramp(const RunResult& result) {
    for (uint32_t phase = 0; phase < kBarFrames; ++phase)
        require(result.out_l[phase] == result.in_l[phase] &&
                result.out_r[phase] == result.in_r[phase],
                "identity ramp capture is dry");
    for (uint32_t phase = 0; phase < kBarFrames; ++phase) {
        if (phase % (kBarFrames / 8) < 32) continue;
        const uint32_t wet = kBarFrames + phase;
        require(result.out_l[wet] == result.in_l[phase] &&
                result.out_r[wet] == result.in_r[phase],
                "identity ramp repeats the previous cycle at the same phase");
    }
}
void assert_dry_segment(const RunResult& result, uint32_t begin, uint32_t end, const char* message);
void assert_dry_prefix(const RunResult& result, uint32_t frames, const char* message) {
    assert_dry_segment(result, 0, frames, message);
}

void assert_dry_segment(const RunResult& result, uint32_t begin, uint32_t end, const char* message) {
    for (uint32_t i = begin; i < end; ++i)
        require(result.out_l[i] == result.in_l[i] && result.out_r[i] == result.in_r[i], message);
}

void assert_position_format(const NnagaPluginDescriptorV1* descriptor, NnagaPluginHandle handle,
                            uint32_t port, float value, const char* expected, const char* message) {
    char output[32] = {};
    const uint32_t length = descriptor->format_parameter(handle, port, value, output, sizeof(output));
    require(length == std::strlen(expected) && std::strcmp(output, expected) == 0, message);
}
}

int main() {
    require(nnaga_plugin_entry(0) == nullptr, "bad ABI rejects");
    const auto* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->plugin_count == 1, "shuffle formatting library descriptor");
    const auto* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->parameter_count == 8 && descriptor->format_parameter,
            "shuffle range descriptor");
    NnagaPluginHandle formatHandle = descriptor->create();
    require(formatHandle && descriptor->activate(formatHandle, kRate, kBlockFrames), "shuffle formatting activate");
    descriptor->set_parameter(formatHandle, 9, 0.0f);
    assert_position_format(descriptor, formatHandle, 10, 0.0f, "1:1:1", "one-bar start formatting");
    assert_position_format(descriptor, formatHandle, 11, 1.0f, "2:1:1", "one-bar end formatting");
    descriptor->set_parameter(formatHandle, 9, 1.0f / 7.0f);
    assert_position_format(descriptor, formatHandle, 11, 1.0f, "3:1:1", "two-bar end formatting");
    descriptor->destroy(formatHandle);

    const RunResult identityRamp = run_identity_ramp();
    assert_identity_ramp(identityRamp);
    const RunResult one = run_shuffle(1, 0.5f, 0.0f);
    assert_dry_prefix(one, kBarFrames, "one bar capture is dry");
    require(one.out_l[kBarFrames + 64] != one.in_l[kBarFrames + 64] &&
            one.out_l[kBarFrames + 64] >= 1.0f && one.out_l[kBarFrames + 64] <= 1.0f,
            "one bar becomes wet in production context");

    const RunResult ranged = run_shuffle(1, 0.5f, 0.0f, 1.0f, true, 0.5f, 1.0f, 3);
    assert_dry_prefix(ranged, kBarFrames, "range capture starts dry");
    assert_dry_segment(ranged, kBarFrames, kBarFrames + kBarFrames / 2,
                       "range leaves the first half of the cycle dry");
    const uint32_t rangedWet = kBarFrames + 3 * kBarFrames / 4;
    require(ranged.out_l[rangedWet] == 1.0f && ranged.out_r[rangedWet] == -1.0f &&
            ranged.out_l[rangedWet] != ranged.in_l[rangedWet],
            "range enables wet output in the second half");
    assert_dry_segment(ranged, 2 * kBarFrames, 2 * kBarFrames + kBarFrames / 2,
                       "range leaves the next cycle first half dry");
    const uint32_t nextCycleWet = 2 * kBarFrames + 3 * kBarFrames / 4;
    require(ranged.out_l[nextCycleWet] == 100.0f && ranged.out_r[nextCycleWet] == -100.0f,
            "range preserves recording for the next cycle");

    const RunResult emptyRange = run_shuffle(1, 0.5f, 0.0f, 1.0f, true, 0.5f, 0.5f);
    require(emptyRange.out_l == emptyRange.in_l && emptyRange.out_r == emptyRange.in_r,
            "empty range is dry");
    const RunResult reversedRange = run_shuffle(1, 0.5f, 0.0f, 1.0f, true, 0.75f, 0.25f);
    require(reversedRange.out_l == reversedRange.in_l && reversedRange.out_r == reversedRange.in_r,
            "reversed range is dry");

    const RunResult two = run_shuffle(2, 0.5f, 0.0f);
    assert_dry_prefix(two, 2 * kBarFrames, "two bars capture is dry");
    require(two.out_l[2 * kBarFrames + 64] != two.in_l[2 * kBarFrames + 64] &&
            two.out_l[2 * kBarFrames + 64] >= 1.0f && two.out_l[2 * kBarFrames + 64] <= 2.0f,
            "two bars become wet only after full cycle");

    const RunResult eightEighth = run_shuffle(8, 0.5f, 0.0f);
    const RunResult eightSixteenth = run_shuffle(8, 1.0f, 0.0f);
    for (uint32_t i = 0; i < eightEighth.out_l.size(); ++i) {
        require(std::isfinite(eightEighth.out_l[i]) && std::isfinite(eightSixteenth.out_l[i]), "eight bar output finite");
        require(std::fabs(eightSixteenth.out_l[i] + eightSixteenth.out_r[i]) < 1.0e-5f, "stereo alignment");
    }
    const RunResult repeat = run_shuffle(2, 0.5f, 0.0f);
    const RunResult alternate = run_shuffle(2, 0.5f, 0.5f);
    require(two.out_l == repeat.out_l && two.out_r == repeat.out_r, "same seed deterministic");
    bool changed = false;
    for (uint32_t i = 2 * kBarFrames + 64; i < two.out_l.size(); ++i)
        if (two.out_l[i] != alternate.out_l[i]) { changed = true; break; }
    require(changed, "different seed changes permutation");

    const RunResult disabled = run_shuffle(1, 0.5f, 0.0f, 0.0f);
    const RunResult stopped = run_shuffle(1, 0.5f, 0.0f, 1.0f, false);
    require(disabled.out_l == disabled.in_l && stopped.out_l == stopped.in_l, "disabled or stopped is dry");
    return 0;
}
