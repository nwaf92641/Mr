#!/bin/sh
# Unit tests for the ml810 registry edit: pin Graphics=null for the iOS prefix.
#
# The function under test rewrites the PREFIX'S REGISTRY ON DISK. A bug here
# does not fail a build, it corrupts a user's user.reg -- the file that holds
# every application setting the prefix has. So it is asserted against fixtures
# and against the real shipped template rather than eyeballed, and the tests
# that matter most are the ones proving it does NOTHING when it has nothing to
# do (already pinned, no section, no trailing newline).
#
# The production code is compiled from the source file -- extracted with awk,
# not copied -- so a change to WineProcessBridge.m is tested as written.
#
# Needs a C compiler; skips (exit 0) without one, like the other C tests here.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "test-graphics-driver-pin: SKIP -- no C compiler on PATH (override with CC=...)" >&2
    exit 0
fi

SRC="app/Madeira/WineProcessBridge.m"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Pull the production function out verbatim: from its signature to the first
# line that closes it at column 0.
awk '/^static int ios_reg_pin_graphics_null\(const char \*path\)$/ { f = 1 }
     f { print }
     f && /^\}/ { exit }' "$SRC" > "$TMP/pin.c"

if ! grep -q 'strstr' "$TMP/pin.c"; then
    echo "test-graphics-driver-pin: FAIL -- could not extract ios_reg_pin_graphics_null from $SRC" >&2
    exit 1
fi

cat > "$TMP/harness.c" <<'C'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/limits.h>
#endif

/* The production LOG macro needs a stub: the function only reports through it. */
#define LOG(...) do { (void)0; } while (0)

#include "pin.c"

/* -- fixtures ------------------------------------------------------------ */

static const char F_INSERT[] =
    "[Software\\\\Wine\\\\Drivers] 1776969700\n"
    "#time=1dcd350bd2d1900\n"
    "\"Audio\"=\"ios\"\n"
    "\n"
    "[Software\\\\Wine\\\\Fonts\\\\Replacements] 1783279264\n"
    "\"Arial\"=\"Tahoma\"\n";

static const char F_REPLACE[] =
    "[Other\\\\Key] 1\n"
    "\"Keep\"=\"yes\"\n"
    "\n"
    "[Software\\\\Wine\\\\Drivers] 1776969700\n"
    "\"Graphics\"=\"winemac.drv\"\n"
    "\"Audio\"=\"ios\"\n"
    "\n"
    "[Software\\\\Wine\\\\Fonts] 2\n"
    "\"Graphics\"=\"must-not-touch\"\n";

static const char F_PINNED[] =
    "[Software\\\\Wine\\\\Drivers] 1776969700\n"
    "\"Graphics\"=\"null\"\n"
    "\"Audio\"=\"ios\"\n"
    "\n"
    "[Next] 3\n";

static const char F_ABSENT[] =
    "[Software\\\\Wine\\\\Fonts] 2\n"
    "\"Arial\"=\"Tahoma\"\n";

static const char F_EOF_NO_NEWLINE[] =
    "[Software\\\\Wine\\\\Drivers] 1776969700";

static const char F_KEY_ELSEWHERE[] =
    "[Software\\\\Wine\\\\Drivers\\\\Sub] 5\n"
    "\"Graphics\"=\"winemac.drv\"\n"
    "\n"
    "[Software\\\\Wine\\\\Drivers] 1776969700\n"
    "\"Audio\"=\"ios\"\n";

/* -- helpers ------------------------------------------------------------- */

static int failures = 0;

static void write_fixture(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) exit(1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) exit(1);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static void expect(int cond, const char *what)
{
    if (cond) printf("  ok   %s\n", what);
    else { printf("  FAIL %s\n", what); failures++; }
}

/* Number of "Graphics" lines inside the [Software\\Wine\\Drivers] section. */
static int graphics_in_section(const char *text, const char **value, size_t *vlen)
{
    const char *sec = strstr(text, "[Software\\\\Wine\\\\Drivers]");
    if (!sec) return -1;
    const char *eol = strchr(sec, '\n');
    if (!eol) return -1;
    const char *end = strchr(eol + 1, '[');
    if (!end) end = text + strlen(text);
    int count = 0;
    for (const char *q = eol + 1; q < end; ) {
        const char *nl = strchr(q, '\n');
        const char *stop = nl ? nl : end;
        if (!strncmp(q, "\"Graphics\"=\"", 12)) {
            count++;
            if (value) {
                *value = q + 12;
                *vlen = (size_t)((strchr(q + 12, '"') ? strchr(q + 12, '"') : stop) - (q + 12));
            }
        }
        if (!nl) break;
        q = nl + 1;
    }
    return count;
}

