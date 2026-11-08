/**
 * siggen_ui.c - LVGL 9.6 user interface for the MS5351M signal generator
 *
 * Layout (1024x600):
 *   header            : title, PLL state, frequency readout
 *   left panel        : 4 waveform buttons (preview selection)
 *   preview panel     : lv_chart with 2 cycles of the selected waveform
 *   parameter panel   : frequency (log slider + fine buttons), amplitude
 *   bottom panel      : offset, output enable switch
 *
 * Only frequency and output on/off reach the hardware: MS5351M produces a
 * square wave.  Shape / amplitude / offset are preview-side parameters.
 */
#include "siggen_ui.h"
#include "ms5351.h"
#include "app.h"
#include "bsp_lcd.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>

#define COL_BG        0x080D14u
#define COL_HEADER    0x0F1724u
#define COL_CARD      0x121C2Bu
#define COL_CARD_HI   0x1A2740u
#define COL_BORDER    0x22314Bu
#define COL_TEXT      0xE8EFF8u
#define COL_MUTED     0x7E93AEu
#define COL_ACCENT    0x18C2FFu
#define COL_ACCENT_2  0xFFA31Au
#define COL_GREEN     0x35D07Fu
#define COL_RED       0xFF5A5Fu

#define CHART_POINTS  360

#define FREQ_MIN_HZ   8000UL
#define FREQ_MAX_HZ   100000000UL
#define FREQ_SLIDER_MAX 1000u

static lv_obj_t *s_btn[SIGGEN_WAVE_COUNT];
static lv_obj_t *s_btn_lbl[SIGGEN_WAVE_COUNT];
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_obj_t *s_freq_slider;
static lv_obj_t *s_amp_slider;
static lv_obj_t *s_off_slider;
static lv_obj_t *s_freq_value;
static lv_obj_t *s_amp_value;
static lv_obj_t *s_off_value;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_head_freq_lbl;
static lv_obj_t *s_mode_lbl;
static lv_obj_t *s_out_sw;

static uint8_t  s_wave = SIGGEN_WAVE_SINE;
static uint16_t s_amp_pct = 60u;
static int16_t  s_off_pct = 0;
static uint16_t s_freq_idx = 500u;
static uint32_t s_freq_hz = 1000000UL;

static const char *const kWaveNames[SIGGEN_WAVE_COUNT] = {
  "SINE", "SQUARE", "TRIANGLE", "SAWTOOTH"
};

/*------------------------------------------------------------------
 * helpers
 *----------------------------------------------------------------*/
static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y,
                            int32_t w, int32_t h)
{
  lv_obj_t *obj = lv_obj_create(parent);

  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_radius(obj, 12, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);

  return obj;
}

static lv_obj_t *make_text(lv_obj_t *parent, const char *txt,
                           const lv_font_t *font, uint32_t color)
{
  lv_obj_t *lbl = lv_label_create(parent);

  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);

  return lbl;
}

static void format_freq(uint32_t f, char *out, size_t n)
{
  if (f >= 1000000UL)
  {
    snprintf(out, n, "%lu.%03lu MHz",
             (unsigned long)(f / 1000000UL),
             (unsigned long)((f / 1000UL) % 1000UL));
  }
  else if (f >= 1000UL)
  {
    snprintf(out, n, "%lu.%03lu kHz",
             (unsigned long)(f / 1000UL),
             (unsigned long)(f % 1000UL));
  }
  else
  {
    snprintf(out, n, "%lu Hz", (unsigned long)f);
  }
}

static uint32_t idx_to_freq(uint16_t idx)
{
  double ratio = (double)FREQ_MAX_HZ / (double)FREQ_MIN_HZ;
  double f = (double)FREQ_MIN_HZ * pow(ratio, (double)idx / (double)FREQ_SLIDER_MAX);

  if (f < (double)FREQ_MIN_HZ)
  {
    f = (double)FREQ_MIN_HZ;
  }
  if (f > (double)FREQ_MAX_HZ)
  {
    f = (double)FREQ_MAX_HZ;
  }

  return (uint32_t)f;
}

