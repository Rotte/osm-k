/*
 * System calls.
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
 * $Id: syscall.c,v 1.3 2004/01/13 11:10:05 ttakanen Exp $
 *
 */
#include "kernel/cswitch.h"
#include "proc/syscall.h"
#include "kernel/halt.h"
#include "kernel/panic.h"
#include "lib/libc.h"
#include "kernel/assert.h"
#include "proc/process.h"
#include "drivers/device.h"
#include "drivers/gcd.h"
#include "fs/vfs.h"

void syscall_exit(int retval)
{
    process_finish(retval);
}

int syscall_write(uint32_t fd, char *s, int len)
{
    gcd_t *gcd;
    device_t *dev;
    if (fd == FILEHANDLE_STDOUT || fd == FILEHANDLE_STDERR)
    {
        dev = device_get(YAMS_TYPECODE_TTY, 0);
        gcd = (gcd_t *)dev->generic_device;
        return gcd->write(gcd, s, len);
    }
    else if (fd > 2 && process_check_file(fd) == 0)
        return vfs_write(fd - 3, s, len);
    return VFS_NOT_OPEN;
}

int syscall_read(uint32_t fd, char *s, int len)
{
    gcd_t *gcd;
    device_t *dev;
    if (fd == FILEHANDLE_STDIN)
    {
        dev = device_get(YAMS_TYPECODE_TTY, 0);
        gcd = (gcd_t *)dev->generic_device;
        return gcd->read(gcd, s, len);
    }
    else if (fd > 2 && process_check_file(fd) == 0)
        return vfs_read(fd - 3, s, len);
    return VFS_NOT_OPEN;
}

int syscall_join(process_id_t pid)
{
    return process_join(pid);
}

process_id_t syscall_exec(const char *filename)
{
    return process_spawn(filename);
}

int syscall_open(char *filename)
{
    openfile_t fd = vfs_open(filename);
    if (fd >= 0)
    {
        if (process_add_file(fd+3) < 0)
        {
            vfs_close(fd - 3);
            return -1;
        }
        return fd + 3;
    }
    return fd;
}

int syscall_close(int fd)
{
    if (fd > 2)
    {
        if (process_rem_file(fd) < 0)
            return -1;
        return vfs_close(fd - 3);
    }
    return -10;
}

int syscall_create(char *filename, int size)
{
    if (size < 0)
        return -1;
    return vfs_create(filename, size);
}

int syscall_delete(char *filename)
{
    return vfs_remove(filename);
}

int syscall_seek(int fd, int offset)
{
    if (fd > 2)
        return vfs_seek(fd - 3, offset);
    return -11;
}

int syscall_filecount(char *path)
{
    return vfs_filecount(path);
}

int syscall_file(char *path, int idx, char *buffer)
{
    return vfs_file(path, idx, buffer);
}

/**
 * Handle system calls. Interrupts are enabled when this function is
 * called.
 *
 * @param user_context The userland context (CPU registers as they
 * where when system call instruction was called in userland)
 */
void syscall_handle(context_t *user_context)
{
    int A1 = user_context->cpu_regs[MIPS_REGISTER_A1];
    int A2 = user_context->cpu_regs[MIPS_REGISTER_A2];
    int A3 = user_context->cpu_regs[MIPS_REGISTER_A3];
    /* When a syscall is executed in userland, register a0 contains
     * the number of the syscall. Registers a1, a2 and a3 contain the
     * arguments of the syscall. The userland code expects that after
     * returning from the syscall instruction the return value of the
     * syscall is found in register v0. Before entering this function
     * the userland context has been saved to user_context and after
     * returning from this function the userland context will be
     * restored from user_context.
     */
    switch(user_context->cpu_regs[MIPS_REGISTER_A0]) {
        case SYSCALL_HALT:
            halt_kernel();
            break;
        case SYSCALL_EXIT:
            syscall_exit(A1);
            break;
        case SYSCALL_WRITE:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_write(A1, (char *)A2, A3);
            break;
        case SYSCALL_READ:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_read(A1, (char *)A2, A3);
            break;
        case SYSCALL_JOIN:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_join(A1);
            break;
        case SYSCALL_EXEC:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_exec((char *)A1);
            break;
        case SYSCALL_OPEN:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_open((char *)A1);
            break;
        case SYSCALL_CLOSE:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_close(A1);
            break;
        case SYSCALL_CREATE:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_create((char *)A1, A2);
            break;
        case SYSCALL_DELETE:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_delete((char *)A1);
            break;
        case SYSCALL_SEEK:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_seek(A1, A2);
            break;
        case SYSCALL_FILECOUNT:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_filecount((char *)A1);
            break;
        case SYSCALL_FILE:
            user_context->cpu_regs[MIPS_REGISTER_V0] =
                syscall_file((char *)A1, A2, (char *)A3);
            break;
        default:
            KERNEL_PANIC("Unhandled system call\n");
    }

    /* Move to next instruction after system call */
    user_context->pc += 4;
}
