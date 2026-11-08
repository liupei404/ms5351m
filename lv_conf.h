/**
 * LVGL 9.6 configuration for STM32H743 + 1024x600 RGB LCD (LTDC, RGB565)
 *
 * This file lives OUTSIDE the lvgl/ directory on purpose so that
 * re-importing / regenerating the project never overwrites the config.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_FORMAT_DEFAULT   LV_COLOR_FORMAT_RGB565
#define LV_COLOR_DEPTH            16

/*====================
   MEMORY / STDLIB
 *====================*/
#define LV_USE_STDLIB_MALLOC      LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING      LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF     LV_STDLIB_BUILTIN

/* LVGL heap lives in internal D2 SRAM (0x30000000, 288K free): cacheable,
   CPU-only, never touched by LTDC/DMA, so no cache maintenance is needed.
   SDRAM is reserved for the LTDC framebuffer only.                       */
#define LV_MEM_ADR                0x30000000U
#define LV_MEM_SIZE               (256U * 1024U)

/*====================
   HAL / OS / TICK
 *====================*/
#define LV_USE_OS                 LV_OS_NONE
#define LV_DEF_REFR_PERIOD        20
#define LV_DPI_DEF                130

/*====================
   RENDERING
 *====================*/
#define LV_USE_DRAW_SW            1
#define LV_DRAW_BUF_STRIDE_ALIGN  1
#define LV_DRAW_BUF_ALIGN         4

/*====================
   LOG / DEBUG
 *====================*/
#define LV_USE_LOG               0
#define LV_USE_PERF_MONITOR      0
#define LV_USE_MEM_MONITOR       0

/*====================
   FONTS
 *====================*/
#define LV_FONT_MONTSERRAT_14     1
#define LV_FONT_MONTSERRAT_20     1
#define LV_FONT_MONTSERRAT_28     1
#define LV_FONT_DEFAULT           &lv_font_montserrat_14

/*====================
   WIDGETS USED BY THE UI
 *====================*/
#define LV_USE_CHART              1
#define LV_USE_SLIDER             1
#define LV_USE_SWITCH             1
#define LV_USE_BUTTON             1
#define LV_USE_LABEL              1
#define LV_USE_BAR                1
#define LV_USE_FLEX               1
#define LV_USE_GRID               1

#endif /* LV_CONF_H */