static uint16_t freq_to_idx(uint32_t f)
{
  double ratio = (double)FREQ_MAX_HZ / (double)FREQ_MIN_HZ;

  if (f < FREQ_MIN_HZ)
  {
    f = FREQ_MIN_HZ;
  }

  double idx = log((double)f / (double)FREQ_MIN_HZ) / log(ratio) * (double)FREQ_SLIDER_MAX;

  if (idx < 0.0)
  {
    idx = 0.0;
  }
  if (idx > (double)FREQ_SLIDER_MAX)
  {
    idx = (double)FREQ_SLIDER_MAX;
  }

  return (uint16_t)idx;
}

static void build_wave(int32_t *values)
{
  const int32_t n = CHART_POINTS;
  const double two_pi = 6.283185307179586;
  const double cycles = 2.0;

  for (int32_t i = 0; i < n; i++)
  {
    double phase = two_pi * cycles * (double)i / (double)(n - 1);
    double value;

    switch (s_wave)
    {
      case SIGGEN_WAVE_SQUARE:
        value = (sin(phase) >= 0.0) ? 1.0 : -1.0;
        break;

      case SIGGEN_WAVE_TRIANGLE:
        value = 2.0 / two_pi * asin(sin(phase));
        break;

      case SIGGEN_WAVE_SAWTOOTH:
      {
        double t = fmod(phase / two_pi, 1.0);
        value = 2.0 * t - 1.0;
        break;
      }

      case SIGGEN_WAVE_SINE:
      default:
        value = sin(phase);
        break;
    }

    values[i] = (int32_t)(value * (double)s_amp_pct) + (int32_t)s_off_pct;
  }
}

static void chart_update(void)
{
  static int32_t values[CHART_POINTS];

  build_wave(values);
  lv_chart_set_series_values(s_chart, s_series, values, CHART_POINTS);
  lv_chart_refresh(s_chart);
}

static void labels_update(void)
{
  char buf[32];

  format_freq(s_freq_hz, buf, sizeof(buf));
  lv_label_set_text(s_freq_value, buf);
  lv_label_set_text(s_head_freq_lbl, buf);

  snprintf(buf, sizeof(buf), "%u%%", (unsigned int)s_amp_pct);
  lv_label_set_text(s_amp_value, buf);

  snprintf(buf, sizeof(buf), "%+d%%", (int)s_off_pct);
  lv_label_set_text(s_off_value, buf);
}

static void apply_frequency(uint32_t f)
{
  if (ms5351_set_freq(f) == 0)
  {
    s_freq_hz = f;
    s_freq_idx = freq_to_idx(f);
    labels_update();
    lv_label_set_text(s_status_lbl,
                      lv_obj_has_state(s_out_sw, LV_STATE_CHECKED) ? "OUTPUT ON"
                                                                   : "OUTPUT OFF");
    lv_obj_set_style_text_color(s_status_lbl,
                                lv_color_hex(lv_obj_has_state(s_out_sw, LV_STATE_CHECKED)
                                                 ? COL_GREEN : COL_MUTED), 0);
  }
  else
  {
    lv_label_set_text(s_status_lbl, "MS5351M NACK");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(COL_RED), 0);
  }
}

/*------------------------------------------------------------------
 * events
 *----------------------------------------------------------------*/
static void wave_btn_cb(lv_event_t *e)
{
  uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

  s_wave = index;

  for (uint8_t i = 0; i < (uint8_t)SIGGEN_WAVE_COUNT; i++)
  {
    bool sel = (i == index);

    lv_obj_set_style_bg_color(s_btn[i],
                              lv_color_hex(sel ? COL_CARD_HI : COL_CARD), 0);
    lv_obj_set_style_border_color(s_btn[i],
                                  lv_color_hex(sel ? COL_ACCENT : COL_BORDER), 0);
    lv_obj_set_style_text_color(s_btn_lbl[i],
                                lv_color_hex(sel ? COL_ACCENT : COL_TEXT), 0);
  }

  lv_label_set_text(s_mode_lbl, kWaveNames[index]);
  chart_update();
}

