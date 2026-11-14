/**********************************
 * host test for dirlist.c (sorted directory listing)
 ***********************************/

#include <stdio.h>
#include <string.h>

#include "dirlist.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("  FAIL: " __VA_ARGS__); printf("\n"); } else { printf("  ok: " __VA_ARGS__); printf("\n"); } } while (0)

// fake directory read by f_readdir
static const char* const* fake_names;
static const uint8_t* fake_attr;
static int fake_count, fake_pos;

FRESULT f_readdir(DIR* d, FILINFO* fi)
{
    (void) d;
    if (fake_pos < fake_count)
    {
        strcpy(fi->fname, fake_names[fake_pos]);
        fi->fattrib = fake_attr[fake_pos];
        ++fake_pos;
    } else {
        fi->fname[0] = 0;
    }
    return FR_OK;
}

static uint16_t build(const char* const* names, const uint8_t* attr, int n, const char* path)
{
    DIR d;
    fake_names = names; fake_attr = attr; fake_count = n; fake_pos = 0;
    return dirlist_build(&d, path);
}

int main(void)
{
    printf("dirlist:\n");
    {
        static const char* const names[] = {
            "Zak McKracken.d64", "games", "disk 10.g64", "disk 2.g64", "Archon.d64",
            "archon II.d64", "disk 1.g64", "Utilities", "readme.txt", "Disk 02b.g64", "a" };
        static const uint8_t attr[] = { 0, AM_DIR, 0, 0, 0, 0, 0, AM_DIR, 0, 0, 0 };
        static const char* const expect[] = {
            "games", "Utilities", "a", "Archon.d64", "archon II.d64", "disk 1.g64",
            "disk 2.g64", "Disk 02b.g64", "disk 10.g64", "readme.txt", "Zak McKracken.d64" };
        const uint16_t total = build(names, attr, 11, "/c64");
        CHECK(total == 11 && dirlist_count() == 11 && dirlist_valid("/c64"), "11 entries listed and valid");
        int ok = 1;
        for (int i = 0; i < 11; ++i)
        {
            if (strcmp(dirlist_name((uint16_t) i), expect[i])) { ok = 0; printf("    %d: %s (expected %s)\n", i, dirlist_name((uint16_t) i), expect[i]); }
        }
        CHECK(ok, "order: folders first, case-insensitive, numbers by value");
        CHECK(dirlist_attrib(0) & AM_DIR && !(dirlist_attrib(2) & AM_DIR), "attributes follow their entries");
        CHECK(!dirlist_valid("/other"), "list is only valid for its own path");
    }
    {
        // too many entries: list invalid, total still counted
        static char buf[DIRLIST_MAX_ENTRIES + 10][16];
        static const char* names[DIRLIST_MAX_ENTRIES + 10];
        static uint8_t attr[DIRLIST_MAX_ENTRIES + 10];
        for (int i = 0; i < DIRLIST_MAX_ENTRIES + 10; ++i) { snprintf(buf[i], sizeof(buf[i]), "f%d.d64", i); names[i] = buf[i]; attr[i] = 0; }
        const uint16_t total = build(names, attr, DIRLIST_MAX_ENTRIES + 10, "/big");
        CHECK(total == DIRLIST_MAX_ENTRIES + 10 && !dirlist_valid("/big"), "%u entries: counted, falls back to FAT order", total);
        build(names, attr, DIRLIST_MAX_ENTRIES, "/full");
        CHECK(dirlist_valid("/full") && 0 == strcmp(dirlist_name(9), "f9.d64") && 0 == strcmp(dirlist_name(10), "f10.d64"),
              "%d entries still sorted (f9 before f10)", DIRLIST_MAX_ENTRIES);
    }
    {
        // names longer than the pool: invalid
        static char longname[256];
        memset(longname, 'x', 250); longname[250] = 0;
        static const char* names[200];
        static uint8_t attr[200];
        for (int i = 0; i < 200; ++i) { names[i] = longname; attr[i] = 0; }
        build(names, attr, 200, "/long");
        CHECK(!dirlist_valid("/long"), "names exceeding the %d byte pool: falls back to FAT order", DIRLIST_POOL_SIZE);
    }
    dirlist_clear();
    CHECK(!dirlist_valid("/full"), "cleared after unmount");

    printf(failures ? "\n%d FAILURES\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
