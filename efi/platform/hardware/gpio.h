#pragma once
#define GPIO_OUT 1
#define GPIO_IN 0
#define PICO_DEFAULT_LED_PIN 16
static inline void gpio_init(unsigned p) { (void)p; }
static inline void gpio_set_dir(unsigned p, int d) { (void)p; (void)d; }
static inline void gpio_put(unsigned p, int v) { (void)p; (void)v; }
