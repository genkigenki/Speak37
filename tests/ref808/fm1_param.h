/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* the host build of FM1 Open's 808: a parameter's name, range, default and choice names (as drum808.c fills them) */
#ifndef FM1_PARAM_H
#define FM1_PARAM_H
typedef struct { const char *name; int max, def; const char *const *names; } fm1_param_t;
#endif
