/*
 * Process startup.
 *
 * Copyright (C) 2003 Juha Aatrokoski, Timo Lilja,
 *   Leena Salmela, Teemu Takanen, Aleksi Virtanen.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $Id: process.h,v 1.4 2003/05/16 10:13:55 ttakanen Exp $
 *
 */

#ifndef BUENOS_PROC_PROCESS
#define BUENOS_PROC_PROCESS

#include "lib/types.h"

#define USERLAND_STACK_TOP 0x7fffeffc

#define PROCESS_PTABLE_FULL  -1
#define PROCESS_ILLEGAL_JOIN -2

#define PROCESS_MAX_FILELENGTH 256
#define PROCESS_MAX_PROCESSES  128
#define PROCESS_MAX_FILES      10

typedef int process_id_t;

typedef enum {
    PROCESS_FREE,
    PROCESS_RUNNING,
    PROCESS_ZOMBIE
} process_state_t;

typedef struct {
    char executable[PROCESS_MAX_FILELENGTH];
    process_state_t state;
    int retval;
    process_id_t parent;

    uint32_t cFiles;
    int files[PROCESS_MAX_FILES];
} process_table_t;

/* Initialize the process table */
void process_init();

/* Run process in a new thread. Returns the PID of the new process. */
process_id_t process_spawn(const char *executable);

process_id_t process_get_current_process(void);
process_table_t *process_get_current_process_entry(void);

/* Stop the process and the thread it runs in. Sets the return value as well */
void process_finish(int retval);

/* Wait for the given process to terminate, returning its return value. This
 * will also mark its process table entry as free.
 * Only works on child processes */
int process_join(process_id_t pid);

/* Add a file to the current process's file list. Returns negative value on
 * error. */
int process_add_file(int fd);

/* Remove a file from the current process's file list. Returns negative value
 * on error. */
int process_rem_file(int fd);

/* Check if a file is in the current process's file list. Returns 0 if it is. */
int process_check_file(int fd);

#endif
