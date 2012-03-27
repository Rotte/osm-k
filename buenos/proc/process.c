/*
 * Process startup.
 *
 * Copyright (C) 2003-2005 Juha Aatrokoski, Timo Lilja,
 *       Leena Salmela, Teemu Takanen, Aleksi Virtanen.
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
 * $Id: process.c,v 1.11 2007/03/07 18:12:00 ttakanen Exp $
 *
 */

#include "proc/process.h"
#include "proc/elf.h"
#include "kernel/thread.h"
#include "kernel/assert.h"
#include "kernel/interrupt.h"
#include "kernel/config.h"
#include "fs/vfs.h"
#include "drivers/yams.h"
#include "vm/vm.h"
#include "vm/pagepool.h"
#include "kernel/sleepq.h"


/** @name Process startup
 *
 * This module contains a function to start a userland process.
 */

process_table_t process_table[PROCESS_MAX_PROCESSES];

spinlock_t process_table_slock;

void process_reset(process_id_t pid)
{
    process_table[pid].state         = PROCESS_FREE;
    process_table[pid].executable[0] = 0;
    process_table[pid].retval        = 0;
    process_table[pid].cFiles        = 0;
}

/* Initialize process table and spinlock */
void process_init()
{
    int i;
    spinlock_reset(&process_table_slock);
    for (i = 0; i <= PROCESS_MAX_PROCESSES; ++i)
        process_reset(i);
}

/* Find a free slot in the process table. Returns PROCESS_MAX_PROCESSES
 * if the table is full. */
process_id_t alloc_process_id()
{
    int i;
    interrupt_status_t intr_status;

    intr_status = _interrupt_disable();
    spinlock_acquire(&process_table_slock);
    for (i = 0; i <= PROCESS_MAX_PROCESSES; ++i)
    {
        if (process_table[i].state == PROCESS_FREE)
        {
            process_table[i].state = PROCESS_RUNNING;
            break;
        }
    }
    spinlock_release(&process_table_slock);
    _interrupt_set_state(intr_status);
    return i;
}


/**
 * Starts one userland process. The thread calling this function will
 * be used to run the process and will therefore never return from
 * this function. This function asserts that no errors occur in
 * process startup (the executable file exists and is a valid ecoff
 * file, enough memory is available, file operations succeed...).
 * Therefore this function is not suitable to allow startup of
 * arbitrary processes.
 *
 * @executable The name of the executable to be run in the userland
 * process
 */
