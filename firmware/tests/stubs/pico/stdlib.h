#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef unsigned int uint; typedef int32_t alarm_id_t; typedef uint64_t absolute_time_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);
#define __unused
enum { GPIO_IN=0, GPIO_OUT=1, GPIO_IRQ_EDGE_RISE=8, GPIO_IRQ_EDGE_FALL=4, GPIO_DRIVE_STRENGTH_12MA=3 };
void gpio_init(uint); void gpio_set_dir(uint,bool); void gpio_put(uint,bool); bool gpio_get(uint);
void gpio_set_pulls(uint,bool,bool); void gpio_init_mask(uint32_t); void gpio_set_dir_in_masked(uint32_t); void gpio_set_dir_out_masked(uint32_t);
void gpio_put_masked(uint32_t,uint32_t); uint32_t gpio_get_all(void); void gpio_set_drive_strength(uint,int); void gpio_set_irq_enabled(uint,uint32_t,bool);
uint64_t time_us_64(void); void busy_wait_us(uint64_t); void sleep_ms(uint32_t);
absolute_time_t from_us_since_boot(uint64_t);
alarm_id_t add_alarm_at(absolute_time_t, alarm_callback_t, void*, bool); alarm_id_t add_alarm_in_ms(uint32_t, alarm_callback_t, void*, bool);
bool cancel_alarm(alarm_id_t);
#define count_of(a) (sizeof(a)/sizeof((a)[0]))
#define bool_to_bit(x) ((uint)!!(x))
#define __compiler_memory_barrier() ((void) 0)
