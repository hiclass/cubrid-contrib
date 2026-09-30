/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, CUBRID Corporation. All rights reserved.
 *
 * volmap_options.h - option code definitions, shared by the body and the standalone launcher
 */
#ifndef _VOLMAP_OPTIONS_H_
#define _VOLMAP_OPTIONS_H_

/* Self-contained option codes.
 * The tool must build from a CUBRID source checkout alone, and the tree changes
 * with re-clones and upstream merges, so it does not depend on VOLMAP_* macros
 * from utility.h.  Those are used only when the tool is registered in the tree
 * (cubrid volmap), in which case the #ifndef below steps aside.
 * The numeric codes (10850+) stay clear of getopt's single characters. */
#ifndef VOLMAP_OUTPUT_FILE_S
#define VOLMAP_OUTPUT_FILE_S  'o'
#define VOLMAP_OUTPUT_FILE_L  "output-file"
#endif
#ifndef VOLMAP_WIDTH_S
#define VOLMAP_WIDTH_S        'w'
#define VOLMAP_WIDTH_L        "width"
#endif
#ifndef VOLMAP_ROWS_S
#define VOLMAP_ROWS_S         'r'
#define VOLMAP_ROWS_L         "rows"
#endif
#ifndef VOLMAP_VOLUME_S
#define VOLMAP_VOLUME_S       'V'
#define VOLMAP_VOLUME_L       "volume"
#endif
#ifndef VOLMAP_FULL_S
#define VOLMAP_FULL_S         'f'
#define VOLMAP_FULL_L         "full"
#endif
#ifndef VOLMAP_INTERACTIVE_S
#define VOLMAP_INTERACTIVE_S  'i'
#define VOLMAP_INTERACTIVE_L  "interactive"
#endif
#ifndef VOLMAP_RESIDENCY_S
#define VOLMAP_RESIDENCY_S    'm'
#define VOLMAP_RESIDENCY_L    "residency"
#endif
#ifndef VOLMAP_BUFMAP_S
#define VOLMAP_BUFMAP_S       'B'
#define VOLMAP_BUFMAP_L       "bufmap"
#endif
#ifndef VOLMAP_DEEP_S
#define VOLMAP_DEEP_S         10850
#define VOLMAP_DEEP_L         "deep"
#endif
#ifndef VOLMAP_FULL_SWEEP_S
#define VOLMAP_FULL_SWEEP_S   10851
#define VOLMAP_FULL_SWEEP_L   "full-sweep"
#endif
#ifndef VOLMAP_NO_OVERLAY_S
#define VOLMAP_NO_OVERLAY_S   10852
#define VOLMAP_NO_OVERLAY_L   "no-overlay"
#endif
#ifndef VOLMAP_PLAIN_S
#define VOLMAP_PLAIN_S        10853
#define VOLMAP_PLAIN_L        "plain"
#endif
#ifndef VOLMAP_WIDE_S
#define VOLMAP_WIDE_S         10854
#define VOLMAP_WIDE_L         "wide"
#endif
#ifndef VOLMAP_CHECK_S
#define VOLMAP_CHECK_S        10855
#define VOLMAP_CHECK_L        "check"
#endif
#ifndef VOLMAP_FORMAT_S
#define VOLMAP_FORMAT_S       10856
#define VOLMAP_FORMAT_L       "format"
#endif
#ifndef VOLMAP_TICK_S
#define VOLMAP_TICK_S         10857
#define VOLMAP_TICK_L         "tick"
#endif
#ifndef VOLMAP_WARN_IDLE_S
#define VOLMAP_WARN_IDLE_S    10858
#define VOLMAP_WARN_IDLE_L    "warn-idle"
#endif

#endif /* _VOLMAP_OPTIONS_H_ */
