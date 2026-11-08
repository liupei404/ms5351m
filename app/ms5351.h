/**
 * ms5351.h - MS5351M (Si5351A compatible) clock generator over I2C1 (PB6/PB7)
 *
 * The chip can only produce square waves; frequency is the parameter that is
 * really programmed onto the hardware.  Amplitude / offset / waveform shape
 * are UI-side parameters (see siggen_ui.c).
 */
#ifndef APP_MS5351_H
#define APP_MS5351_H

#include <stdint.h>

/* Crystal fitted on the module.  MS5351M accepts 25MHz or 27MHz parts.
   If the measured output is ~8% high, change this to 27000000.            */
#ifndef MS5351_XTAL_HZ
#define MS5351_XTAL_HZ 25000000UL
#endif

#define MS5351_OUT_MIN_HZ 8000UL
#define MS5351_OUT_MAX_HZ 160000000UL

/* Returns 0 when the device acknowledged on the bus */
int  ms5351_init(void);

/* Program CLK0 to freq_hz (8 kHz .. 160 MHz). Returns 0 on success */
int  ms5351_set_freq(uint32_t freq_hz);

/* 1 = output on, 0 = high-impedance/off */
int  ms5351_output_enable(uint8_t on);

/* Last frequency successfully programmed */
uint32_t ms5351_get_freq(void);

/* Raw DEVICE_STATUS register (bit5 = PLL A unlocked, bit7 = SYS_INIT) */
uint8_t ms5351_get_status(void);

/* 1 when the chip answered at least once */
uint8_t ms5351_present(void);

#endif /* APP_MS5351_H */