void process_start(process_id_t pid)
{
    thread_table_t *my_entry;
    pagetable_t *pagetable;
    uint32_t phys_page;
    context_t user_context;
    uint32_t stack_bottom;
    elf_info_t elf;
    openfile_t file;
    char *executable;

    int i;

    interrupt_status_t intr_status;

    my_entry = thread_get_current_thread_entry();
    my_entry->process_id = pid;
    executable = process_table[pid].executable;

    /* If the pagetable of this thread is not NULL, we are trying to
       run a userland process for a second time in the same thread.
       This is not possible. */
    KERNEL_ASSERT(my_entry->pagetable == NULL);

    pagetable = vm_create_pagetable(thread_get_current_thread());
    KERNEL_ASSERT(pagetable != NULL);

    intr_status = _interrupt_disable();
    my_entry->pagetable = pagetable;

    /* Update TLB to allow access to the virtual addresses of the
       segments. */
    _tlb_set_asid(thread_get_current_thread());

    _interrupt_set_state(intr_status);

    file = vfs_open((char *)executable);
    /* Make sure the file existed and was a valid ELF file */
    KERNEL_ASSERT(file >= 0);
    KERNEL_ASSERT(elf_parse_header(&elf, file));

    /* Trivial and naive sanity check for entry point: */
    KERNEL_ASSERT(elf.entry_point >= PAGE_SIZE);

    /* Calculate the number of pages needed by the whole process
       (including userland stack). Since we don't have proper tlb
       handling code, all these pages must fit into TLB. */
    KERNEL_ASSERT(elf.ro_pages + elf.rw_pages + CONFIG_USERLAND_STACK_SIZE
            <= _tlb_get_maxindex() + 1);

    /* Allocate and map stack */
    for(i = 0; i < CONFIG_USERLAND_STACK_SIZE; i++) {
        phys_page = pagepool_get_phys_page();
        KERNEL_ASSERT(phys_page != 0);
        vm_map(my_entry->pagetable, phys_page,
                (USERLAND_STACK_TOP & PAGE_SIZE_MASK) - i*PAGE_SIZE, 1);
    }

    /* Allocate and map pages for the segments. We assume that
       segments begin at page boundary. (The linker script in tests
       directory creates this kind of segments) */
    for(i = 0; i < (int)elf.ro_pages; i++) {
        phys_page = pagepool_get_phys_page();
        KERNEL_ASSERT(phys_page != 0);
        vm_map(my_entry->pagetable, phys_page,
                elf.ro_vaddr + i*PAGE_SIZE, 1);
    }

    for(i = 0; i < (int)elf.rw_pages; i++) {
        phys_page = pagepool_get_phys_page();
        KERNEL_ASSERT(phys_page != 0);
        vm_map(my_entry->pagetable, phys_page,
                elf.rw_vaddr + i*PAGE_SIZE, 1);
    }

    /* Zero the pages. */
    memoryset((void *)elf.ro_vaddr, 0, elf.ro_pages*PAGE_SIZE);
    memoryset((void *)elf.rw_vaddr, 0, elf.rw_pages*PAGE_SIZE);

    stack_bottom = (USERLAND_STACK_TOP & PAGE_SIZE_MASK) -
        (CONFIG_USERLAND_STACK_SIZE-1)*PAGE_SIZE;
    memoryset((void *)stack_bottom, 0, CONFIG_USERLAND_STACK_SIZE*PAGE_SIZE);

    /* Copy segments */

    if (elf.ro_size > 0) {
        /* Make sure that the segment is in proper place. */
        KERNEL_ASSERT(elf.ro_vaddr >= PAGE_SIZE);
        KERNEL_ASSERT(vfs_seek(file, elf.ro_location) == VFS_OK);
        KERNEL_ASSERT(vfs_read(file, (void *)elf.ro_vaddr, elf.ro_size)
                == (int)elf.ro_size);
    }

    if (elf.rw_size > 0) {
        /* Make sure that the segment is in proper place. */
        KERNEL_ASSERT(elf.rw_vaddr >= PAGE_SIZE);
        KERNEL_ASSERT(vfs_seek(file, elf.rw_location) == VFS_OK);
        KERNEL_ASSERT(vfs_read(file, (void *)elf.rw_vaddr, elf.rw_size)
                == (int)elf.rw_size);
    }


    /* Set the dirty bit to zero (read-only) on read-only pages. */
    for(i = 0; i < (int)elf.ro_pages; i++) {
        vm_set_dirty(my_entry->pagetable, elf.ro_vaddr + i*PAGE_SIZE, 0);
    }

    /* Initialize the user context. (Status register is handled by
       thread_goto_userland) */
    memoryset(&user_context, 0, sizeof(user_context));
    user_context.cpu_regs[MIPS_REGISTER_SP] = USERLAND_STACK_TOP;
    user_context.pc = elf.entry_point;

    thread_goto_userland(&user_context);

    KERNEL_PANIC("thread_goto_userland failed.");
}

process_id_t process_spawn(const char *executable)
{
    TID_t thread;
    process_id_t pid = alloc_process_id();

    if (pid == PROCESS_MAX_PROCESSES)
        return PROCESS_PTABLE_FULL;

    /* Remember to copy the executable name for use in process_start */
    stringcopy(process_table[pid].executable, executable, PROCESS_MAX_FILELENGTH);
    process_table[pid].parent = process_get_current_process();

    thread = thread_create((void (*)(uint32_t))(&process_start), pid);
    thread_run(thread);
    return pid;
}

