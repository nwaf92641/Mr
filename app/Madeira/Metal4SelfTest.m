/* Mr — Metal 4 self test, run on the device from inside the app.
 *
 * The harness takes a FILE * and prints one row per stage. Rather than reinvent
 * a sink, this gives it a temporary file and reads the rows back, so the table
 * the app logs is exactly the table graphics/metal4/mr_metal4_milestone.mm
 * produced -- PASS, FAIL or SKIP with its own wording. Nothing is rewritten,
 * reworded or summarised in between.
 *
 * iOS has no stdout worth reading for a GUI app, which is why the file exists
 * at all; a string is also what the Swift side can put in LogStore.
 */
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Metal4SelfTest.h"
#include "mr_metal4_milestone.h"

static char *g_report;

const char *madeira_metal4_selftest(void) {
  if (g_report != NULL) {
    free(g_report);
    g_report = NULL;
  }

  FILE *capture = tmpfile();
  if (capture == NULL) {
    return "metal4 self test: could not open a capture file";
  }

  /* The same entry point the macOS runner calls, with no drawable: there is no
   * window at this point, and a nil drawable is what the harness already
   * handles when it has no presentable surface. */
  (void)mr_mtl4_milestone_run(capture, NULL);
  fflush(capture);
  rewind(capture);

  size_t capacity = 4096;
  size_t used = 0;
  g_report = (char *)malloc(capacity);
  if (g_report == NULL) {
    fclose(capture);
    return "metal4 self test: out of memory";
  }

  size_t read_count = 0;
  while ((read_count = fread(g_report + used, 1, capacity - used - 1, capture)) > 0) {
    used += read_count;
    if (used + 1 >= capacity) {
      capacity *= 2;
      char *grown = (char *)realloc(g_report, capacity);
      if (grown == NULL) {
        free(g_report);
        g_report = NULL;
        fclose(capture);
        return "metal4 self test: out of memory while reading the table";
      }
      g_report = grown;
    }
  }
  g_report[used] = '\0';
  fclose(capture);

  /* An empty table is itself a result, and a silent one would read as a pass. */
  if (used == 0) {
    free(g_report);
    g_report = NULL;
    return "metal4 self test: the harness produced no rows at all";
  }
  return g_report;
}
