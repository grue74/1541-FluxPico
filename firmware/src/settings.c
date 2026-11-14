/**********************************
 * persistent settings in flash
 *
 * see settings.h
 *
 * Last change: 2026/11/12
 ***********************************/

#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"

#include "settings.h"
#include "main.h"

#define SETTINGS_MAGIC      (0x31353431u)       // "1541"
#define SETTINGS_VERSION    (1)
#define SETTINGS_OFFSET     (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)     // last sector

// stored layout - only append new fields at the end, defaults for old blocks
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;          // sizeof(settings_block_t) when written
    uint8_t  beeper;        // stepper sound on/off
    uint8_t  reserved[3];
    uint32_t checksum;      // over all bytes before it
} settings_block_t;

static volatile bool save_requested = false;

/////////////////////////////////////////////////////////////////////

static uint32_t settings_checksum(const settings_block_t* block)
{
    const uint8_t* p = (const uint8_t*) block;
    uint32_t sum = 0x5A5A5A5Au;
    for (size_t i = 0; i < offsetof(settings_block_t, checksum); ++i)
    {
        sum = (sum << 5) + (sum >> 27) + p[i];
    }
    return sum;
}

static void settings_collect(settings_block_t* block)
{
    memset(block, 0, sizeof(*block));
    block->magic   = SETTINGS_MAGIC;
    block->version = SETTINGS_VERSION;
    block->size    = sizeof(settings_block_t);
    block->beeper  = beeper_enabled ? 1 : 0;
    block->checksum = settings_checksum(block);
}

/////////////////////////////////////////////////////////////////////

void settings_load(void)
{
    const settings_block_t* stored = (const settings_block_t*) (XIP_BASE + SETTINGS_OFFSET);

    if ((SETTINGS_MAGIC == stored->magic) && (sizeof(settings_block_t) == stored->size)
        && (settings_checksum(stored) == stored->checksum))
    {
        beeper_enabled = (0 != stored->beeper);
    }
    // else: erased flash or unknown layout - keep the defaults
}

/////////////////////////////////////////////////////////////////////

void settings_request_save(void)
{
    save_requested = true;
}

/////////////////////////////////////////////////////////////////////

// runs with the other core parked and interrupts off - only RAM code paths
static void settings_write(void* param)
{
    const uint8_t* page = (const uint8_t*) param;
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_OFFSET, page, FLASH_PAGE_SIZE);
}

void settings_service(void)
{
    if (!save_requested)
    {
        return;
    }
    save_requested = false;

    static uint8_t page[FLASH_PAGE_SIZE];
    settings_block_t block;
    settings_collect(&block);

    // unchanged: no flash write (saves wear)
    if (0 == memcmp((const void*) (XIP_BASE + SETTINGS_OFFSET), &block, sizeof(block)))
    {
        return;
    }

    memset(page, 0xFF, sizeof(page));
    memcpy(page, &block, sizeof(block));
    if (PICO_OK != flash_safe_execute(settings_write, page, 1000))
    {
        save_requested = true;      // core1 did not respond: try again later
    }
}
