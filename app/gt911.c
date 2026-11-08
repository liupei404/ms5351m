/**
 * gt911.c - GT911 touch driver, bit-banged I2C (PH6/SCL, PI3/SDA)
 *
 * The pins are re-configured as open-drain outputs here, because CubeMX
 * generates them as push-pull GPIO.  SDA is read back through the input data
 * register, which stays valid while the pin is in open-drain output mode.
 *
 * Address selection follows the GT911 boot protocol: the level driven on INT
 * while RST rises selects the 7-bit slave address (INT low -> 0x5D,
 * INT high -> 0x14).  Both combinations are tried because some panels wire
 * the INT net inverted.
 */
#include "gt911.h"
#include "app.h"
#include "main.h"
#include "stm32h7xx_hal.h"

#define GT911_REG_PRODUCT_ID   0x8140u
#define GT911_REG_TOUCH_STATUS 0x814Eu

/* Pin names differ between the two CubeMX trees this build supports:
   the old project used SCL/SDA/INT/RST, the regenerated one exposes
   T_SCK / T_SDA / T_INT (and has no touch reset pin at all).            */
#if defined(T_SCK_Pin)
#define GT911_PIN_SCL_PORT T_SCK_GPIO_Port
#define GT911_PIN_SCL      T_SCK_Pin
#define GT911_PIN_SDA_PORT T_SDA_GPIO_Port
#define GT911_PIN_SDA      T_SDA_Pin
#define GT911_PIN_INT_PORT T_INT_GPIO_Port
#define GT911_PIN_INT      T_INT_Pin
#else
#define GT911_PIN_SCL_PORT SCL_GPIO_Port
#define GT911_PIN_SCL      SCL_Pin
#define GT911_PIN_SDA_PORT SDA_GPIO_Port
#define GT911_PIN_SDA      SDA_Pin
#define GT911_PIN_INT_PORT INT_GPIO_Port
#define GT911_PIN_INT      INT_Pin
#endif

#if defined(RST_Pin)
#define GT911_PIN_RST_PORT RST_GPIO_Port
#define GT911_PIN_RST      RST_Pin
#define GT911_HAS_RST      1
#else
#define GT911_HAS_RST      0
#endif

static uint8_t s_addr = 0u;

/* Half-period of the bit-banged bus.  20 us = 25 kHz: slow enough for the
   long FPC of a 7" panel with only internal pull-ups (~40 kOhm).          */
#define GT911_DELAY_US 20u

/*------------------------------------------------------------------
 * microsecond delay on DWT cycle counter
 *----------------------------------------------------------------*/
static void delay_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us)
{
  uint32_t ticks = us * (SystemCoreClock / 1000000u);
  uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < ticks)
  {
  }
}

/*------------------------------------------------------------------
 * bit-banged I2C (~40 kHz, internal pull-ups)
 *----------------------------------------------------------------*/
static GPIO_TypeDef *s_scl_port = GT911_PIN_SCL_PORT;
static uint16_t      s_scl_pin  = GT911_PIN_SCL;
static GPIO_TypeDef *s_sda_port = GT911_PIN_SDA_PORT;
static uint16_t      s_sda_pin  = GT911_PIN_SDA;

static void scl_write(uint8_t level);
static void sda_write(uint8_t level);

static void i2c_pin_config(uint8_t swap)
{
  GPIO_InitTypeDef gpio = {0};

  gpio.Mode  = GPIO_MODE_OUTPUT_OD;
  gpio.Pull  = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

  if (swap != 0u)
  {
    s_scl_port = GT911_PIN_SDA_PORT;
    s_scl_pin  = GT911_PIN_SDA;
    s_sda_port = GT911_PIN_SCL_PORT;
    s_sda_pin  = GT911_PIN_SCL;
  }
  else
  {
    s_scl_port = GT911_PIN_SCL_PORT;
    s_scl_pin  = GT911_PIN_SCL;
    s_sda_port = GT911_PIN_SDA_PORT;
    s_sda_pin  = GT911_PIN_SDA;
  }

  gpio.Pin = s_scl_pin;
  HAL_GPIO_Init(s_scl_port, &gpio);

  gpio.Pin = s_sda_pin;
  HAL_GPIO_Init(s_sda_port, &gpio);

  scl_write(1);
  sda_write(1);
}

