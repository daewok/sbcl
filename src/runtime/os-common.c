/*
 * This software is part of the SBCL system. See the README file for
 * more information.
 *
 * This software is derived from the CMU CL system, which was
 * written at Carnegie Mellon University and released into the
 * public domain. The software is in the public domain and is
 * provided with absolutely no warranty. See the COPYING and CREDITS
 * files for more information.
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "sbcl.h"
#include "globals.h"
#include "runtime.h"
#include "genesis/config.h"
#include "genesis/constants.h"
#include "genesis/cons.h"
#include "genesis/vector.h"
#include "genesis/symbol.h"
#include "genesis/static-symbols.h"
#include "thread.h"
#include "sbcl.h"
#include "os.h"
#include "arch.h"
#include "interr.h"
#include "immobile-space.h"

/*
 * historically, this used sysconf to select the runtime page size
 * per recent changes on other arches and discussion on sbcl-devel,
 * however, this is not necessary -- the VM page size need not match
 * the OS page size (and the default backend page size has been
 * ramped up accordingly for efficiency reasons).
*/
os_vm_size_t os_vm_page_size = BACKEND_PAGE_BYTES;

/* Expose to Lisp the value of the preprocessor define. Don't touch! */
int install_sig_memory_fault_handler = INSTALL_SIG_MEMORY_FAULT_HANDLER;

/* Except for os_zero, these routines are only called by Lisp code.
 * These routines may also be replaced by os-dependent versions
 * instead. See hpux-os.c for some useful restrictions on actual
 * usage. */

#ifdef LISP_FEATURE_CHENEYGC
void
os_zero(os_vm_address_t addr, os_vm_size_t length)
{
    os_vm_address_t block_start;
    os_vm_size_t block_size;

#ifdef DEBUG
    fprintf(stderr,";;; os_zero: addr: 0x%08x, len: 0x%08x\n",addr,length);
#endif

    block_start = os_round_up_to_page(addr);

    length -= block_start-addr;
    block_size = os_trunc_size_to_page(length);

    if (block_start > addr)
        bzero((char *)addr, block_start-addr);
    if (block_size < length)
        bzero((char *)block_start+block_size, length-block_size);

    if (block_size != 0) {
        /* Now deallocate and allocate the block so that it faults in
         * zero-filled. */

        os_invalidate(block_start, block_size);
        addr = os_validate(NOT_MOVABLE, block_start, block_size);

        if (addr == NULL || addr != block_start)
            lose("os_zero: block moved! %p ==> %p", block_start, addr);
    }
}
#endif

os_vm_address_t
os_allocate(os_vm_size_t len)
{
    return os_validate(MOVABLE, (os_vm_address_t)NULL, len);
}

void
os_deallocate(os_vm_address_t addr, os_vm_size_t len)
{
    os_invalidate(addr,len);
}

int
os_get_errno(void)
{
    return errno;
}


#if defined(LISP_FEATURE_SB_THREAD) && (!defined(CANNOT_USE_POSIX_SEM_T) || defined(LISP_FEATURE_WIN32))

void
os_sem_init(os_sem_t *sem, unsigned int value)
{
    if (-1==sem_init(sem, 0, value))
        lose("os_sem_init(%p, %u): %s", sem, value, strerror(errno));
    FSHOW((stderr, "os_sem_init(%p, %u)\n", sem, value));
}

void
os_sem_wait(os_sem_t *sem, char *what)
{
    FSHOW((stderr, "%s: os_sem_wait(%p) ...\n", what, sem));
    while (-1 == sem_wait(sem))
        if (EINTR!=errno)
            lose("%s: os_sem_wait(%p): %s", what, sem, strerror(errno));
    FSHOW((stderr, "%s: os_sem_wait(%p) => ok\n", what, sem));
}

void
os_sem_post(sem_t *sem, char *what)
{
    if (-1 == sem_post(sem))
        lose("%s: os_sem_post(%p): %s", what, sem, strerror(errno));
    FSHOW((stderr, "%s: os_sem_post(%p)\n", what, sem));
}

void
os_sem_destroy(os_sem_t *sem)
{
    if (-1==sem_destroy(sem))
        lose("os_sem_destroy(%p): %s", sem, strerror(errno));
}

#endif

/* When :LINKAGE-TABLE is enabled, the special category of /static/ foreign
 * symbols disappears. Foreign fixups are resolved to linkage table locations
 * during genesis, and for each of them a record is added to
 * REQUIRED_FOREIGN_SYMBOLS vector, of the form "name" for a function reference,
 * or ("name") for a data reference. "name" is a base-string.
 *
 * Before any code in lisp image can be called, we have to resolve all
 * references to runtime foreign symbols that used to be static, adding linkage
 * table entry for each element of lisp_linkage_values.
 */

int lisp_linkage_table_n_prelinked;

