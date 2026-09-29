/* Copyright (C) 2025 EchoStretch
   Copyright (C) 2026 slopmaster33

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#ifndef DUMP_JOB_H
#define DUMP_JOB_H

#include <stdint.h>
#include <time.h>

#include "utils.h"

typedef enum {
    JOB_IDLE = 0,
    JOB_PREPARING,
    JOB_RUNNING,
    JOB_DONE,
    JOB_FAILED,
    JOB_ABORTED
} job_state_t;

typedef struct {
    job_state_t state;
    char        app_dir[128];
    char        title[128];
    char        title_id[16];
    char        dest[384];        /* folder the dump is written to     */
    char        stage[64];        /* coarse phase, e.g. "Copying"      */
    char        current_file[256];
    char        message[256];     /* result or error description       */
    uint64_t    total_bytes;
    uint64_t    copied_bytes;
    time_t      started;
    time_t      copy_started;     /* when bytes began to move, 0 before that */
    time_t      finished;
} job_status_t;

#define JOB_ERR_EXISTS (-2)

/* Where a dump to this drive goes: the mount plus the configured folder. */
void job_dest_path(const char *mount, const dumper_config_t *cfg, char *out, size_t out_size);

/* Starts a dump in the background. Returns 0 on success and -1 when a job is
   already running or the arguments do not resolve, with the reason placed
   in err (when given).

   A dump of this title that was cut short is cleared away first - two
   halves in one folder are worth nothing. A finished one, or a folder of
   unknown origin, is only replaced with overwrite set; without it the call
   returns JOB_ERR_EXISTS and touches nothing. */
int  job_start(const char *app_dir, const char *mount,
               const dumper_config_t *cfg, int overwrite,
               char *err, size_t err_size);

/* Asks the running job to stop. The copy routines look for this between
   blocks, so it takes effect within moments even inside a huge file. */
void job_abort(void);

/* Snapshot of the current job, safe to call from any thread. */
void job_get_status(job_status_t *out);

int  job_is_active(void);

const char *job_state_name(job_state_t state);

#endif /* DUMP_JOB_H */
