/**
 * siggen_ui.h - LVGL function generator UI
 */
#ifndef APP_SIGGEN_UI_H
#define APP_SIGGEN_UI_H

#include "lvgl.h"

typedef enum
{
  SIGGEN_WAVE_SINE = 0,
  SIGGEN_WAVE_SQUARE,
  SIGGEN_WAVE_TRIANGLE,
  SIGGEN_WAVE_SAWTOOTH,
  SIGGEN_WAVE_COUNT
} siggen_wave_t;

void siggen_ui_create(void);

#endif /* APP_SIGGEN_UI_H */
