/**********************************
 * header - sorted directory listing
 *
 * FAT directories come in the order the files were written to the card.
 * dirlist_build() reads a directory once, keeps the names in a pool and
 * sorts them: folders first, then files, case-insensitive, numbers by
 * value ("disk 2" before "disk 10").
 * If a directory does not fit (too many entries or too long names), the
 * list is marked invalid and the callers use the FAT order as before.
 *
 * Last change: 2026/11/12
 ***********************************/

#ifndef _DIRLIST_H_
#define _DIRLIST_H_

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"

#define DIRLIST_POOL_SIZE   (32*1024)   // names incl. attribute byte and terminator
#define DIRLIST_MAX_ENTRIES (1024)

// read and sort all entries of the open directory dir; path is remembered.
// returns the number of entries in the directory (also if they do not fit)
uint16_t dirlist_build(DIR* dir, const char* path);

// true if the sorted list holds the complete directory path
bool dirlist_valid(const char* path);

// number of entries in the list
uint16_t dirlist_count(void);

// name and FAT attributes of sorted entry n (n < dirlist_count())
const char* dirlist_name(uint16_t n);
uint8_t dirlist_attrib(uint16_t n);

// forget the list (card removed, ...)
void dirlist_clear(void);

#endif
