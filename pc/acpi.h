#ifndef WYSE_ACPI_H
#define WYSE_ACPI_H
#include <stdbool.h>
#include <stdint.h>
#include "pc.h"
void acpi_reset(PC *pc);
void acpi_step(void);
bool acpi_io_read(uint16_t port,unsigned size,uint32_t *value);
bool acpi_io_write(uint16_t port,unsigned size,uint32_t value);
void acpi_report(void);
#endif