static void freq_slider_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_VALUE_CHANGED)
  {
    s_freq_idx = (uint16_t)lv_slider_get_value(s_freq_slider);
    char buf[32];
    format_freq(idx_to_freq(s_freq_idx), buf, sizeof(buf));
    lv_label_set_text(s_freq_value, buf);
    lv_label_set_text(s_head_freq_lbl, buf);
  }
  else if (code == LV_EVENT_RELEASED)
  {
    apply_frequency(idx_to_freq((uint16_t)lv_slider_get_value(s_freq_slider)));
  }
}

static void freq_step_cb(lv_event_t *e)
{
  int32_t step = (int32_t)(intptr_t)lv_event_get_user_data(e);
  int32_t idx = (int32_t)s_freq_idx + step;

  if (idx < 0)
  {
    idx = 0;
  }
  if (idx > (int32_t)FREQ_SLIDER_MAX)
  {
    idx = (int32_t)FREQ_SLIDER_MAX;
  }

  s_freq_idx = (uint16_t)idx;
  lv_slider_set_value(s_freq_slider, idx, LV_ANIM_OFF);

  apply_frequency(idx_to_freq(s_freq_idx));
}

static void amp_slider_cb(lv_event_t *e)
{
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
  {
    s_amp_pct = (uint16_t)lv_slider_get_value(s_amp_slider);
    labels_update();
    chart_update();
  }
}

static void off_slider_cb(lv_event_t *e)
{
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
  {
    s_off_pct = (int16_t)lv_slider_get_value(s_off_slider);
    labels_update();
    chart_update();
  }
}

static void output_sw_cb(lv_event_t *e)
{
  bool on = lv_obj_has_state(s_out_sw, LV_STATE_CHECKED);

  (void)ms5351_output_enable(on ? 1u : 0u);
  lv_label_set_text(s_status_lbl, on ? "OUTPUT ON" : "OUTPUT OFF");
  lv_obj_set_style_text_color(s_status_lbl,
                              lv_color_hex(on ? COL_GREEN : COL_MUTED), 0);
}

static void status_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  uint8_t st = ms5351_get_status();
  bool locked = (ms5351_present() != 0u) && ((st & 0x20u) == 0u);

  g_dbg.si_status = st;
  g_dbg.si_lock = locked ? 1u : 0u;

  if (lv_obj_has_state(s_out_sw, LV_STATE_CHECKED))
  {
    /* The LOL bit of this module reads back stuck on 1 for every VCO /
       crystal combination (see 显示调试结论.md), so the on-screen state
       reports the output switch instead of a lock claim.                  */
    lv_label_set_text(s_status_lbl, "OUTPUT ON");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(COL_GREEN), 0);
  }
}

/*------------------------------------------------------------------
 * construction
 *----------------------------------------------------------------*/
static void create_header(lv_obj_t *scr)
{
  lv_obj_t *bar = lv_obj_create(scr);

  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_size(bar, LCD_WIDTH, 72);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COL_HEADER), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);

  lv_obj_t *title = make_text(bar, "MS5351M SIGNAL GENERATOR",
                              &lv_font_montserrat_28, COL_TEXT);
  lv_obj_set_pos(title, 24, 10);

  lv_obj_t *sub = make_text(bar, "STM32H743  |  CLK0  |  8 kHz - 100 MHz",
                            &lv_font_montserrat_14, COL_MUTED);
  lv_obj_set_pos(sub, 26, 44);

  s_status_lbl = make_text(bar, "READY", &lv_font_montserrat_20, COL_GREEN);
  lv_obj_set_pos(s_status_lbl, 470, 24);

  s_head_freq_lbl = make_text(bar, "1.000 MHz", &lv_font_montserrat_28, COL_ACCENT);
  lv_obj_set_width(s_head_freq_lbl, 300);
  lv_obj_set_style_text_align(s_head_freq_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_head_freq_lbl, 700, 20);
}

