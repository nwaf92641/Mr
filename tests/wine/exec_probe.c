/* What a process on this machine is allowed to do with executable pages.
 *
 * This is asked because the load test reached the point where Wine maps the PE
 * image and reported:
 *
 *   err:virtual:map_image_into_view failed to set 60000020 protection on
 *   L"\\??\\Z:\\...\\load_d3d11.exe" section .text, noexec filesystem?
 *
 * 0x60000020 is PAGE_EXECUTE_READ, so Wine could not make the section executable.
 * On arm64 macOS that is not a question about the filesystem: executable mappings
 * are a code signing matter, and whether they are permitted depends on how the
 * process itself is signed. The three mappings below are the ways a program gets
 * executable memory, and the last one is the one Wine's loader takes -- page
 * writable first, executable afterwards. The call at the end says whether code in
 * such a page actually runs, which is the difference between a permission being
 * granted and a mapping being usable.
 *
 * Run unsigned, and run again after ad-hoc signing with --entitlements. The two
 * runs together say whether the answer is an entitlement, or something else. */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void report( const char *what, void *at, int err )
{
    if (at == MAP_FAILED)
        printf( "EXEC FAIL %-30s errno=%d (%s)\n", what, err, strerror( err ));
    else
        printf( "EXEC OK   %-30s at %p\n", what, at );
}

static void report_call( const char *what, long result )
{
    if (result < 0)
        printf( "EXEC FAIL %-30s the call did not return\n", what );
    else
        printf( "EXEC OK   %-30s returned %ld\n", what, result );
}

/* The case a PE loader performs: a file mapped writable while the section is
 * copied in, then made executable. Wine's set_vprot() is this second step, and
 * map_image_into_view() reported that it failed on the .text section of the load
 * test. The path is the file Wine maps, so the probe is asking about that file. */
static void probe_file( const char *path )
{
    struct stat st;
    int fd = open( path, O_RDONLY );
    void *p;

    if (fd == -1)
    {
        printf( "EXEC SKIP %-30s cannot open %s\n", "file mapped, then rx", path );
        return;
    }
    if (fstat( fd, &st ) == -1 || st.st_size == 0)
    {
        printf( "EXEC SKIP %-30s cannot stat %s\n", "file mapped, then rx", path );
        close( fd );
        return;
    }

    errno = 0;
    p = mmap( NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0 );
    if (p == MAP_FAILED)
    {
        report( "file mapped, then rx", p, errno );
        close( fd );
        return;
    }

    errno = 0;
    if (mprotect( p, (size_t)st.st_size, PROT_READ | PROT_EXEC ) == -1)
        report( "file mapped, then rx", MAP_FAILED, errno );
    else
        report( "file mapped, then rx", p, 0 );

    munmap( p, (size_t)st.st_size );
    close( fd );
}

int main( int argc, char **argv )
{
    size_t page = (size_t)getpagesize();
    void *p;
    int err;

    /* Executable from the moment it is mapped. */
    errno = 0;
    p = mmap( NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
              MAP_PRIVATE | MAP_ANON, -1, 0 );
    err = errno;
    report( "mmap rwx anonymous", p, err );
    if (p != MAP_FAILED) munmap( p, page );

    /* The order an image loader uses: writable while the section is written, then
     * read and execute. Wine's set_vprot() is this second step. */
    errno = 0;
    p = mmap( NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (p == MAP_FAILED)
    {
        report( "mprotect rx after rw", p, errno );
    }
    else
    {
        errno = 0;
        if (mprotect( p, page, PROT_READ | PROT_EXEC ) == -1)
            report( "mprotect rx after rw", MAP_FAILED, errno );
        else
            report( "mprotect rx after rw", p, 0 );
        munmap( p, page );
    }

#ifdef MAP_JIT
    /* What the subsystem for just-in-time compilation is for. It needs the
     * allow-jit entitlement, which is what the signed run is here to show. */
    errno = 0;
    p = mmap( NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
              MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0 );
    err = errno;
    report( "mmap rwx MAP_JIT", p, err );
    if (p != MAP_FAILED) munmap( p, page );
#else
    printf( "EXEC SKIP %-30s MAP_JIT is not defined here\n", "mmap rwx MAP_JIT" );
#endif

#if defined(__aarch64__)
    /* Permission to map is not permission to run. A page is written with two arm64
     * instructions, mov w0, #7 and ret, made executable, and then called. */
    errno = 0;
    p = mmap( NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (p == MAP_FAILED)
    {
        report( "write, mprotect, call", p, errno );
    }
    else
    {
        unsigned int *code = p;

        code[0] = 0x528000e0;   /* mov  w0, #7 */
        code[1] = 0xd65f03c0;   /* ret        */
        __builtin___clear_cache( (char *)code, (char *)code + 8 );

        errno = 0;
        if (mprotect( p, page, PROT_READ | PROT_EXEC ) == -1)
        {
            report( "write, mprotect, call", MAP_FAILED, errno );
        }
        else
        {
            long (*f)( void ) = (long (*)( void ))p;
            /* A signal handler would be the careful way to call something that may
             * not run. The point of the probe is the answer, and a process that
             * cannot run the page fails here rather than returning a wrong value. */
            report_call( "write, mprotect, call", f() );
        }
        munmap( p, page );
    }
#else
    printf( "EXEC SKIP %-30s not arm64\n", "write, mprotect, call" );
#endif

    /* The file the workflow passes is the load test, which is the exact file Wine
     * maps and could not make executable. Without an argument, this binary. */
    probe_file( argc > 1 ? argv[1] : argv[0] );

    return 0;
}
