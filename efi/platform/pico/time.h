#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <pico.h>
typedef uint64_t absolute_time_t;
absolute_time_t make_timeout_time_ms(uint32_t ms);
bool time_reached(absolute_time_t t);
