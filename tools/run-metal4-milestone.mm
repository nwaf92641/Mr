/* Mr — runs the Metal 4 milestone harness and reports where it stopped.
 *
 * The harness is stage-by-stage and already reports SKIP rather than claiming
 * success when Metal 4 is unavailable, so this only has to exist, run it, and
 * turn its stages into an exit status:
 *
 *   0  every stage reached
 *   2  a stage failed -- the report names it
 *   3  skipped: no Metal 4 device on this machine
 *
 * Exit 3 is not a pass. It is what the CI runner reports until it has a Metal 4
 * GPU, and it is deliberately distinct from 0 so a green job cannot be mistaken
 * for a drawn frame.
 */
#include <stdio.h>

#include "mr_metal4_milestone.h"

int main(void) {
  const int result = mr_mtl4_milestone_run(stdout, nullptr);
  fflush(stdout);
  if (result == 0) {
    fprintf(stderr, "metal4-milestone: all stages reached\n");
    return 0;
  }
  fprintf(stderr, "metal4-milestone: stopped early, see the stage table above\n");
  return result;
}
