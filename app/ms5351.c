/**
 * ms5351.c - register level driver for MS5351M / Si5351A (CLK0 only)
 *
 * PLL A is always parked at 900 MHz; CLK0 is derived with an integer/fractional
 * MultiSynth divider, which gives sub-hertz resolution on all output ranges.
 * An output R divider (/1..128) extends the low end down to 8 kHz.
 */
#include "ms5351.h"
#include "i2c_bus.h"
#include "app.h"
#include <string.h>

#define MS5351_I2C_ADDR       (0x60u << 1)

#define REG_DEVICE_STATUS     0u
#define REG_OUTPUT_ENABLE     3u
#define REG_CLK0_CTRL         16u
#define REG_PLL_A_PARAMS      26u
#define REG_MS0_PARAMS        42u
#define REG_PLL_RESET         177u
#define REG_CRYSTAL_LOAD      183u
#define REG_FANOUT_ENABLE     187u

#define PLL_VCO_HZ            900000000ULL
#define FRAC_DENOM            1048575UL

/* Build-time diagnostic: 1 = CLK0 outputs the raw crystal (no PLL), used to
   measure the real reference frequency on a scope.                            */
#ifndef MS5351_XTAL_TEST
#define MS5351_XTAL_TEST      0
#endif

static uint8_t  s_present = 0u;
static uint32_t s_freq_hz = 0u;
static uint32_t s_xtal_hz = MS5351_XTAL_HZ;
static uint32_t s_vco_hz  = (uint32_t)PLL_VCO_HZ;

/* Read-back snapshot for debugging over SWD */
uint8_t g_si_regback[16];

/* Raw device-status byte captured for each of the four crystal load
   capacitance codes, in the order 0x40, 0x80, 0x00, 0xC0.  Lets the SWD
   side see which reference-clock flags flicker as CL is changed.        */
uint8_t g_cl_status[4];

static uint8_t reg_read(uint8_t reg);

static void snapshot_regs(void)
{
  static const uint8_t kRegs[16] = {26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u,
                                    183u, 187u, 3u, 16u, 0u, 1u, 15u, 2u};
  for (uint8_t i = 0u; i < 16u; i++)
  {
    g_si_regback[i] = reg_read(kRegs[i]);
  }
}

static int reg_write(uint8_t reg, uint8_t value)
{
  HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c1, MS5351_I2C_ADDR, reg,
                                           I2C_MEMADD_SIZE_8BIT, &value, 1u, 100u);
  return (st == HAL_OK) ? 0 : 1;
}

static int reg_write_bulk(uint8_t reg, const uint8_t *buf, uint8_t len)
{
  HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c1, MS5351_I2C_ADDR, reg,
                                           I2C_MEMADD_SIZE_8BIT,
                                           (uint8_t *)buf, len, 100u);
  return (st == HAL_OK) ? 0 : 1;
}

static uint8_t reg_read(uint8_t reg)
{
  uint8_t value = 0xFFu;
  (void)HAL_I2C_Mem_Read(&hi2c1, MS5351_I2C_ADDR, reg,
                         I2C_MEMADD_SIZE_8BIT, &value, 1u, 100u);
  return value;
}

/* Pack a MultiSynth / PLL parameter set into the 8 bytes the chip expects.
   For the MS registers byte 2 also carries DIVBY4 and the R divider.       */
static void pack_params(uint8_t *out, uint32_t p1, uint32_t p2, uint32_t p3,
                        uint8_t r_div_code)
{
  out[0] = (uint8_t)((p3 >> 8) & 0xFFu);
  out[1] = (uint8_t)(p3 & 0xFFu);
  out[2] = (uint8_t)((p1 >> 16) & 0x03u) | (uint8_t)((r_div_code & 0x07u) << 4);
  out[3] = (uint8_t)((p1 >> 8) & 0xFFu);
  out[4] = (uint8_t)(p1 & 0xFFu);
  out[5] = (uint8_t)(((p3 >> 12) & 0xF0u) | ((p2 >> 16) & 0x0Fu));
  out[6] = (uint8_t)((p2 >> 8) & 0xFFu);
  out[7] = (uint8_t)(p2 & 0xFFu);
}

static void calc_divider(uint64_t numerator, uint64_t denominator,
                         uint32_t *p1, uint32_t *p2, uint32_t *p3)
{
  uint64_t a = numerator / denominator;
  uint64_t rem = numerator - (a * denominator);

  if (a < 4u)
  {
    a = 4u;
    rem = 0u;
  }
  if (a > 900u)
  {
    a = 900u;
    rem = 0u;
  }

  uint32_t c = FRAC_DENOM;
  uint32_t b = (uint32_t)((rem * c) / denominator);

  if (b >= c)
  {
    a++;
    b = 0u;
  }

  if (b == 0u)
  {
    c = 1u;
  }

  *p3 = c;
  *p1 = (uint32_t)(128u * a + (uint32_t)((128ULL * b) / c) - 512u);
  *p2 = (uint32_t)((128ULL * b) - (uint64_t)c * ((128ULL * b) / c));
}

