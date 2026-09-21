/*
 * Measurement.
 *
 * The brief is explicit that an optimisation must not be added because it sounds
 * good, and that the runtime must be able to say where the time went. This is the
 * machinery for that. Each layer reports into a named ring, so a slow frame is
 * attributed to the layer that spent it rather than being lumped into one number.
 *
 * Four design choices:
 *
 *   Rings, not aggregates. A running mean hides exactly what matters in a
 *   frame-time problem, which is the tail.
 *
 *   Percentiles, not maxima. One hitch from the OS paging in a shader is not a
 *   performance characteristic, and quoting it as one sends the work in the
 *   wrong direction. Both are reported; the percentile is the one to act on.
 *
 *   Spans. A named interval is opened and closed around a layer's work, so the
 *   call site does not have to know the clock.
 *
 *   Free to leave on. Recording is a store and an index bump into a fixed array:
 *   no allocation, no lock, no I/O. A measurement harness you have to enable is
 *   a measurement harness that is not running when the problem happens, so the
 *   default is on and the cost is a few hundred kilobytes.
 */
#ifndef MR_TELEMETRY_H
#define MR_TELEMETRY_H

#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Samples retained per measurement. At 60 Hz this is about 34 seconds of history,
 * which is long enough to cover a shader-heavy area and short enough that the
 * whole recorder stays inside a few hundred kilobytes. */
#define MR_TELEMETRY_RING 2048
#define MR_TELEMETRY_MAX_COUNTERS 32
#define MR_TELEMETRY_MAX_SPANS 32

/* Monotonic nanoseconds. */
uint64_t mr_tick(void);
uint64_t mr_ticks_per_second(void);
/*
 * Prefers CLOCK_MONOTONIC_RAW where the platform has it, which is not slewed by
 * NTP, so a clock step mid-capture cannot appear as a negative frame time.
 * Falls back to mr_tick where it does not.
 */
uint64_t mr_tick_raw(void);

/*
 * The layers worth separating, because they are the ones that can be fixed
 * independently. The enum exists so the report carries the breakdown the brief
 * asks for from the first run; further counters can be added by name at any time.
 */
typedef enum {
  MR_LAYER_FRAME = 0,      /* whole frame, the denominator */
  MR_LAYER_FEX_JIT,        /* translating guest blocks */
  MR_LAYER_FEX_LOOKUP,     /* code-cache lookups */
  MR_LAYER_WINE_SYSCALL,   /* guest API call into the Unix layer */
  MR_LAYER_WINE_SYNC,      /* wait primitives, critical sections */
  MR_LAYER_DXMT_RECORD,    /* D3D11 command recording */
  MR_LAYER_DXMT_TRANSLATE, /* DXBC -> Metal IR */
  MR_LAYER_DXMT_BIND,      /* state and resource binding */
  MR_LAYER_METAL_ENCODE,   /* Metal encoder work */
  MR_LAYER_METAL_SUBMIT,   /* commit and scheduling */
  MR_LAYER_GPU_WAIT,       /* CPU blocked on the GPU */
  MR_LAYER_SHADER_COMPILE, /* pipeline compilation: the stutter source */
  MR_LAYER_PRESENT,        /* drawable acquisition and presentation */
  MR_LAYER_AUDIO,
  MR_LAYER_INPUT,
  MR_LAYER_COUNT,
} mr_layer;

const char *mr_layer_str(mr_layer layer);

typedef struct {
  char name[48];
  char unit[16];
  double samples[MR_TELEMETRY_RING];
  uint64_t count; /* total recorded, including samples that overwrote older ones */
  uint64_t over_budget;
  double total;
  double total_sq;
  double min;
  double max;
} mr_counter;

typedef struct {
  char name[48];
  int open_depth;
  uint64_t open_since;
} mr_span;

typedef struct {
  mr_counter counters[MR_TELEMETRY_MAX_COUNTERS];
  size_t counter_count;
  mr_span spans[MR_TELEMETRY_MAX_SPANS];
  size_t span_count;
  mr_strvec span_names;
  uint64_t frame_budget_ns;
} mr_telemetry;

/*
 * `frame_budget_ns` is the target frame time; 0 disables the over-budget count.
 * At 120 Hz that is 8333333. Nanoseconds rather than frames keeps the comparison
 * exact and avoids a division per sample.
 */
mr_status mr_telemetry_init(mr_telemetry *t, uint64_t frame_budget_ns);
void mr_telemetry_destroy(mr_telemetry *t);

/*
 * Returns the index of a measurement, creating it on first use, or -1 when the
 * table is full. The index is stable for the lifetime of the recorder, so callers
 * cache it instead of looking it up per sample.
 */
int mr_telemetry_counter(mr_telemetry *t, const char *name, const char *unit);

void mr_telemetry_record(mr_telemetry *t, int counter, double value);
void mr_telemetry_inc(mr_telemetry *t, int counter, double delta);

/* Opens a span. The returned index feeds begin/end; -1 on failure. */
int mr_telemetry_span(mr_telemetry *t, const char *name);
void mr_telemetry_span_begin(mr_telemetry *t, int span);
void mr_telemetry_span_end(mr_telemetry *t, int span);
double mr_telemetry_span_last_ns(const mr_telemetry *t, int span);

/*
 * The report: one row per measurement with samples, mean, p50, p99 and max.
 * Returns MR_ERR_INVALID for a NULL argument, MR_ERR_NOMEM if `out` could not be
 * grown.
 */
mr_status mr_telemetry_report(const mr_telemetry *t, mr_str *out);
bool mr_telemetry_write_report(const mr_telemetry *t, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MR_TELEMETRY_H */
