/**
 * bsp_lcd.h - LTDC 1024x600 RGB565 panel + LVGL 9.6 display port
 */
#ifndef APP_BSP_LCD_H
#define APP_BSP_LCD_H

#include <stdint.h>

#define LCD_WIDTH   1024U
#define LCD_HEIGHT  600U

/* LTDC layer 0 framebuffer, located in FMC SDRAM (bank1 @ 0xC0000000) */
#define LCD_FB_ADDR 0xC0000000U

void bsp_lcd_mpu_config(void);
void bsp_lcd_pll3_config(void);
void bsp_lcd_init(void);
uint16_t *bsp_lcd_fb(void);
void bsp_lcd_fill(uint16_t color);

#endif /* APP_BSP_LCD_H */