static void create_wave_panel(lv_obj_t *scr)
{
  lv_obj_t *panel = make_panel(scr, 20, 88, 236, 496);

  lv_obj_t *cap = make_text(panel, "WAVEFORM", &lv_font_montserrat_14, COL_MUTED);
  lv_obj_set_pos(cap, 16, 12);

  for (uint8_t i = 0; i < (uint8_t)SIGGEN_WAVE_COUNT; i++)
  {
    s_btn[i] = lv_button_create(panel);
    lv_obj_set_pos(s_btn[i], 16, 40 + (int32_t)i * 112);
    lv_obj_set_size(s_btn[i], 204, 96);
    lv_obj_set_style_radius(s_btn[i], 10, 0);
    lv_obj_set_style_bg_color(s_btn[i],
                              lv_color_hex(i == 0u ? COL_CARD_HI : COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_btn[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_btn[i], 2, 0);
    lv_obj_set_style_border_color(s_btn[i],
                                  lv_color_hex(i == 0u ? COL_ACCENT : COL_BORDER), 0);
    lv_obj_set_style_shadow_width(s_btn[i], 0, 0);

    s_btn_lbl[i] = make_text(s_btn[i], kWaveNames[i], &lv_font_montserrat_20,
                             (i == 0u) ? COL_ACCENT : COL_TEXT);
    lv_obj_center(s_btn_lbl[i]);

    lv_obj_add_event_cb(s_btn[i], wave_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
  }
}

static void create_preview_panel(lv_obj_t *scr)
{
  lv_obj_t *panel = make_panel(scr, 272, 88, 732, 300);

  lv_obj_t *cap = make_text(panel, "WAVEFORM PREVIEW", &lv_font_montserrat_14, COL_MUTED);
  lv_obj_set_pos(cap, 16, 10);

  s_mode_lbl = make_text(panel, "SINE", &lv_font_montserrat_20, COL_ACCENT);
  lv_obj_set_width(s_mode_lbl, 200);
  lv_obj_set_style_text_align(s_mode_lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_mode_lbl, 514, 8);

  s_chart = lv_chart_create(panel);
  lv_obj_set_pos(s_chart, 12, 42);
  lv_obj_set_size(s_chart, 708, 246);
  lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_chart, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_chart, 0, 0);
  lv_obj_set_style_radius(s_chart, 8, 0);
  lv_obj_set_style_pad_all(s_chart, 0, 0);
  lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);
  lv_obj_set_style_line_color(s_chart, lv_color_hex(COL_BORDER), LV_PART_MAIN);
  lv_obj_set_style_line_opa(s_chart, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);

  lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(s_chart, CHART_POINTS);
  lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -150, 150);
  lv_chart_set_div_line_count(s_chart, 4, 8);

  s_series = lv_chart_add_series(s_chart, lv_color_hex(COL_ACCENT),
                                 LV_CHART_AXIS_PRIMARY_Y);
}

