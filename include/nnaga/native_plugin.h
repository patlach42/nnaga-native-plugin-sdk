#ifndef NNAGA_NATIVE_PLUGIN_H
#define NNAGA_NATIVE_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
#define NNAGA_NATIVE_NOEXCEPT noexcept
extern "C" {
#else
#define NNAGA_NATIVE_NOEXCEPT
#endif

#if defined(_WIN32)
#define NNAGA_NATIVE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define NNAGA_NATIVE_EXPORT __attribute__((visibility("default")))
#else
#define NNAGA_NATIVE_EXPORT
#endif

#define NNAGA_NATIVE_ABI_VERSION 1u
#define NNAGA_NATIVE_MAX_PARAMETERS 256u

typedef void* NnagaPluginHandle;

typedef struct NnagaScalePointV1 {
    uint32_t struct_size;
    float normalized_value;
    const char* label;
} NnagaScalePointV1;

enum {
    NNAGA_PARAMETER_ENUM = 1u << 0,
    NNAGA_PARAMETER_TOGGLE = 1u << 1,
};

typedef struct NnagaParameterV1 {
    uint32_t struct_size;
    uint32_t port_index;
    const char* name;
    const char* symbol;
    const char* unit;
    uint32_t flags;
    float default_normalized;
    uint32_t scale_point_count;
    const NnagaScalePointV1* scale_points;
} NnagaParameterV1;

typedef struct NnagaProcessContextV1 {
    uint32_t struct_size;
    uint64_t sample_position;
    uint64_t transport_frame;
    uint64_t loop_end_frame;
    double sample_rate;
    double beats_per_minute;
    uint8_t playing;
    uint8_t looping;
    uint16_t reserved0;
    double beat_position;
    int64_t bar;
    double bar_beat;
    double musical_quarter_notes;
    float beats_per_bar;
    int32_t beat_unit;
} NnagaProcessContextV1;

typedef NnagaPluginHandle (*NnagaCreateFnV1)(void) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaDestroyFnV1)(NnagaPluginHandle handle) NNAGA_NATIVE_NOEXCEPT;
typedef int32_t (*NnagaActivateFnV1)(NnagaPluginHandle handle, double sample_rate,
                                      uint32_t max_frames) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaDeactivateFnV1)(NnagaPluginHandle handle) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaResetFnV1)(NnagaPluginHandle handle) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaSetParameterFnV1)(NnagaPluginHandle handle, uint32_t port_index,
                                        float normalized) NNAGA_NATIVE_NOEXCEPT;
typedef uint32_t (*NnagaFormatParameterFnV1)(NnagaPluginHandle handle, uint32_t port_index,
                                               float normalized, char* output,
                                               uint32_t output_capacity) NNAGA_NATIVE_NOEXCEPT;
typedef uint32_t (*NnagaLatencyFramesFnV1)(NnagaPluginHandle handle) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaProcessFnV1)(NnagaPluginHandle handle, const float* input_left,
                                  const float* input_right, float* output_left,
                                  float* output_right, uint32_t frames,
                                  const NnagaProcessContextV1* context) NNAGA_NATIVE_NOEXCEPT;

typedef struct NnagaPluginDescriptorV1 {
    uint32_t struct_size;
    const char* id;
    const char* name;
    const char* vendor;
    const char* version;
    uint32_t audio_inputs;
    uint32_t audio_outputs;
    uint32_t parameter_count;
    const NnagaParameterV1* parameters;
    NnagaCreateFnV1 create;
    NnagaDestroyFnV1 destroy;
    NnagaActivateFnV1 activate;
    NnagaDeactivateFnV1 deactivate;
    NnagaResetFnV1 reset;
    NnagaSetParameterFnV1 set_parameter;
    NnagaFormatParameterFnV1 format_parameter;
    NnagaLatencyFramesFnV1 latency_frames;
    NnagaProcessFnV1 process;
} NnagaPluginDescriptorV1;

typedef const NnagaPluginDescriptorV1* (*NnagaGetPluginFnV1)(uint32_t index) NNAGA_NATIVE_NOEXCEPT;

typedef struct NnagaPluginLibraryV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t plugin_count;
    NnagaGetPluginFnV1 get_plugin;
} NnagaPluginLibraryV1;

NNAGA_NATIVE_EXPORT const NnagaPluginLibraryV1*
nnaga_plugin_entry(uint32_t host_abi_version) NNAGA_NATIVE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
