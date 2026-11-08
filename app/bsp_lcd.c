/**
 * bsp_lcd.c - LTDC panel bring-up and LVGL display driver for STM32H743
 *
 * Panel  : 1024x600 RGB565 (LTDC layer0, framebuffer in SDRAM @0xC0000000)
 * LVGL   : 9.6.x, partial render mode, 2 x (1024x40) draw buffers in AXI SRAM
 * Cache  : SDRAM is mapped as Normal/Non-cacheable by MPU so LTDC always reads
 *          fresh pixels without any cache maintenance.
 */
#include "bsp_lcd.h"
#include "app.h"
#include "main.h"
#include "ltdc.h"
#include "lvgl.h"
#include <string.h>

#define DRAW_BUF_LINES 40
#define DRAW_BUF_PIXELS (LCD_WIDTH * DRAW_BUF_LINES)

static uint16_t s_draw_buf1[DRAW_BUF_PIXELS]
    __attribute__((section(".ltdc_draw_buf"), aligned(32)));
static uint16_t s_draw_buf2[DRAW_BUF_PIXELS]
    __attribute__((section(".ltdc_draw_buf"), aligned(32)));

static lv_display_t *s_disp;

static uint32_t bsp_tick_cb(void)
{
  return HAL_GetTick();
}

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
  int32_t x1 = area->x1;
  int32_t y1 = area->y1;
  int32_t w  = area->x2 - area->x1 + 1;
  int32_t h  = area->y2 - area->y1 + 1;

  if (w <= 0 || h <= 0)
  {
    lv_display_flush_ready(disp);
    return;
  }

  uint16_t *src = (uint16_t *)px_map;
  uint16_t *dst = ((uint16_t *)LCD_FB_ADDR) + ((uint32_t)y1 * LCD_WIDTH) + (uint32_t)x1;
  uint32_t line_bytes = (uint32_t)w * 2U;

  for (int32_t row = 0; row < h; row++)
  {
    memcpy(dst, src, line_bytes);
    src += w;
    dst += LCD_WIDTH;
  }

  /* SDRAM is write-back cacheable: push the freshly written pixels out so
     LTDC (which bypasses the D-Cache) sees them on the next frame.  Ranges
     are widened to 32-byte cache lines.                                    */
  {
    uint32_t addr = (uint32_t)((uint16_t *)LCD_FB_ADDR + ((uint32_t)y1 * LCD_WIDTH) + (uint32_t)x1);
    uint32_t bytes = line_bytes;
    uint32_t start = addr & ~31UL;
    uint32_t end   = (addr + bytes + 31UL) & ~31UL;

    for (int32_t row = 0; row < h; row++)
    {
      SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
      start += LCD_WIDTH * 2U;
      end   += LCD_WIDTH * 2U;
    }
  }

  g_dbg.flush_cnt++;

  lv_display_flush_ready(disp);
}

void bsp_lcd_mpu_config(void)
{
  MPU_Region_InitTypeDef mpu = {0};

  HAL_MPU_Disable();

  /* Region 0: 0xC0000000 .. 0xC1FFFFFF (32MB SDRAM), Normal, write-back
     cacheable + bufferable (same policy as the known-good sdr_trx project).
     The flush path cleans the touched lines so LTDC always reads real data. */
  mpu.Enable              = MPU_REGION_ENABLE;
  mpu.Number              = MPU_REGION_NUMBER0;
  mpu.BaseAddress         = LCD_FB_ADDR;
  mpu.Size                = MPU_REGION_SIZE_32MB;
  mpu.SubRegionDisable    = 0x00;
  mpu.TypeExtField        = MPU_TEX_LEVEL0;
  mpu.AccessPermission    = MPU_REGION_FULL_ACCESS;
  mpu.DisableExec         = MPU_INSTRUCTION_ACCESS_DISABLE;
  mpu.IsShareable         = MPU_ACCESS_NOT_SHAREABLE;
  mpu.IsCacheable         = MPU_ACCESS_CACHEABLE;
  mpu.IsBufferable        = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&mpu);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

  /* Drop anything cached before the attribute change */
  SCB_CleanInvalidateDCache();
}

