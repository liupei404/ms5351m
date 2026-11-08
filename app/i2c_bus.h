/**
 * i2c_bus.h - I2C1 bus used by the MS5351M driver
 */
#ifndef APP_I2C_BUS_H
#define APP_I2C_BUS_H

#include "main.h"
#include "i2c.h"

/* Re-targets I2C1 to a GPIOB pin pair and re-initialises hi2c1 at ~400 kHz. */
void app_i2c_set_pins(uint16_t scl_pin, uint16_t sda_pin);

/* Same, on the default pair (SCL = PB8, SDA = PB7). */
void app_i2c_init(void);

#endif /* APP_I2C_BUS_H */
