/**********************************
 * sorted directory listing
 *
 * see dirlist.h
 *
 * Last change: 2026/11/12
 ***********************************/

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "dirlist.h"

static char     dl_pool[DIRLIST_POOL_SIZE];     // per entry: attribute byte, name, 0
static uint16_t dl_entry[DIRLIST_MAX_ENTRIES];  // offset of each entry in dl_pool, sorted
static uint16_t dl_count = 0;
static bool     dl_complete = false;
static char     dl_path[512];

/////////////////////////////////////////////////////////////////////

// case-insensitive compare of a[0..len_a) and b[0..len_b),
// runs of digits compared by their value
static int natural_compare(const char* a, const char* const end_a, const char* b, const char* const end_b)
{
    while ((a < end_a) && (b < end_b))
    {
        if (isdigit((unsigned char) *a) && isdigit((unsigned char) *b))
        {
            while ((a < end_a) && ('0' == *a)) { ++a; }
            while ((b < end_b) && ('0' == *b)) { ++b; }
            const char* start_a = a;
            const char* start_b = b;
            while ((a < end_a) && isdigit((unsigned char) *a)) { ++a; }
            while ((b < end_b) && isdigit((unsigned char) *b)) { ++b; }
            const size_t len_a = (size_t) (a - start_a);
            const size_t len_b = (size_t) (b - start_b);
            if (len_a != len_b)
            {
                return (len_a < len_b) ? -1 : 1;    // fewer digits = smaller number
            }
            const int c = strncmp(start_a, start_b, len_a);
            if (0 != c)
            {
                return c;
            }
            continue;
        }
        const int ca = tolower((unsigned char) *a);
        const int cb = tolower((unsigned char) *b);
        if (ca != cb)
        {
            return (ca < cb) ? -1 : 1;
        }
        ++a;
        ++b;
    }
    return (a < end_a) ? 1 : ((b < end_b) ? -1 : 0);   // shorter first
}

// end of the name without extension (folders: the whole name)
static const char* name_stem_end(const char* name, const bool is_dir)
{
    const char* end = name + strlen(name);
    if (!is_dir)
    {
        const char* dot = strrchr(name, '.');
        if ((NULL != dot) && (dot != name))
        {
            return dot;
        }
    }
    return end;
}

static int entry_compare(const void* pa, const void* pb)
{
    const char* a = &dl_pool[*(const uint16_t*) pa];
    const char* b = &dl_pool[*(const uint16_t*) pb];
    const bool dir_a = (0 != (a[0] & AM_DIR));
    const bool dir_b = (0 != (b[0] & AM_DIR));
    if (dir_a != dir_b)
    {
        return dir_a ? -1 : 1;      // folders first
    }
    ++a;
    ++b;
    // name without extension first ("Archon" before "Archon II"), then extension
    const char* stem_a = name_stem_end(a, dir_a);
    const char* stem_b = name_stem_end(b, dir_b);
    int c = natural_compare(a, stem_a, b, stem_b);
    if (0 == c)
    {
        c = natural_compare(stem_a, stem_a + strlen(stem_a), stem_b, stem_b + strlen(stem_b));
    }
    return (0 != c) ? c : strcmp(a, b);
}

/////////////////////////////////////////////////////////////////////

// "*.den": density rules for the G64 image of the same name
static bool is_density_file(const char* name, size_t len)
{
    return (len > 4) && ('.' == name[len - 4]) && ('d' == (name[len - 3] | 0x20)) && ('e' == (name[len - 2] | 0x20)) && ('n' == (name[len - 1] | 0x20));
}

uint16_t dirlist_build(DIR* dir, const char* path)
{
    FILINFO entry;
    uint16_t total = 0;
    uint32_t used  = 0;

    dl_count    = 0;
    dl_complete = true;

    while (FR_OK == f_readdir(dir, &entry))
    {
        if (0 == entry.fname[0])
        {
            break;
        }
        ++total;
        const size_t len = strlen(entry.fname);
        if (is_density_file(entry.fname, len))
        {
            continue;   // density rules of an image (see rw_routines.c) are not listed
        }
        if ((dl_count >= DIRLIST_MAX_ENTRIES) || ((used + len + 2) > DIRLIST_POOL_SIZE))
        {
            dl_complete = false;    // keep counting, the list is not used
            continue;
        }
        dl_entry[dl_count++] = (uint16_t) used;
        dl_pool[used] = (char) entry.fattrib;
        memcpy(&dl_pool[used + 1], entry.fname, len + 1);
        used += (uint32_t) (len + 2);
    }

    if (dl_complete)
    {
        qsort(dl_entry, dl_count, sizeof(dl_entry[0]), entry_compare);
    }
    strncpy(dl_path, path, sizeof(dl_path) - 1);
    dl_path[sizeof(dl_path) - 1] = 0;
    return dl_complete ? dl_count : total;
}

/////////////////////////////////////////////////////////////////////

bool dirlist_valid(const char* path)
{
    return dl_complete && (0 == strcmp(dl_path, path));
}

uint16_t dirlist_count(void)
{
    return dl_count;
}

const char* dirlist_name(uint16_t n)
{
    return &dl_pool[dl_entry[n] + 1];
}

uint8_t dirlist_attrib(uint16_t n)
{
    return (uint8_t) dl_pool[dl_entry[n]];
}

void dirlist_clear(void)
{
    dl_count    = 0;
    dl_complete = false;
    dl_path[0]  = 0;
}