#if defined(LISP_FEATURE_ELF)
// Weak only works on ELF targets and we'd like this to be weak on those
// targets for shrinkwrapping.
extern __attribute__((weak)) lispobj lisp_linkage_values;
#else
extern lispobj lisp_linkage_values;
#endif

void os_link_runtime()
{

    // There is a potentially better technique we could use which would
    // simplify this function on platforms with dlopen/dlsym, namely: all we
    // need are two prefilled entries: one for dlsym() itself, and one for the
    // allocation region overflow handler ("alloc" or "alloc_tramp").  Lisp can
    // fill in the linkage table as the very first action on startup.
#ifdef LISP_FEATURE_LINKAGE_TABLE
    int entry_index = 0;

    // Prefill the Lisp linkage table using references stored in
    // lisp_linkage_values. This array has an interesting format. The first
    // entry is interpreted as how many references to symbols are found in the
    // array. Each subsequent entry is either a reference or -1 (an invalid
    // funciton pointer). A -1 indicates that the following reference is a
    // reference to data instead of a function.
    lispobj *ptr = &lisp_linkage_values;

    if (&lisp_linkage_values) {
      int count;
      count = lisp_linkage_table_n_prelinked = *ptr++;
      for ( ; count-- ; entry_index++ ) {
        if (entry_index == 0) {
#ifdef LISP_FEATURE_WIN32
          os_validate_recommit(LINKAGE_TABLE_SPACE_START, os_vm_page_size);
#endif
        }
        boolean datap = *ptr == (lispobj)-1; // -1 can't be a function address
        if (datap)
          ++ptr;
        arch_write_linkage_table_entry(entry_index, (void*)*ptr++, datap);
      }
      return;
    }
#endif /* LISP_FEATURE_LINKAGE_TABLE */
}

void os_unlink_runtime()
{
}

boolean
gc_managed_heap_space_p(lispobj addr)
{
    if ((READ_ONLY_SPACE_START <= addr && addr < READ_ONLY_SPACE_END)
        || (STATIC_SPACE_START <= addr && addr < STATIC_SPACE_END)
#if defined LISP_FEATURE_GENCGC
        || (DYNAMIC_SPACE_START <= addr &&
            addr < (DYNAMIC_SPACE_START + dynamic_space_size))
        || immobile_space_p(addr)
#else
        || (DYNAMIC_0_SPACE_START <= addr &&
            addr < DYNAMIC_0_SPACE_START + dynamic_space_size)
        || (DYNAMIC_1_SPACE_START <= addr &&
            addr < DYNAMIC_1_SPACE_START + dynamic_space_size)
#endif
        )
        return 1;
    return 0;
}

#ifndef LISP_FEATURE_WIN32

/* Remap a part of an already existing memory mapping from a file,
 * and/or create a new mapping as need be */
void* load_core_bytes(int fd, os_vm_offset_t offset, os_vm_address_t addr, os_vm_size_t len)
{
    int fail = 0;
#ifdef LISP_FEATURE_HPUX
    // Revision afcfb8b5da said that mmap() didn't work on HPUX, changing to use read() instead.
    // Strangely it also read 4K at a time into a buffer and used memcpy to transfer the buffer.
    // I don't see why, and given the lack of explanation, I've simplified to 1 read.
    fail = lseek(fd, offset, SEEK_SET) == (off_t)-1 || read(fd, addr, len) != (ssize_t)len;
    // This looks bogus but harmlesss, so I'm leaving it.
    os_flush_icache(addr, len);
#else
    os_vm_address_t actual;
    actual = mmap(addr, len,
                  // If mapping to a random address, then the assumption is
                  // that we're not going to execute the core; nor should we write to it.
                  // However, the addr=0 case is for 'editcore' which unfortunately _does_
                  // write the memory. I'd prefer that it not,
                  // but that's not the concern here.
                  addr ? OS_VM_PROT_ALL : OS_VM_PROT_READ | OS_VM_PROT_WRITE,
                  // Do not pass MAP_FIXED with addr of 0, because most OSes disallow that.
                  MAP_PRIVATE | (addr ? MAP_FIXED : 0),
                  fd, (off_t) offset);
    if (actual == MAP_FAILED) {
        perror("mmap");
        fail = 1;
    } else if (addr && (addr != actual)) {
        fail = 1;
    }
#endif
    if (fail)
        lose("load_core_bytes(%d,%zx,%p,%zx) failed", fd, offset, addr, len);
    return (void*)actual;
}

boolean
gc_managed_addr_p(lispobj addr)
{
    struct thread *th;

    if (gc_managed_heap_space_p(addr))
        return 1;
    for_each_thread(th) {
        if(th->control_stack_start <= (lispobj*)addr
           && (lispobj*)addr < th->control_stack_end)
            return 1;
        if(th->binding_stack_start <= (lispobj*)addr
           && (lispobj*)addr < th->binding_stack_start + BINDING_STACK_SIZE)
            return 1;
    }
    return 0;
}

#endif
