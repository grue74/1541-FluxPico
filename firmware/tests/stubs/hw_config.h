#pragma once
#include <stdint.h>
typedef struct sd_card_t { uint32_t (*get_num_sectors)(struct sd_card_t*); } sd_card_t; sd_card_t* sd_get_by_num(int);
