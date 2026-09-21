/*
 * Telemetry.
 *
 * The brief's rule is that no optimisation is applied because it sounds good,
 * and that the runtime must be able to say which layer is costing what. This is
 * the mechanism that makes that possible: a named ring per measurement, and a
 * report that shows counts, means and tail percentiles side by side.
 *
 * Three design choices worth stating:
 *
 *   Rings, not aggregates. A running mean hides exactly the thing that matters
 *   in a frame-time problem, which is the tail. Keeping raw samples costs a
 *   fixed few hundred kilobytes and is what makes p99 available at all.
 *
 *   Percentiles, not maxima. A single hitch from the OS paging in a shader is
 *   not a performance characteristic, and quoting it as one sends the work in
 *   the wrong direction.
 *
 *   Spans, not just counters. The breakdown between translator, Wine, DXMT and
 *   the GPU driver is the whole point, and a span name is what carries it.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr/mr_telemetry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------ timesource */

uint64_t mr_tick(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t mr_ticks_per_second(void) { return 1000000000ull; }

/* Prefer CLOCK_MONOTONIC_RAW where it exists: it is not slewed by NTP, and a
 * clock step mid-capture would show up as a frame that took a negative time. */
uint64_t mr_tick_raw(void) {
#if defined(CLOCK_MONOTONIC_RAW)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
#endif
  return mr_tick();
}

/* ------------------------------------------------------------- recorder */

const char *mr_layer_str(mr_layer layer) {
  switch (layer) {
    case MR_LAYER_FRAME: return "frame_time";
    case MR_LAYER_FEX_JIT: return "fex_jit";
    case MR_LAYER_FEX_LOOKUP: return "fex_lookup";
    case MR_LAYER_WINE_SYSCALL: return "wine_syscall";
    case MR_LAYER_WINE_SYNC: return "wine_sync";
    case MR_LAYER_DXMT_RECORD: return "dxmt_record";
    case MR_LAYER_DXMT_TRANSLATE: return "dxmt_translate";
    case MR_LAYER_DXMT_BIND: return "dxmt_bind";
    case MR_LAYER_METAL_ENCODE: return "metal_encode";
    case MR_LAYER_METAL_SUBMIT: return "metal_submit";
    case MR_LAYER_GPU_WAIT: return "gpu_wait";
    case MR_LAYER_SHADER_COMPILE: return "shader_compile";
    case MR_LAYER_PRESENT: return "present";
    case MR_LAYER_AUDIO: return "audio";
    case MR_LAYER_INPUT: return "input";
    case MR_LAYER_COUNT: break;
  }
  return "unknown";
}

mr_status mr_telemetry_init(mr_telemetry *t, uint64_t frame_budget_ns) {
  if (t == NULL) return MR_ERR_INVALID;
  memset(t, 0, sizeof(*t));
  t->frame_budget_ns = frame_budget_ns;

  mr_status st = mr_strvec_init(&t->span_names);
  if (st != MR_OK) return st;

  /*
   * The layer counters are created up front, in enum order, so a layer's enum
   * value is also its counter index. That makes mr_telemetry_record(t,
   * MR_LAYER_FEX_JIT, ns) correct without a lookup, which matters because these
   * are the hottest recording sites in the runtime.
   */
  for (int i = 0; i < (int)MR_LAYER_COUNT; i++) {
    int index = mr_telemetry_counter(t, mr_layer_str((mr_layer)i), "ns");
    if (index != i) return MR_ERR_RANGE;
  }
  return MR_OK;
}

void mr_telemetry_destroy(mr_telemetry *t) {
  if (t == NULL) return;
  mr_strvec_free(&t->span_names);
  memset(t, 0, sizeof(*t));
}

/* ---------------------------------------------------------------- counters */

int mr_telemetry_counter(mr_telemetry *t, const char *name, const char *unit) {
  if (t == NULL || name == NULL) return -1;

  for (size_t i = 0; i < t->counter_count; i++) {
    if (strcmp(t->counters[i].name, name) == 0) return (int)i;
  }
  if (t->counter_count >= MR_TELEMETRY_MAX_COUNTERS) return -1;

  size_t index = t->counter_count++;
  snprintf(t->counters[index].name, sizeof(t->counters[index].name), "%s",
           name);
  snprintf(t->counters[index].unit, sizeof(t->counters[index].unit), "%s",
           unit != NULL ? unit : "");
  return (int)index;
}

void mr_telemetry_record(mr_telemetry *t, int counter, double value) {
  if (t == NULL || counter < 0) return;
  size_t index = (size_t)counter;
  if (index >= t->counter_count) return;

  mr_counter *c = &t->counters[index];
  c->samples[c->count % MR_TELEMETRY_RING] = value;
  c->count++;
  c->total += value;
  c->total_sq += value * value;
  if (c->count == 1 || value < c->min) c->min = value;
  if (c->count == 1 || value > c->max) c->max = value;
  if (t->frame_budget_ns != 0 && value > (double)t->frame_budget_ns) {
    c->over_budget++;
  }
}

void mr_telemetry_inc(mr_telemetry *t, int counter, double delta) {
  if (t == NULL) return;
  mr_telemetry_record(t, counter, delta);
}

/* ------------------------------------------------------------------ spans */

int mr_telemetry_span(mr_telemetry *t, const char *name) {
  if (t == NULL || name == NULL) return -1;

  for (size_t i = 0; i < t->span_names.count; i++) {
    if (strcmp(t->span_names.items[i], name) == 0) return (int)i;
  }
  if (t->span_count >= MR_TELEMETRY_MAX_SPANS) return -1;

  if (mr_strvec_push(&t->span_names, name) != MR_OK) return -1;

  size_t index = t->span_count++;
  snprintf(t->spans[index].name, sizeof(t->spans[index].name), "%s", name);
  t->spans[index].open_depth = 0;
  t->spans[index].open_since = 0;
  return (int)index;
}

void mr_telemetry_span_begin(mr_telemetry *t, int span) {
  if (t == NULL || span < 0) return;
  size_t index = (size_t)span;
  if (index >= t->span_count) return;

  mr_span *s = &t->spans[index];
  /*
   * Only the outermost begin records a timestamp. A span begun twice without an
   * end -- which happens when a caller returns early through an error path --
   * must not overwrite the start time, or the measured duration becomes the
   * inner fragment and the outer cost disappears.
   */
  if (s->open_depth == 0) s->open_since = mr_tick();
  s->open_depth++;
}

void mr_telemetry_span_end(mr_telemetry *t, int span) {
  if (t == NULL || span < 0) return;
  size_t index = (size_t)span;
  if (index >= t->span_count) return;

  mr_span *s = &t->spans[index];
  if (s->open_depth == 0) return; /* an end without a begin is ignored */

  s->open_depth--;
  if (s->open_depth != 0) return;

  uint64_t now = mr_tick();
  if (now < s->open_since) return; /* a clock step: drop the sample */

  int counter = mr_telemetry_counter(t, s->name, "ns");
  if (counter >= 0) {
    mr_telemetry_record(t, counter, (double)(now - s->open_since));
  }
  s->open_since = 0;
}

double mr_telemetry_span_last_ns(const mr_telemetry *t, int span) {
  if (t == NULL || span < 0) return 0.0;
  size_t index = (size_t)span;
  if (index >= t->span_count) return 0.0;

  const char *name = t->spans[index].name;
  for (size_t i = 0; i < t->counter_count; i++) {
    if (strcmp(t->counters[i].name, name) != 0) continue;
    const mr_counter *c = &t->counters[i];
    if (c->count == 0) return 0.0;
    /* The most recently written sample in the ring. */
    size_t last = (c->count - 1) % MR_TELEMETRY_RING;
    return c->samples[last];
  }
  return 0.0;
}

/* --------------------------------------------------------------- reporting */

static int mr_compare_double(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  if (x < y) return -1;
  if (x > y) return 1;
  return 0;
}

/* Nearest-rank percentile over the ring, with the samples copied so the caller's
 * data is not reordered. Nearest-rank rather than interpolated because the
 * interpolation invents a value between two observed frame times, and the
 * report is read as "at worst, one frame in a hundred took this long". */
static double mr_percentile(const mr_counter *c, double fraction) {
  size_t n = c->count < MR_TELEMETRY_RING ? c->count : MR_TELEMETRY_RING;
  if (n == 0) return 0.0;

  double *scratch = (double *)malloc(n * sizeof(*scratch));
  if (scratch == NULL) return 0.0;
  memcpy(scratch, c->samples, n * sizeof(*scratch));
  qsort(scratch, n, sizeof(*scratch), mr_compare_double);

  size_t rank = (size_t)(fraction * (double)n);
  if (rank >= n) rank = n - 1;
  double value = scratch[rank];
  free(scratch);
  return value;
}

static mr_status mr_append_result(mr_str *out, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buf[512];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return mr_str_appendz(out, buf);
}

mr_status mr_telemetry_report(const mr_telemetry *t, mr_str *out) {
  if (t == NULL || out == NULL) return MR_ERR_INVALID;

  mr_status st = MR_OK;
  if (out->data == NULL) st = mr_str_init(out);
  if (st != MR_OK) return st;
  mr_str_clear(out);

  st = mr_append_result(
      out, "%-22s %10s %12s %12s %12s %12s\n", "measurement", "samples", "mean",
      "p50", "p99", "max");
  if (st != MR_OK) return st;

  for (size_t i = 0; i < t->counter_count; i++) {
    const mr_counter *c = &t->counters[i];
    if (c->count == 0) continue;

    double mean = c->total / (double)c->count;
    st = mr_append_result(out, "%-22s %10llu %12.3f %12.3f %12.3f %12.3f",
                          c->name, (unsigned long long)c->count, mean,
                          mr_percentile(c, 0.50), mr_percentile(c, 0.99),
                          c->max);
    if (st != MR_OK) return st;
    if (c->unit[0] != '\0') {
      st = mr_append_result(out, " %s", c->unit);
      if (st != MR_OK) return st;
    }
    /* The budget column is only meaningful for the frame series; printing it
     * against a shader-compile count would invite a wrong conclusion. */
    if (t->frame_budget_ns != 0 &&
        strcmp(c->name, "frame_time") == 0) {
      st = mr_append_result(out, "  (%llu over the %llu us budget)\n",
                            (unsigned long long)c->over_budget,
                            (unsigned long long)(t->frame_budget_ns / 1000ull));
    } else if (t->frame_budget_ns != 0 && c->over_budget > 0) {
      st = mr_append_result(out, "  (%llu over budget)\n",
                            (unsigned long long)c->over_budget);
    } else {
      st = mr_append_result(out, "\n");
    }
    if (st != MR_OK) return st;
  }

  return st;
}

bool mr_telemetry_write_report(const mr_telemetry *t, const char *path) {
  if (t == NULL || path == NULL) return false;

  mr_str report;
  if (mr_telemetry_report(t, &report) != MR_OK) return false;

  bool ok = mr_write_file_atomic(path, report.data, report.len) == MR_OK;
  mr_str_free(&report);
  return ok;
}
