/*
 * SPDX-License-Identifier: MIT
 *
 * Standalone Vulkan Trace Replayer CLI (tools/panthor_vk_replayer.cpp)
 *
 * Usage:
 *   ./panthor_vk_replayer <trace.panvkcap> [--icd <mali_aot_icd.x86_64.json>] [--dump-ppm <out.ppm>] [--verbose]
 */
#include "panthor_vk_capture_replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
   if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
      printf("Usage: %s <trace.panvkcap> [--icd <icd.json>] [--dump-ppm <out.ppm>] [--verbose]\n", argv[0]);
      return (argc < 2) ? 1 : 0;
   }

   const char *trace_path = nullptr;
   struct panvk_replay_options opts;
   memset(&opts, 0, sizeof(opts));
   opts.verbose = true;

   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--icd") == 0 && i + 1 < argc) {
         opts.icd_json_path = argv[++i];
      } else if (strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
         opts.dump_ppm_path = argv[++i];
      } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
         opts.verbose = true;
      } else if (argv[i][0] != '-') {
         trace_path = argv[i];
      }
   }

   if (!trace_path) {
      fprintf(stderr, "Error: missing <trace.panvkcap> path\n");
      return 1;
   }

   struct panvk_replay_summary sum;
   int rc = panthor_vk_replay_trace_file(trace_path, &opts, &sum);
   if (rc != 0) {
      fprintf(stderr, "[panthor_vk_replayer] Replay FAILED (rc=%d)\n", rc);
      return 1;
   }
   printf("[panthor_vk_replayer] SUCCESS: Replayed %llu records (%llu submits, %llu shaders, %llu mem snapshots)\n",
          static_cast<unsigned long long>(sum.total_records_replayed),
          static_cast<unsigned long long>(sum.total_queue_submits),
          static_cast<unsigned long long>(sum.total_shaders_created),
          static_cast<unsigned long long>(sum.total_memory_snapshots_restored));
   return 0;
}
