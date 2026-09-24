/* Which addresses this machine lets a process put a mapping at, page by page.
 *
 * The load test stopped with the PE image at its own ImageBase, 0x140000000, and
 * Wine unable to raise the protections of any of its pages:
 *
 *   arm64 set_vprot: mprotect to 0x1 failed for 0x140000000-0x140001000,
 *   errno 13 (Permission denied)
 *
 * and with a fixed mapping at 0x100000000 refused:
 *
 *   mach ret 1 (KERN_INVALID_ADDRESS) at 0x100000000-0x100030000
 *
 * The loader's own PAGEZERO was measured at 4GB, which is a window that slides
 * with the image and is not mapped, so both addresses could be inside it. This
 * asks the kernel directly, from a process with the same layout, at every GB from
 * the 4GB line upward: is a fixed mapping granted here, and is it granted when the
 * pages asked for are read and execute rather than nothing.
 *
 * The two protections are asked separately because they fail differently. A window
 * reserved for the main executable refuses to be mapped at all; an address that is
 * mapped but cannot be given to Wine refuses one of the two.
 *
 * A mapping that is granted is unmapped immediately: the question is permission,
 * and the process is left as it was found. */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

/* Every GB from the 4GB line to 0x800000000, plus the addresses that were seen in
 * the runs: the shared user data page Wine was told to use, the PE ImageBase, and
 * the two ends of the loader's own image. */
static const unsigned long long addresses[] = {
    0x100000000ULL,  /* refused with KERN_INVALID_ADDRESS in the run */
    0x101000000ULL,
    0x110000000ULL,
    0x140000000ULL,  /* the load test's ImageBase, where mprotect failed */
    0x180000000ULL,
    0x1c0000000ULL,
    0x200000000ULL,
    0x240000000ULL,
    0x300000000ULL,
    0x400000000ULL,
    0x500000000ULL,
    0x600000000ULL,
    0x7ff000000000ULL, /* the shared user data page Wine was moved to */
    0x7ffee000000ULL,
    0x800000000ULL,
};

static void probe( unsigned long long addr, int prot, const char *name )
{
    mach_vm_address_t got = (mach_vm_address_t)addr;
    mach_vm_size_t size = (mach_vm_size_t)getpagesize();
    kern_return_t ret = mach_vm_map( mach_task_self(), &got, size, 0, VM_FLAGS_FIXED,
                                     MEMORY_OBJECT_NULL, 0, 0, prot, VM_PROT_ALL,
                                     VM_INHERIT_COPY );

    if (ret == KERN_SUCCESS)
    {
        /* Granted. Readable and executable pages are the ones a PE image needs, so
         * the next question is whether those permissions can be changed afterwards:
         * a mapping that cannot be reduced to read is not usable as a section either. */
        if (prot & VM_PROT_EXECUTE)
        {
            kern_return_t r2 = mach_vm_protect( mach_task_self(), got, size, FALSE, VM_PROT_READ );
            if (r2 == KERN_SUCCESS)
                printf( "ADDR OK   %#012llx %-10s granted, and read after execute\n", addr, name );
            else
                printf( "ADDR FAIL %#012llx %-10s granted, read after execute: mach ret %d\n",
                        addr, name, r2 );
        }
        else printf( "ADDR OK   %#012llx %-10s granted\n", addr, name );

        mach_vm_deallocate( mach_task_self(), got, size );
    }
    else
        printf( "ADDR FAIL %#012llx %-10s mach ret %d\n", addr, name, ret );
}

int main( void )
{
    size_t i;

    printf( "page size %zu, asking for one page at a time\n", (size_t)getpagesize());
    for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
        probe( addresses[i], VM_PROT_NONE, "none" );
        probe( addresses[i], VM_PROT_READ | VM_PROT_EXECUTE, "read+exec" );
    }

    return 0;
}