int ms5351_init(void)
{
  uint8_t status;

  s_present = 0u;
  g_dbg.si_mask = 0u;

  /* Which GPIOB pair actually carries the chip?  Probe the documented pair
     first, then the alternatives; si_mask bits 24..26 record the winner:
     bit24 = SCL PB8 / SDA PB7, bit25 = PB6 / PB7, bit26 = PB8 / PB9.      */
  {
    static const uint16_t kScl[3] = {GPIO_PIN_8, GPIO_PIN_6, GPIO_PIN_8};
    static const uint16_t kSda[3] = {GPIO_PIN_7, GPIO_PIN_7, GPIO_PIN_9};
    uint8_t found = 0u;

    for (uint8_t i = 0u; i < 3u; i++)
    {
      app_i2c_set_pins(kScl[i], kSda[i]);
      HAL_Delay(2);

      if (HAL_I2C_IsDeviceReady(&hi2c1, MS5351_I2C_ADDR, 3u, 20u) == HAL_OK)
      {
        g_dbg.si_mask |= (uint32_t)(1u << (24u + i));
        found = 1u;
        break;
      }
    }

    if (found == 0u)
    {
      g_dbg.si_ok = 0u;
      return 1;
    }
  }

  /* Presence + crystal sweep.  Device status layout (per the Si5351A register
     map): bit7 SYS_INIT, bit6 LOL_B, bit5 LOL_A, bit4 LOS (reference lost),
     bits[1:0] REVID.  LOS must read 0 for the crystal to be running; sweep
     the internal load capacitance and reset the PLLs to find a value that
     starts it.  si_mask bits 0..3 record which CL code cleared LOS.        */
  {
    static const uint8_t kCl[4] = {0x40u, 0x80u, 0x00u, 0xC0u};

    for (uint8_t i = 0u; i < 4u; i++)
    {
      if (reg_write(REG_CRYSTAL_LOAD, kCl[i]) != 0)
      {
        g_dbg.si_ok = 0u;
        return 1;
      }

      (void)reg_write(REG_PLL_RESET, 0xA0u);   /* reset PLLA + PLLB */
      HAL_Delay(10);

      status = reg_read(REG_DEVICE_STATUS);
      g_cl_status[i] = status;
      if ((status & 0x10u) == 0u)
      {
        g_dbg.si_mask |= (uint32_t)(1u << i);
      }
    }

    /* park on the first load cap that started the oscillator */
    for (uint8_t i = 0u; i < 4u; i++)
    {
      if ((g_dbg.si_mask & (uint32_t)(1u << i)) != 0u)
      {
        (void)reg_write(REG_CRYSTAL_LOAD, kCl[i]);
        break;
      }
    }
  }

  s_present = 1u;
  g_dbg.si_ok = 1u;

#if MS5351_XTAL_TEST
  /* Diagnostic image: CLK0 is driven straight from the crystal, no PLL at
     all.  Whatever appears on CLK0 is then the true reference frequency,
     which is the one number every divider calculation depends on.        */
  (void)reg_write(REG_OUTPUT_ENABLE, 0xFEu);   /* CLK0 on, others off */
  (void)reg_write(REG_FANOUT_ENABLE, 0x50u);   /* MS + XTAL fanout   */
  (void)reg_write(REG_CLK0_CTRL, 0x03u);       /* source = XTAL, 8 mA */
  HAL_Delay(5);
  g_dbg.si_status = reg_read(REG_DEVICE_STATUS);
  g_dbg.si_lock   = 0u;

  /* Prove the passthrough configuration really landed in the chip */
  {
    uint8_t v183 = reg_read(REG_CRYSTAL_LOAD);
    uint8_t v187 = reg_read(REG_FANOUT_ENABLE);
    uint8_t v16  = reg_read(REG_CLK0_CTRL);
    uint8_t v3   = reg_read(REG_OUTPUT_ENABLE);

    g_si_regback[0]  = v183;
    g_si_regback[1]  = v187;
    g_si_regback[2]  = v16;
    g_si_regback[3]  = v3;
    g_si_regback[4]  = reg_read(REG_DEVICE_STATUS);
    g_si_regback[5]  = reg_read(15u);   /* PLL input source */
    g_si_regback[6]  = reg_read(1u);
    g_si_regback[7]  = reg_read(2u);
  }

  return 0;
#endif

  /* All outputs off while we set things up */
  (void)reg_write(REG_OUTPUT_ENABLE, 0xFFu);

  /* Multisynth fan-out on (bit4 of reg 187) */
  (void)reg_write(REG_FANOUT_ENABLE, 0x10u);

  /* CLK0: powered down, MS source, 8 mA drive */
  (void)reg_write(REG_CLK0_CTRL, 0x8Fu);

  /* Neither the crystal frequency nor the usable VCO range is known for
     sure, so lock is searched over a small grid.  si_mask bits 4..12 hold
     one bit per (xtal, vco) combination that produced LOL_A = 0.           */
  {
    static const uint32_t kXtal[2] = {25000000UL, 27000000UL};
    static const uint32_t kVco[7]  = {600000000UL, 650000000UL, 700000000UL,
                                      750000000UL, 800000000UL, 850000000UL,
                                      900000000UL};
    uint8_t locked = 0u;

    for (uint8_t xi = 0u; (xi < 2u) && !locked; xi++)
    {
      for (uint8_t vi = 0u; (vi < 7u) && !locked; vi++)
      {
        s_xtal_hz = kXtal[xi];
        s_vco_hz  = kVco[vi];

        (void)ms5351_set_freq(1000000UL);
        HAL_Delay(30);

        status = reg_read(REG_DEVICE_STATUS);
        if ((status & 0x20u) == 0u)          /* LOL_A clear */
        {
          g_dbg.si_mask |= (uint32_t)(1u << (4u + xi * 7u + vi));
          locked = 1u;
        }
      }
    }

    if (!locked)
    {
      /* Nothing locked: leave the most likely setup programmed so the output
         at least reflects the requested divider.                          */
      s_xtal_hz = 25000000UL;
      s_vco_hz  = (uint32_t)PLL_VCO_HZ;
      (void)ms5351_set_freq(1000000UL);
      HAL_Delay(30);
    }
  }

  status = reg_read(REG_DEVICE_STATUS);

  (void)ms5351_output_enable(1u);

  snapshot_regs();

  g_dbg.si_status = status;
  g_dbg.si_lock = ((status & 0x20u) == 0u) ? 1u : 0u;

  return 0;
}

