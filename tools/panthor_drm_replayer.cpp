/*
 * SPDX-License-Identifier: MIT
 *
 * Standalone Panthor DRM / CSF / Valhall ISA Trace Replayer & Inspector CLI
 * (tools/panthor_drm_replayer.cpp)
 *
 * Usage:
 *   ./panthor_drm_replayer <trace.pandrmcap> [--verbose] [--dump-csf] [--bug-catching-mode]
 */
#include "panthor_drm_capture_replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
   if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
      printf("Usage: %s <trace.pandrmcap> [--verbose] [--dump-csf] [--bug-catching-mode]\n", argv[0]);
      return (argc < 2) ? 1 : 0;
   }

   const char *trace_path = nullptr;
   struct pandrm_replay_options opts;
   memset(&opts, 0, sizeof(opts));
   opts.verbose = true;
   opts.verify_invariants = true;

   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
         opts.verbose = true;
      } else if (strcmp(argv[i], "--dump-csf") == 0) {
         opts.dump_csf = true;
      } else if (strcmp(argv[i], "--bug-catching-mode") == 0) {
         opts.bug_catching_mode = true;
      } else if (argv[i][0] != '-') {
         trace_path = argv[i];
      }
   }

   if (!trace_path) {
      fprintf(stderr, "Error: missing <trace.pandrmcap> path\n");
      return 1;
   }

   struct pandrm_replay_summary sum;
   int rc = panthor_drm_replay_trace_file(trace_path, &opts, &sum);
   if (rc != 0) {
      fprintf(stderr, "[panthor_drm_replayer] Replay FAILED (rc=%d, violations=%u)\n",
              rc, sum.csf_invariant_violations);
      return 1;
   }
   printf("[panthor_drm_replayer] SUCCESS: Replayed %llu records (%llu submits, %llu CSF chunks, 0 violations)\n",
          static_cast<unsigned long long>(sum.total_records_replayed),
          static_cast<unsigned long long>(sum.total_group_submits),
          static_cast<unsigned long long>(sum.total_csf_chunks_verified));
   return 0;
}