static void run(const char *dir, const char *name, const char *fixture,
                int expect_changed, int expect_section)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.reg", dir, name);
    write_fixture(path, fixture);
    char *before = read_file(path);

    int rc = ios_reg_pin_graphics_null(path);

    char *after = read_file(path);
    const char *value = NULL; size_t vlen = 0;
    int n = graphics_in_section(after, &value, &vlen);
    int pinned = (n > 0 && value && vlen == 4 && !strncmp(value, "null", 4));

    printf("%s:\n", name);
    expect(rc == expect_changed, "return value says changed / not changed");
    if (expect_section) {
        expect(n == 1, "exactly one Graphics value in the section");
        expect(pinned, "the value is null");
    } else {
        expect(n == -1, "no Drivers section to write into");
    }
    if (expect_changed == 0)
        expect(!strcmp(before, after), "file left byte-identical");

    /* Idempotence: a second call must be a no-op in every case. */
    char *mid = read_file(path);
    int rc2 = ios_reg_pin_graphics_null(path);
    char *end = read_file(path);
    expect(rc2 == 0, "second call reports no change");
    expect(!strcmp(mid, end), "second call leaves the file byte-identical");

    free(before); free(after); free(mid); free(end);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: harness <tmpdir> [real-user.reg]\n"); return 2; }
    const char *dir = argv[1];

    printf("insert into a section that has no Graphics value:\n");
    run(dir, "insert", F_INSERT, 1, 1);

    printf("replace a foreign Graphics value:\n");
    run(dir, "replace", F_REPLACE, 1, 1);
    {
        /* The other section's "must-not-touch" value has to survive. */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/replace.reg", dir);
        char *after = read_file(path);
        expect(strstr(after, "\"Graphics\"=\"must-not-touch\"") != NULL,
               "a Graphics value in another section is untouched");
        expect(strstr(after, "winemac.drv") == NULL,
               "the stale driver name is gone");
        expect(strstr(after, "[Other\\\\Key] 1") != NULL, "surrounding sections survive");
        free(after);
    }

    printf("already pinned:\n");
    run(dir, "pinned", F_PINNED, 0, 1);

    printf("no Drivers section at all:\n");
    run(dir, "absent", F_ABSENT, 0, 0);

    printf("section header with no trailing newline:\n");
    run(dir, "eof", F_EOF_NO_NEWLINE, 0, 0);

    printf("Graphics under a different key, none under ours:\n");
    run(dir, "elsewhere", F_KEY_ELSEWHERE, 1, 1);
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/elsewhere.reg", dir);
        char *after = read_file(path);
        expect(strstr(after, "[Software\\\\Wine\\\\Drivers\\\\Sub] 5\n"
                             "\"Graphics\"=\"winemac.drv\"") != NULL,
               "the sub-key value is untouched");
        free(after);
    }

    if (argc >= 3) {
        printf("the real shipped prefix template:\n");
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/real.reg", dir);
        char *src = read_file(argv[2]);
        write_fixture(path, src);
        size_t before_len = strlen(src);
        int rc = ios_reg_pin_graphics_null(path);
        char *after = read_file(path);
        const char *value = NULL; size_t vlen = 0;
        int n = graphics_in_section(after, &value, &vlen);
        expect(rc == 1, "wrote the pin");
        expect(n == 1 && vlen == 4 && !strncmp(value, "null", 4), "Graphics=null present");
        expect(strlen(after) == before_len + strlen("\"Graphics\"=\"null\"\n"),
               "exactly one line was added");
        expect(strstr(after, "\"Audio\"=\"ios\"") != NULL, "the Audio value survives");
        /* Everything after the inserted line must be the original text. */
        const char *tail = strstr(after, "\"Audio\"=\"ios\"");
        expect(tail && strcmp(tail, strstr(src, "\"Audio\"=\"ios\"") + 0) == 0
                   && strlen(tail) == strlen(strstr(src, "\"Audio\"=\"ios\"")),
               "the section body after the insert is unchanged");
        free(src); free(after);
    }

    if (failures) { printf("\n%d check(s) FAILED\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
C

"$CC" -std=c11 -Wall -Wextra -Wno-unused-function -o "$TMP/harness" "$TMP/harness.c"

# Real-registry case: the shipped template's user.reg, extracted from the
# archive the app actually unpacks. This is the file the fix has to work on.
REAL=""
if command -v python3 >/dev/null 2>&1 && [ -f app/Madeira/prefix-template.tar.gz ]; then
    if python3 - "$TMP/real-user.reg" <<'PY'
import sys, tarfile
with tarfile.open("app/Madeira/prefix-template.tar.gz", "r:gz") as tar:
    member = next(m for m in tar.getmembers() if m.name.endswith("user.reg"))
    with open(sys.argv[1], "wb") as out:
        out.write(tar.extractfile(member).read())
PY
    then REAL="$TMP/real-user.reg"; fi
fi

"$TMP/harness" "$TMP" "$REAL"