static void create_param_panel(lv_obj_t *scr)
{
  lv_obj_t *panel = make_panel(scr, 272, 400, 732, 88);

  /* Frequency row */
  lv_obj_t *cap = make_text(panel, "FREQ", &lv_font_montserrat_20, COL_MUTED);
  lv_obj_set_pos(cap, 14, 8);

  s_freq_slider = lv_slider_create(panel);
  lv_obj_set_pos(s_freq_slider, 96, 19);
  lv_obj_set_size(s_freq_slider, 300, 10);
  lv_slider_set_range(s_freq_slider, 0, (int32_t)FREQ_SLIDER_MAX);
  lv_slider_set_value(s_freq_slider, (int32_t)s_freq_idx, LV_ANIM_OFF);
  lv_obj_add_event_cb(s_freq_slider, freq_slider_cb, LV_EVENT_ALL, NULL);

  lv_obj_t *btn_minus = lv_button_create(panel);
  lv_obj_set_pos(btn_minus, 412, 6);
  lv_obj_set_size(btn_minus, 42, 34);
  lv_obj_set_style_radius(btn_minus, 8, 0);
  lv_obj_set_style_bg_color(btn_minus, lv_color_hex(COL_CARD_HI), 0);
  lv_obj_add_event_cb(btn_minus, freq_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
  {
    lv_obj_t *l = make_text(btn_minus, "-", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_center(l);
  }

  lv_obj_t *btn_plus = lv_button_create(panel);
  lv_obj_set_pos(btn_plus, 458, 6);
  lv_obj_set_size(btn_plus, 42, 34);
  lv_obj_set_style_radius(btn_plus, 8, 0);
  lv_obj_set_style_bg_color(btn_plus, lv_color_hex(COL_CARD_HI), 0);
  lv_obj_add_event_cb(btn_plus, freq_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)10);
  {
    lv_obj_t *l = make_text(btn_plus, "+", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_center(l);
  }

  s_freq_value = make_text(panel, "1.000 MHz", &lv_font_montserrat_28, COL_ACCENT);
  lv_obj_set_width(s_freq_value, 220);
  lv_obj_set_style_text_align(s_freq_value, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_freq_value, 500, 2);

  /* Amplitude row */
  lv_obj_t *cap2 = make_text(panel, "AMP", &lv_font_montserrat_20, COL_MUTED);
  lv_obj_set_pos(cap2, 14, 56);

  s_amp_slider = lv_slider_create(panel);
  lv_obj_set_pos(s_amp_slider, 96, 67);
  lv_obj_set_size(s_amp_slider, 300, 10);
  lv_slider_set_range(s_amp_slider, 0, 100);
  lv_slider_set_value(s_amp_slider, (int32_t)s_amp_pct, LV_ANIM_OFF);
  lv_obj_add_event_cb(s_amp_slider, amp_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

  s_amp_value = make_text(panel, "60%", &lv_font_montserrat_20, COL_TEXT);
  lv_obj_set_width(s_amp_value, 220);
  lv_obj_set_style_text_align(s_amp_value, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_amp_value, 500, 56);
}

static void create_bottom_panel(lv_obj_t *scr)
{
  lv_obj_t *panel = make_panel(scr, 272, 494, 732, 90);

  lv_obj_t *cap = make_text(panel, "OFFSET", &lv_font_montserrat_20, COL_MUTED);
  lv_obj_set_pos(cap, 14, 32);

  s_off_slider = lv_slider_create(panel);
  lv_obj_set_pos(s_off_slider, 96, 43);
  lv_obj_set_size(s_off_slider, 300, 10);
  lv_slider_set_range(s_off_slider, -40, 40);
  lv_slider_set_value(s_off_slider, (int32_t)s_off_pct, LV_ANIM_OFF);
  lv_obj_add_event_cb(s_off_slider, off_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

  s_off_value = make_text(panel, "+0%", &lv_font_montserrat_20, COL_TEXT);
  lv_obj_set_width(s_off_value, 120);
  lv_obj_set_style_text_align(s_off_value, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_off_value, 406, 32);

  s_out_sw = lv_switch_create(panel);
  lv_obj_set_pos(s_out_sw, 520, 26);
  lv_obj_set_size(s_out_sw, 92, 44);
  lv_obj_add_state(s_out_sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_out_sw, output_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *cap2 = make_text(panel, "OUTPUT", &lv_font_montserrat_20, COL_TEXT);
  lv_obj_set_pos(cap2, 624, 34);
}

void siggen_ui_create(void)
{
  lv_obj_t *scr = lv_screen_active();

  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  create_header(scr);
  create_wave_panel(scr);
  create_preview_panel(scr);
  create_param_panel(scr);
  create_bottom_panel(scr);

  labels_update();
  chart_update();

  (void)ms5351_set_freq(s_freq_hz);
  (void)ms5351_output_enable(1u);

  lv_timer_create(status_timer_cb, 1000, NULL);
}
