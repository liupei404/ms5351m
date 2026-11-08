/**
 * app.c - application bring-up and main-loop hook
 */
#include "app.h"
#include "bsp_lcd.h"
#include "dma2d.h"
#include "ltdc.h"
#include "siggen_ui.h"
#include "lvgl.h"

/* Optional peripherals.  MS5351M needs I2C1, the GT911 needs its four
   bit-banged pins; both can be switched off per build (-DAPP_ENABLE_MS5351=0
   / -DAPP_ENABLE_GT911=0).  Defaults keep them on.                        */
#ifndef APP_ENABLE_MS5351
#define APP_ENABLE_MS5351 1
#endif
#ifndef APP_ENABLE_GT911
#define APP_ENABLE_GT911 1
#endif

#if APP_ENABLE_GT911
#include "gt911.h"
#endif
#if APP_ENABLE_MS5351
#include "ms5351.h"
#include "i2c_bus.h"
#endif

volatile app_debug_t g_dbg;

#define PANEL_TEST_MODE 0   /* 0 = normal UI, 1 = colour cycle, 2 = background only, 3 = DMA2D fill */

static lv_point_t s_touch_point;
static uint32_t s_lt_isr_count;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  uint16_t x = 0u;
  uint16_t y = 0u;

  (void)indev;

#if APP_ENABLE_GT911
  if (gt911_read_point(&x, &y) != 0)
  {
    s_touch_point.x = (int32_t)x;
    s_touch_point.y = (int32_t)y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
#else
  (void)x;
  (void)y;
  data->state = LV_INDEV_STATE_RELEASED;
#endif

  data->point = s_touch_point;
}

void app_init(void)
{
  g_dbg.magic = APP_DBG_MAGIC;

  bsp_lcd_mpu_config();
  bsp_lcd_init();
  g_dbg.lvgl_ok = 1u;

#if (PANEL_TEST_MODE == 3)
  /* Same data path as the known-good sdr_trx project: fill the frame buffer
     with DMA2D (bypasses the CPU cache completely). */
  hdma2d.Init.Mode         = DMA2D_R2M;
  hdma2d.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
  hdma2d.Init.OutputOffset = 0;
  (void)HAL_DMA2D_Init(&hdma2d);
  (void)HAL_DMA2D_Start(&hdma2d, 0x07E0U, LCD_FB_ADDR, (uint32_t)LCD_WIDTH, (uint32_t)LCD_HEIGHT);
  (void)HAL_DMA2D_PollForTransfer(&hdma2d, 100U);
  g_dbg.ui_ok = 3u;
#elif (PANEL_TEST_MODE == 2)
  /* Raw panel path only: no touch driver, no MS5351, no LVGL widgets.
     Layers stay off and the LTDC background colour is set to pure green, so
     the panel should show a solid green screen without touching SDRAM.     */
  __HAL_LTDC_LAYER_DISABLE(&hltdc, 0);
  LTDC->BCCR = 0x0000FF00U;   /* RGB888 background: pure green */
  LTDC->SRCR = LTDC_SRCR_VBR; /* reload */
  g_dbg.ui_ok = 2u;
#else
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
  lv_indev_set_display(indev, lv_display_get_default());

#if APP_ENABLE_GT911
  (void)gt911_init();
#endif
#if APP_ENABLE_MS5351
  app_i2c_init();
  (void)ms5351_init();
#endif

#if (PANEL_TEST_MODE == 0)
  siggen_ui_create();
  g_dbg.ui_ok = 1u;
#endif
#endif
}

void app_process(void)
{
  lv_timer_handler();

  /* Count LTDC FIFO underruns: they mean the panel is reading pixels faster
     than SDRAM can serve them (visible as noise / flicker).  0 after a few
     seconds of running means the display path has enough bandwidth.      */
  if ((LTDC->ISR & LTDC_ISR_FUIF) != 0u)
  {
    s_lt_isr_count++;
    LTDC->ICR = 0x0000001FU;
  }

#if APP_ENABLE_GT911
  gt911_service();
#endif

#if (PANEL_TEST_MODE == 1)
  /* Full-screen solid colours, one per second: red -> green -> blue -> white.
     Used to judge the panel path independently of LVGL.                     */
  {
    static uint32_t s_last_tick;
    static uint32_t s_phase;

    if (HAL_GetTick() - s_last_tick >= 1000U)
    {
      s_last_tick = HAL_GetTick();

      switch (s_phase & 3U)
      {
        case 0U: bsp_lcd_fill(0xF800U); break;             /* red   */
        case 1U: bsp_lcd_fill(0x07E0U); break;             /* green */
        case 2U: bsp_lcd_fill(0x001FU); break;             /* blue  */
        default: bsp_lcd_fill(0xFFFFU); break;             /* white */
      }
      s_phase++;
    }
  }
#endif

  g_dbg.app_loops++;
}
