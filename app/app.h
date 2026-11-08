/**
 * app.h - application entry points and a small debug snapshot that can be
 *         inspected through SWD without a serial console.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>

#define APP_DBG_MAGIC 0x53504731u   /* "SPG1" */

typedef struct
{
  uint32_t magic;
  uint32_t lvgl_ok;
  uint32_t flush_cnt;
  uint32_t ui_ok;
  uint32_t touch_addr;
  int32_t  touch_x;
  int32_t  touch_y;
  uint32_t si_ok;
  uint32_t si_lock;
  uint32_t freq_hz;
  uint32_t si_status;
  uint32_t app_loops;
  uint32_t gt_id;      /* raw 4-byte product id read from 0x8140 */
  uint32_t gt_ack;     /* low byte: which reset/addr combo answered      */
  uint32_t si_mask;    /* PLL lock sweep bitmask (vco/xtal combos)        */
  uint32_t gt_scan;    /* background re-probe attempts while addr == 0   */
  uint32_t gt_int_low; /* how often PH7 (touch INT) was sampled low      */
  uint32_t gt_pins;    /* bit0 SCL level, bit1 SDA level, bit2 INT level */
} app_debug_t;

extern volatile app_debug_t g_dbg;

void app_init(void);
void app_process(void);

#endif /* APP_H */
