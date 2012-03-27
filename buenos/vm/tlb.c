/*
 * TLB handling
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
 * $Id: tlb.c,v 1.6 2004/04/16 10:54:29 ttakanen Exp $
 *
 */

#include "kernel/thread.h"
#include "kernel/panic.h"
#include "kernel/assert.h"
#include "vm/tlb.h"
#include "vm/pagetable.h"
#include "vm/vm.h"
#include "proc/process.h"

void tlb_modified_exception(void) {
    /* A correct handling of this exception would be to send a terminate
       signal to the process that caused it. For now, just panic. */
    KERNEL_PANIC("TLB modified exception.");
}

void tlb_load_exception(void) {
    tlb_store_exception();
}

void tlb_store_exception(void) {
    tlb_exception_state_t tes;
    _tlb_get_exception_state(&tes);

    pagetable_t *ptable = thread_get_current_thread_entry()->pagetable;
    if(ptable == NULL) {
        KERNEL_PANIC("No pagetable associated with thread.");
    }
    uint32_t i;
    for(i=0; i<ptable->valid_count; i++) {
        tlb_entry_t *entry = &ptable->entries[i];
        if(entry->VPN2 == tes.badvpn2) {
            /* Remove this assertion and handle invalid pages properly. */
            KERNEL_ASSERT(tlb_entry_is_valid(entry, tes.badvaddr));

            /* place matching tlb entry somewhere in TLB */
            _tlb_write_random(&ptable->entries[i]);
            return;
        }
    }
    KERNEL_PANIC("Page not found in pagetable.");
}

/** 
 * Get virtual addres of even page in TLB entry.
 */
uint32_t tlb_entry_get_vaddr(tlb_entry_t *entry) {
    return entry->VPN2 << 13;
}

/* Get physical addres from TLB entry corresponding to the given 
   virtual address.
   Asserts that vaddr matches that in given TLB entry. */ 
uint32_t tlb_entry_get_paddr(tlb_entry_t *entry, uint32_t vaddr) {
    KERNEL_ASSERT(entry->VPN2 == vaddr >> 13);
    if(ADDR_IS_ON_EVEN_PAGE(vaddr)) {
        return entry->PFN0 << 12;
    } else {
        return entry->PFN1 << 12;
    }
}


/* Get valid bit from TLB entry corresponding to the given 
   virtual address.
   Asserts that vaddr matches that in given TLB entry. */ 
int tlb_entry_is_valid(tlb_entry_t *entry, uint32_t vaddr) {
    KERNEL_ASSERT(entry->VPN2 == vaddr >> 13);
    if(ADDR_IS_ON_EVEN_PAGE(vaddr)) {
        return entry->V0;
    } else {
        return entry->V1;
    }
}

/* Set valid bit in TLB entry corresponding to the given 
   virtual address.
   Asserts that vaddr matches that in given TLB entry. */ 
void tlb_entry_set_valid(tlb_entry_t *entry, uint32_t vaddr, int valid) {
    KERNEL_ASSERT(entry->VPN2 == vaddr >> 13);
    if(ADDR_IS_ON_EVEN_PAGE(vaddr)) {
        entry->V0 = valid;
    } else {
        entry->V1 = valid;
    }
}


