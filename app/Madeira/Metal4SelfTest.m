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

/* Opt-in, checked here rather than from Swift.
 *
 * The first attempt put the trigger in Swift and referenced LogStore and this C
 * symbol from another file, which scripts/typecheck-app.sh rejected: it compiles
 * each Swift file on its own, so a file may only use what it defines. Marking the
 * symbol @_silgen_name or reaching for dlsym would have worked around the gate
 * rather than respected it, and the Swift side was never the point -- the stage
 * table is.
 *
 * So the whole path lives in this file: the check, the run, and the output. A
 * constructor runs it at load when explicitly asked, which needs no call site in
 * any other file, and does nothing otherwise -- creating a Metal 4 device on
 * every launch is how a diagnostic becomes a game launch bug.
 *
 * Two triggers, neither set by default:
 *   --metal4-selftest                        launch argument
 *   MadeiraMetal4SelfTest                    user default, bool
 *
 * The table goes to NSLog, which is what reaches a device log and Console.app,
 * and madeira_metal4_selftest() stays available for a UI to call later. */
static bool mr_metal4_selftest_requested(void) {
  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:@"MadeiraMetal4SelfTest"]) {
    return true;
  }
  for (NSString *argument in [[NSProcessInfo processInfo] arguments]) {
    if ([argument isEqualToString:@"--metal4-selftest"]) {
      return true;
    }
  }
  return false;
}

__attribute__((constructor)) static void mr_metal4_selftest_autorun(void) {
  @autoreleasepool {
    if (!mr_metal4_selftest_requested()) {
      return;
    }
    const char *table = madeira_metal4_selftest();
    NSLog(@"[metal4] self test");
    if (table == NULL) {
      NSLog(@"[metal4] no table returned");
      return;
    }
    NSString *text = [NSString stringWithUTF8String:table];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
      if ([line length] > 0) {
        NSLog(@"[metal4] %@", line);
      }
    }
    NSLog(@"[metal4] self test done");
  }
}