process_id_t process_get_current_process(void)
{
    return thread_get_current_thread_entry()->process_id;
}

process_table_t *process_get_current_process_entry(void)
{
    return &process_table[process_get_current_process()];
}

int process_join(process_id_t pid)
{
    int retval;
    interrupt_status_t intr_status;

    /* Only join with legal pids */
    if (pid < 0 || pid >= PROCESS_MAX_PROCESSES ||
            process_table[pid].parent != process_get_current_process())
        return PROCESS_ILLEGAL_JOIN;

    intr_status = _interrupt_disable();
    spinlock_acquire(&process_table_slock);

    /* The thread could be zombie even though it wakes us (maybe). */
    while (process_table[pid].state != PROCESS_ZOMBIE)
    {
        sleepq_add(&process_table[pid]);
        spinlock_release(&process_table_slock);
        thread_switch();
        spinlock_acquire(&process_table_slock);
    }

    retval = process_table[pid].retval;
    process_reset(pid);

    spinlock_release(&process_table_slock);
    _interrupt_set_state(intr_status);
    return retval;
}

void process_finish(int retval)
{
    interrupt_status_t intr_status;
    process_id_t cur = process_get_current_process();
    thread_table_t *thread = thread_get_current_thread_entry();

    intr_status = _interrupt_disable();
    spinlock_acquire(&process_table_slock);

    process_table[cur].state  = PROCESS_ZOMBIE;
    process_table[cur].retval = retval;

    /* Remember to destroy the pagetable! */
    vm_destroy_pagetable(thread->pagetable);
    thread->pagetable = NULL;

    sleepq_wake_all(&process_table[cur]);

    spinlock_release(&process_table_slock);
    _interrupt_set_state(intr_status);
    thread_finish();
}

int process_add_file(openfile_t fd)
{
    interrupt_status_t intr_status;
    process_id_t pid = process_get_current_process();

    intr_status = _interrupt_disable();
    spinlock_acquire(&process_table_slock);

    if (pid < 0 || pid > PROCESS_MAX_PROCESSES ||
            process_table[pid].state != PROCESS_RUNNING ||
            process_table[pid].cFiles >= PROCESS_MAX_FILES)
    {
        spinlock_release(&process_table_slock);
        _interrupt_set_state(intr_status);
        return -1;
    }

    process_table[pid].files[process_table[pid].cFiles++] = fd;

    spinlock_release(&process_table_slock);
    _interrupt_set_state(intr_status);

    return 0;
}

int process_rem_file(openfile_t fd)
{
    interrupt_status_t intr_status;
    uint32_t i;
    process_id_t pid = process_get_current_process();
    process_table_t *p = &process_table[pid];

    if (pid < 0 || pid > PROCESS_MAX_PROCESSES ||
            p->state != PROCESS_RUNNING)
        return -1;

    intr_status = _interrupt_disable();
    spinlock_acquire(&process_table_slock);

    for (i = 0; i < p->cFiles; ++i)
        if (p->files[i] == fd)
            p->files[i] = p->files[--(p->cFiles)];

    spinlock_release(&process_table_slock);
    _interrupt_set_state(intr_status);

    return 0;
}

int process_check_file(openfile_t fd)
{
    interrupt_status_t intr_status;
    uint32_t i;
    int found = -1;
    process_id_t pid = process_get_current_process();
    process_table_t *p = &process_table[pid];

    if (pid < 0 || pid > PROCESS_MAX_PROCESSES ||
            p->state != PROCESS_RUNNING)
        return -1;

    intr_status = _interrupt_disable();
    spinlock_acquire(&process_table_slock);

    for (i = 0; i < p->cFiles; ++i)
        if (p->files[i] == fd)
            found = 0;

    spinlock_release(&process_table_slock);
    _interrupt_set_state(intr_status);
    return found;
}

/** @} */
