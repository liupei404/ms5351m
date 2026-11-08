/**
 * gt911.h - GT911 capacitive touch controller on software I2C
 *           SCL = PH6, SDA = PI3, INT = PH7, RST = PI8
 */
#ifndef APP_GT911_H
#define APP_GT911_H

#include <stdint.h>

#define GT911_I2C_ADDR_5D 0x5Du
#define GT911_I2C_ADDR_14 0x14u

/* Returns 0 on success, 1 if the controller did not answer */
int  gt911_init(void);

/* Returns 1 when a new point is available (x/y already de-glitched) */
int  gt911_read_point(uint16_t *x, uint16_t *y);

/* 7-bit address actually used, 0 = controller not detected */
uint8_t gt911_get_addr(void);

/* Background supervisor: re-probes while undetected and samples INT */
void gt911_service(void);

#endif /* APP_GT911_H */
