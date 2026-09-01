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

#define NNAGA_NATIVE_ABI_VERSION 2u
#define NNAGA_NATIVE_MAX_PARAMETERS 256u
#define NNAGA_NATIVE_MAX_FRAMES 8192u

typedef enum NnagaRealtimeClassV2 {
    NNAGA_REALTIME_CERTIFIED_IN_PROCESS = 0,
    NNAGA_REALTIME_ISOLATED = 1,
    NNAGA_REALTIME_UNSUPPORTED = 2,
} NnagaRealtimeClassV2;

typedef void* NnagaPluginHandle;

typedef struct NnagaScalePointV2 { uint32_t struct_size; float normalized_value; const char* label; } NnagaScalePointV2;
enum { NNAGA_PARAMETER_ENUM = 1u << 0, NNAGA_PARAMETER_TOGGLE = 1u << 1 };
typedef struct NnagaParameterV2 {
    uint32_t struct_size; uint32_t port_index; const char* name; const char* symbol; const char* unit;
    uint32_t flags; float default_normalized; uint32_t scale_point_count; const NnagaScalePointV2* scale_points; uint32_t step_count;
} NnagaParameterV2;
typedef struct NnagaProcessContextV2 {
    uint32_t struct_size; uint64_t sample_position; uint64_t transport_frame; uint64_t loop_end_frame;
    double sample_rate; double beats_per_minute; uint8_t playing; uint8_t looping; uint16_t reserved0;
    double beat_position; int64_t bar; double bar_beat; double musical_quarter_notes; float beats_per_bar; int32_t beat_unit;
} NnagaProcessContextV2;
typedef void* (*NnagaCreateFnV2)(void) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaDestroyFnV2)(NnagaPluginHandle) NNAGA_NATIVE_NOEXCEPT;
typedef int32_t (*NnagaActivateFnV2)(NnagaPluginHandle, double sample_rate, uint32_t max_frames) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaDeactivateFnV2)(NnagaPluginHandle) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaResetFnV2)(NnagaPluginHandle) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaSetParameterFnV2)(NnagaPluginHandle, uint32_t, float) NNAGA_NATIVE_NOEXCEPT;
typedef uint32_t (*NnagaFormatParameterFnV2)(NnagaPluginHandle, uint32_t, float, char*, uint32_t) NNAGA_NATIVE_NOEXCEPT;
typedef uint32_t (*NnagaLatencyFramesFnV2)(NnagaPluginHandle) NNAGA_NATIVE_NOEXCEPT;
typedef void (*NnagaProcessFnV2)(NnagaPluginHandle, const float*, const float*, float*, float*, uint32_t, const NnagaProcessContextV2*) NNAGA_NATIVE_NOEXCEPT;
typedef struct NnagaPluginDescriptorV2 {
    uint32_t struct_size; const char* id; const char* alias; const char* name; const char* vendor; const char* version;
    uint32_t audio_inputs; uint32_t audio_outputs; uint32_t parameter_count; uint32_t max_frames; NnagaRealtimeClassV2 realtime_class;
    const NnagaParameterV2* parameters; NnagaCreateFnV2 create; NnagaDestroyFnV2 destroy; NnagaActivateFnV2 activate;
    NnagaDeactivateFnV2 deactivate; NnagaResetFnV2 reset; NnagaSetParameterFnV2 set_parameter; NnagaFormatParameterFnV2 format_parameter;
    NnagaLatencyFramesFnV2 latency_frames; NnagaProcessFnV2 process;
} NnagaPluginDescriptorV2;
typedef const NnagaPluginDescriptorV2* (*NnagaGetPluginFnV2)(uint32_t) NNAGA_NATIVE_NOEXCEPT;
typedef struct NnagaPluginLibraryV2 { uint32_t struct_size; uint32_t abi_version; uint32_t plugin_count; NnagaGetPluginFnV2 get_plugin; } NnagaPluginLibraryV2;
NNAGA_NATIVE_EXPORT const NnagaPluginLibraryV2* nnaga_plugin_entry(uint32_t host_abi_version) NNAGA_NATIVE_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