static void scl_write(uint8_t level)
{
  HAL_GPIO_WritePin(s_scl_port, s_scl_pin,
                    level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void sda_write(uint8_t level)
{
  HAL_GPIO_WritePin(s_sda_port, s_sda_pin,
                    level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t sda_read(void)
{
  return (HAL_GPIO_ReadPin(s_sda_port, s_sda_pin) == GPIO_PIN_SET) ? 1u : 0u;
}

static void i2c_start(void)
{
  sda_write(1);
  scl_write(1);
  delay_us(GT911_DELAY_US);
  sda_write(0);
  delay_us(GT911_DELAY_US);
  scl_write(0);
  delay_us(GT911_DELAY_US);
}

static void i2c_stop(void)
{
  sda_write(0);
  delay_us(GT911_DELAY_US);
  scl_write(1);
  delay_us(GT911_DELAY_US);
  sda_write(1);
  delay_us(GT911_DELAY_US);
}

/* returns 0 when the slave acknowledged the byte */
static uint8_t i2c_write_byte(uint8_t data)
{
  uint8_t nack;

  for (uint8_t i = 0u; i < 8u; i++)
  {
    sda_write((data & 0x80u) ? 1u : 0u);
    delay_us(GT911_DELAY_US);
    scl_write(1);
    delay_us(GT911_DELAY_US);
    scl_write(0);
    delay_us(GT911_DELAY_US);
    data <<= 1;
  }

  sda_write(1);          /* release SDA, let the slave ACK */
  delay_us(GT911_DELAY_US);
  scl_write(1);
  delay_us(GT911_DELAY_US);
  nack = sda_read();     /* 0 = ACK */
  scl_write(0);
  delay_us(GT911_DELAY_US);

  return nack;
}

static uint8_t i2c_read_byte(uint8_t ack)
{
  uint8_t data = 0u;

  sda_write(1);          /* release SDA for the slave to drive it */

  for (uint8_t i = 0u; i < 8u; i++)
  {
    delay_us(GT911_DELAY_US);
    scl_write(1);
    delay_us(GT911_DELAY_US);
    data = (uint8_t)((data << 1) | sda_read());
    scl_write(0);
    delay_us(GT911_DELAY_US);
  }

  sda_write(ack ? 0u : 1u);
  delay_us(GT911_DELAY_US);
  scl_write(1);
  delay_us(GT911_DELAY_US);
  scl_write(0);
  sda_write(1);

  return data;
}

static int gt911_write_reg16(uint8_t addr, uint16_t reg, const uint8_t *buf, uint16_t len)
{
  uint8_t nack;

  i2c_start();
  nack = i2c_write_byte((uint8_t)(addr << 1));
  if (nack == 0u)
  {
    nack = i2c_write_byte((uint8_t)(reg >> 8));
  }
  if (nack == 0u)
  {
    nack = i2c_write_byte((uint8_t)(reg & 0xFFu));
  }
  for (uint16_t i = 0u; (nack == 0u) && (i < len); i++)
  {
    nack = i2c_write_byte(buf[i]);
  }
  i2c_stop();

  return (nack == 0u) ? 0 : 1;
}

static int gt911_read_reg16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
  uint8_t nack;

  i2c_start();
  nack = i2c_write_byte((uint8_t)(addr << 1));
  if (nack == 0u)
  {
    nack = i2c_write_byte((uint8_t)(reg >> 8));
  }
  if (nack == 0u)
  {
    nack = i2c_write_byte((uint8_t)(reg & 0xFFu));
  }
  if (nack != 0u)
  {
    i2c_stop();
    return 1;
  }

  i2c_start();
  nack = i2c_write_byte((uint8_t)((addr << 1) | 0x01u));
  if (nack != 0u)
  {
    i2c_stop();
    return 1;
  }

  for (uint16_t i = 0u; i < len; i++)
  {
    buf[i] = i2c_read_byte((uint8_t)((i + 1u) < len));
  }

  i2c_stop();
  return 0;
}

/*------------------------------------------------------------------
 * GT911 control
 *----------------------------------------------------------------*/
static int gt911_probe(uint8_t addr)
{
  uint8_t id[4] = {0};

  if (gt911_read_reg16(addr, GT911_REG_PRODUCT_ID, id, 4u) != 0)
  {
    return 1;
  }

  g_dbg.gt_id = ((uint32_t)id[0] << 24) | ((uint32_t)id[1] << 16) |
                ((uint32_t)id[2] << 8) | (uint32_t)id[3];

  if ((id[0] == '9') && (id[1] == '1') && (id[2] == '1'))
  {
    return 0;
  }

  return 1;
}

/* Address-only probe: no register access, just an ACK check */
static int gt911_ping(uint8_t addr)
{
  uint8_t nack;

  i2c_start();
  nack = i2c_write_byte((uint8_t)(addr << 1));
  i2c_stop();

  return (nack == 0u) ? 0 : 1;
}

static void gt911_hw_reset(uint8_t int_level)
{
  GPIO_InitTypeDef gpio = {0};

  /* INT driven by the MCU during reset, RST toggled low -> high */
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;

#if GT911_HAS_RST
  gpio.Pin = GT911_PIN_RST;
  HAL_GPIO_Init(GT911_PIN_RST_PORT, &gpio);
#endif

  gpio.Pin = GT911_PIN_INT;
  HAL_GPIO_Init(GT911_PIN_INT_PORT, &gpio);

  HAL_GPIO_WritePin(GT911_PIN_INT_PORT, GT911_PIN_INT,
                    int_level ? GPIO_PIN_SET : GPIO_PIN_RESET);
#if GT911_HAS_RST
  HAL_GPIO_WritePin(GT911_PIN_RST_PORT, GT911_PIN_RST, GPIO_PIN_RESET);
  HAL_Delay(10);
  HAL_GPIO_WritePin(GT911_PIN_RST_PORT, GT911_PIN_RST, GPIO_PIN_SET);
  HAL_Delay(60);
#else
  HAL_Delay(60);
#endif

  /* INT is an output of the GT911 after boot: switch back to input */
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  gpio.Pin  = GT911_PIN_INT;
  HAL_GPIO_Init(GT911_PIN_INT_PORT, &gpio);
}

int gt911_init(void)
{
  static const uint8_t kCombos[4][2] = {
    {0u, 0x5Du},   /* INT low  -> 0x5D */
    {1u, 0x14u},   /* INT high -> 0x14 */
    {0u, 0x14u},   /* inverted wiring fallbacks */
    {1u, 0x5Du}
  };

  uint8_t hit[4] = {0};   /* 0..1 normal wiring, 2..3 swapped wiring */

  delay_init();
  i2c_pin_config(0u);

  /* Keep the EXTI line quiet: touch is polled from LVGL */
  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
  __HAL_GPIO_EXTI_CLEAR_IT(GT911_PIN_INT);

  s_addr = 0u;
  g_dbg.gt_ack = 0u;
  g_dbg.gt_id = 0u;

  /* Snapshot the released bus levels: bit16 = SCL high, bit17 = SDA high */
  if (HAL_GPIO_ReadPin(GT911_PIN_SCL_PORT, GT911_PIN_SCL) == GPIO_PIN_SET)
  {
    g_dbg.gt_ack |= (1u << 16);
  }
  if (HAL_GPIO_ReadPin(GT911_PIN_SDA_PORT, GT911_PIN_SDA) == GPIO_PIN_SET)
  {
    g_dbg.gt_ack |= (1u << 17);
  }

  /* Drive test: prove the two pins really belong to us and that the open
     drain output plus input read-back works.  bits20/21 = SDA drove low /
     read back high, bits22/23 = SCL drove low / read back high.           */
  sda_write(0);
  delay_us(GT911_DELAY_US);
  if (sda_read() == 0u) { g_dbg.gt_ack |= (1u << 20); }
  sda_write(1);
  delay_us(GT911_DELAY_US);
  if (sda_read() != 0u) { g_dbg.gt_ack |= (1u << 21); }
  scl_write(0);
  delay_us(GT911_DELAY_US);
  if (HAL_GPIO_ReadPin(s_scl_port, s_scl_pin) == GPIO_PIN_RESET)
  {
    g_dbg.gt_ack |= (1u << 22);
  }
  scl_write(1);
  delay_us(GT911_DELAY_US);
  if (HAL_GPIO_ReadPin(s_scl_port, s_scl_pin) == GPIO_PIN_SET)
  {
    g_dbg.gt_ack |= (1u << 23);
  }

  /* ACK-only scan of both possible addresses before any reset */
  if (gt911_ping(GT911_I2C_ADDR_5D) == 0)
  {
    g_dbg.gt_ack |= (1u << 4);
  }
  if (gt911_ping(GT911_I2C_ADDR_14) == 0)
  {
    g_dbg.gt_ack |= (1u << 5);
  }

  /* Full 7-bit address scan on the normal wiring, then on swapped wiring */
  for (uint8_t a = 0x08u; a < 0x78u; a++)
  {
    if (gt911_ping(a) == 0)
    {
      if (hit[0] == 0u) { hit[0] = a; }
      else if (hit[1] == 0u) { hit[1] = a; }
    }
  }

  i2c_pin_config(1u);
  if (HAL_GPIO_ReadPin(GT911_PIN_SCL_PORT, GT911_PIN_SCL) == GPIO_PIN_SET)
  {
    g_dbg.gt_ack |= (1u << 18);   /* PH6 (now SDA) high */
  }
  if (HAL_GPIO_ReadPin(GT911_PIN_SDA_PORT, GT911_PIN_SDA) == GPIO_PIN_SET)
  {
    g_dbg.gt_ack |= (1u << 19);   /* PI3 (now SCL) high */
  }
  for (uint8_t a = 0x08u; a < 0x78u; a++)
  {
    if (gt911_ping(a) == 0)
    {
      if (hit[2] == 0u) { hit[2] = a; }
      else if (hit[3] == 0u) { hit[3] = a; }
    }
  }

  g_dbg.gt_id = (uint32_t)hit[0] | ((uint32_t)hit[1] << 8) |
                ((uint32_t)hit[2] << 16) | ((uint32_t)hit[3] << 24);

  i2c_pin_config(0u);

  /* The controller needs well over 100 ms after power-up before it answers
     at all; probing that early just yields NACK on every address.  Wait for
     it instead of giving up (that was the "zero ACK" trap).               */
  for (uint16_t i = 0u; (s_addr == 0u) && (i < 40u); i++)
  {
    if (gt911_ping(GT911_I2C_ADDR_14) == 0)
    {
      s_addr = GT911_I2C_ADDR_14;
      g_dbg.gt_ack |= (1u << 0);
    }
    else if (gt911_ping(GT911_I2C_ADDR_5D) == 0)
    {
      s_addr = GT911_I2C_ADDR_5D;
      g_dbg.gt_ack |= (1u << 0);
    }
    else
    {
      HAL_Delay(50);
    }
  }

  /* Fallback: if it still does not answer, try the four reset/INT combos */
  for (uint8_t i = 0u; (s_addr == 0u) && (i < 4u); i++)
  {
    gt911_hw_reset(kCombos[i][0]);

    if (gt911_probe(kCombos[i][1]) == 0)
    {
      s_addr = kCombos[i][1];
      g_dbg.gt_ack |= (uint32_t)(1u << (i + 1));
    }
    else
    {
      g_dbg.gt_ack |= (uint32_t)(1u << (i + 8));
    }
  }

  g_dbg.touch_addr = s_addr;

  if (s_addr == 0u)
  {
    return 1;
  }

  /* Read the product ID once the address is known (debug + sanity check) */
  {
    uint8_t id[4] = {0};

    if (gt911_read_reg16(s_addr, GT911_REG_PRODUCT_ID, id, 4u) == 0)
    {
      g_dbg.gt_id = ((uint32_t)id[0] << 24) | ((uint32_t)id[1] << 16) |
                    ((uint32_t)id[2] << 8) | (uint32_t)id[3];
    }
  }

  /* Clear any pending touch status left over from power-up */
  {
    uint8_t zero = 0u;
    gt911_write_reg16(s_addr, GT911_REG_TOUCH_STATUS, &zero, 1u);
  }

  return 0;
}

int gt911_read_point(uint16_t *x, uint16_t *y)
{
  uint8_t buf[7] = {0};

  if (s_addr == 0u)
  {
    return 0;
  }

  if (gt911_read_reg16(s_addr, GT911_REG_TOUCH_STATUS, buf, 7u) != 0)
  {
    return 0;
  }

  if ((buf[0] & 0x80u) == 0u)
  {
    return 0;
  }

  /* Clear the "buffer ready" flag for every report, touched or not */
  {
    uint8_t zero = 0u;
    gt911_write_reg16(s_addr, GT911_REG_TOUCH_STATUS, &zero, 1u);
  }

  if ((buf[0] & 0x0Fu) == 0u)
  {
    return 0;
  }

  *x = (uint16_t)(((uint16_t)buf[3] << 8) | buf[2]);
  *y = (uint16_t)(((uint16_t)buf[5] << 8) | buf[4]);

  g_dbg.touch_x = (int32_t)*x;
  g_dbg.touch_y = (int32_t)*y;

  return 1;
}

uint8_t gt911_get_addr(void)
{
  return s_addr;
}

/**
 * Background supervisor, called from the main loop.
 *
 * While the controller has not been detected it keeps re-probing the two
 * valid addresses (the chip may come up later than the one-shot init) and
 * samples the INT line so that a touch can be seen even when I2C does not
 * answer - that separates "chip alive but bus broken" from "chip absent".
 */
void gt911_service(void)
{
  static uint32_t s_last_tick;
  uint8_t scl;
  uint8_t sda;
  uint8_t intr;

  /* Sample the three nets every call (cheap, and gives a live trace) */
  scl  = (HAL_GPIO_ReadPin(GT911_PIN_SCL_PORT, GT911_PIN_SCL) == GPIO_PIN_SET) ? 1u : 0u;
  sda  = (HAL_GPIO_ReadPin(GT911_PIN_SDA_PORT, GT911_PIN_SDA) == GPIO_PIN_SET) ? 1u : 0u;
  intr = (HAL_GPIO_ReadPin(GT911_PIN_INT_PORT, GT911_PIN_INT) == GPIO_PIN_SET) ? 1u : 0u;

  g_dbg.gt_pins = (uint32_t)scl | ((uint32_t)sda << 1) | ((uint32_t)intr << 2);

  if (intr == 0u)
  {
    g_dbg.gt_int_low++;
  }

  if (s_addr != 0u)
  {
    return;
  }

  if ((HAL_GetTick() - s_last_tick) < 200u)
  {
    return;
  }
  s_last_tick = HAL_GetTick();
  g_dbg.gt_scan++;

  if (gt911_ping(GT911_I2C_ADDR_14) == 0)
  {
    s_addr = GT911_I2C_ADDR_14;
  }
  else if (gt911_ping(GT911_I2C_ADDR_5D) == 0)
  {
    s_addr = GT911_I2C_ADDR_5D;
  }

  if (s_addr != 0u)
  {
    uint8_t id[4] = {0};
    uint8_t zero = 0u;

    if (gt911_read_reg16(s_addr, GT911_REG_PRODUCT_ID, id, 4u) == 0)
    {
      g_dbg.gt_id = ((uint32_t)id[0] << 24) | ((uint32_t)id[1] << 16) |
                    ((uint32_t)id[2] << 8) | (uint32_t)id[3];
    }
    gt911_write_reg16(s_addr, GT911_REG_TOUCH_STATUS, &zero, 1u);
    g_dbg.gt_ack |= (1u << 2);   /* found by the background supervisor */
  }

  g_dbg.touch_addr = s_addr;
}