void bsp_lcd_fill(uint16_t color)
{
  uint16_t *fb = (uint16_t *)LCD_FB_ADDR;
  for (uint32_t i = 0; i < (LCD_WIDTH * LCD_HEIGHT); i++)
  {
    fb[i] = color;
  }
  SCB_CleanDCache_by_Addr((uint32_t *)LCD_FB_ADDR, (int32_t)(LCD_WIDTH * LCD_HEIGHT * 2U));
}

uint16_t *bsp_lcd_fb(void)
{
  return (uint16_t *)LCD_FB_ADDR;
}

void bsp_lcd_pll3_config(void)
{
  /* CubeMX sets PLL3R = 5 (25 MHz pixel clock), which only gives ~35 Hz on
     a 1343x634 raster: the panel flickers.  Re-divide PLL3 to 50 MHz
     (25 / 1 * 6 / 3) for a ~59 Hz refresh.  Call this BEFORE MX_LTDC_Init()
     so the pixel clock is final when the LTDC is set up (changing it while
     the LTDC is already running upsets the LTDC).                          */
  RCC_PeriphCLKInitTypeDef p = {0};

  p.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
  p.PLL3.PLL3M       = 1;
  p.PLL3.PLL3N       = 6;
  p.PLL3.PLL3P       = 2;
  p.PLL3.PLL3Q       = 2;
  p.PLL3.PLL3R       = 3;
  p.PLL3.PLL3RGE     = RCC_PLL3VCIRANGE_3;
  p.PLL3.PLL3VCOSEL  = RCC_PLL3VCOMEDIUM;
  p.PLL3.PLL3FRACN   = 0.0;

  (void)HAL_RCCEx_PeriphCLKConfig(&p);
}

void bsp_lcd_init(void)
{
  /* Panel reset + backlight.  Pin names differ between the two CubeMX
     projects this build supports: the old one had RST/DISP, the regenerated
     one only exposes LCD_BK, so probe the macros at compile time.          */
#ifdef RST_Pin
  HAL_GPIO_WritePin(RST_GPIO_Port, RST_Pin, GPIO_PIN_SET);
  HAL_Delay(20);
#endif
#ifdef DISP_Pin
  HAL_GPIO_WritePin(DISP_GPIO_Port, DISP_Pin, GPIO_PIN_SET);
#elif defined(LCD_BK_Pin)
  HAL_GPIO_WritePin(LCD_BK_GPIO_Port, LCD_BK_Pin, GPIO_PIN_SET);
#endif

  /* CubeMX generated a second LTDC layer whose frame buffer sits at
     0xC0200000 and which is *enabled in hardware*.  That buffer is never
     rendered, so the layer blends raw SDRAM noise over layer 1 and the panel
     shows nothing but snow.  __HAL_LTDC_LAYER_DISABLE() only edits the RAM
     config struct (no hardware effect), so clear the register directly.   */
  LTDC_Layer2->CR = 0U;
  hltdc.Instance->SRCR = LTDC_SRCR_IMR;   /* reload immediately */

  bsp_lcd_fill(0x0000);

  lv_init();
  lv_tick_set_cb(bsp_tick_cb);

  s_disp = lv_display_create((int32_t)LCD_WIDTH, (int32_t)LCD_HEIGHT);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, lv_flush_cb);
  lv_display_set_buffers(s_disp, s_draw_buf1, s_draw_buf2,
                         (uint32_t)sizeof(s_draw_buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_default(s_disp);

  /* Drop the FIFO-underrun / transfer-error flags accumulated during the
     boot-time framebuffer clear so that later readbacks show live state.   */
  hltdc.Instance->ICR = 0x0000001FU;
}