int ms5351_set_freq(uint32_t freq_hz)
{
  uint8_t ms_params[8];
  uint8_t pll_params[8];
  uint32_t p1, p2, p3;
  uint32_t r = 1u;
  uint8_t  r_div_code = 0u;
  uint8_t  int_mode = 0u;

  if (s_present == 0u)
  {
    return 1;
  }

  if (freq_hz < MS5351_OUT_MIN_HZ)
  {
    freq_hz = MS5351_OUT_MIN_HZ;
  }
  if (freq_hz > MS5351_OUT_MAX_HZ)
  {
    freq_hz = MS5351_OUT_MAX_HZ;
  }

  /* Pick the smallest output R divider that keeps the MultiSynth input >= 1 MHz */
  while (((uint64_t)freq_hz * r < 1000000ULL) && (r < 128u))
  {
    r <<= 1;
    r_div_code++;
  }

  uint64_t target = (uint64_t)freq_hz * r;

  calc_divider((uint64_t)s_vco_hz, target, &p1, &p2, &p3);
  if (p3 == 1u)
  {
    int_mode = 1u;
  }
  pack_params(ms_params, p1, p2, p3, r_div_code);

  /* MS registers first, then CLK0 control, then the PLL, then PLL reset */
  if (reg_write_bulk(REG_MS0_PARAMS, ms_params, 8u) != 0)
  {
    return 1;
  }

  (void)reg_write(REG_CLK0_CTRL, (uint8_t)(0x0Fu | (int_mode ? 0x40u : 0x00u)));

  calc_divider((uint64_t)s_vco_hz, (uint64_t)s_xtal_hz, &p1, &p2, &p3);
  pack_params(pll_params, p1, p2, p3, 0u);
  if (reg_write_bulk(REG_PLL_A_PARAMS, pll_params, 8u) != 0)
  {
    return 1;
  }

  (void)reg_write(REG_PLL_RESET, 0xA0u);   /* reset PLLA + PLLB */
  HAL_Delay(10);

  s_freq_hz = freq_hz;
  g_dbg.freq_hz = freq_hz;
  g_dbg.si_lock = (ms5351_get_status() & 0x20u) ? 0u : 1u;

  return 0;
}

int ms5351_output_enable(uint8_t on)
{
  uint8_t reg = reg_read(REG_OUTPUT_ENABLE);

  if (on)
  {
    reg &= (uint8_t)~(1u << 0);
  }
  else
  {
    reg |= (uint8_t)(1u << 0);
  }

  return reg_write(REG_OUTPUT_ENABLE, reg);
}

uint32_t ms5351_get_freq(void)
{
  return s_freq_hz;
}

uint8_t ms5351_get_status(void)
{
  return reg_read(REG_DEVICE_STATUS);
}

uint8_t ms5351_present(void)
{
  return s_present;
}
