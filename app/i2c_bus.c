/**
 * i2c_bus.c - put I2C1 on the pins the MS5351M actually sits on
 *
 * CubeMX generates I2C1 on whatever pins the .ioc says (the reference design
 * uses PH4/PH5).  This board carries the MS5351M on GPIOB, so the handle is
 * re-initialised here.  This code lives in app/ and is therefore untouched by
 * a CubeMX regeneration.
 *
 * I2C1 pin options that matter here (AF4):
 *   SCL = PB6 / PB8      SDA = PB7 / PB9
 * so the plausible pairs are PB6+PB7, PB8+PB7 and PB8+PB9.  Which one is
 * populated depends on the module wiring, so ms5351_init() probes all three
 * and keeps the one that answers.
 */
#include "i2c_bus.h"
#include "gpio.h"

/* ~400 kHz with a 100 MHz I2C kernel clock (PCLK1 = 120 MHz / APB1 div 4) */
#define APP_I2C1_TIMING  0x0090253BU

void app_i2c_set_pins(uint16_t scl_pin, uint16_t sda_pin)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_I2C1_CLK_ENABLE();

  /* Park the bus first so the peripheral never sees a half-formed edge */
  (void)HAL_I2C_DeInit(&hi2c1);

  /* Release the pins CubeMX assigned to I2C1 - they belong to other
     peripherals on this board.                                            */
#ifdef IIC_SCL_Pin
  gpio.Pin  = IIC_SCL_Pin;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(IIC_SCL_GPIO_Port, &gpio);
#endif
#ifdef IIC_SDA_Pin
  gpio.Pin  = IIC_SDA_Pin;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(IIC_SDA_GPIO_Port, &gpio);
#endif

  gpio.Pin       = scl_pin | sda_pin;
  gpio.Mode      = GPIO_MODE_AF_OD;
  gpio.Pull      = GPIO_PULLUP;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(GPIOB, &gpio);

  hi2c1.Instance              = I2C1;
  hi2c1.Init.Timing           = APP_I2C1_TIMING;
  hi2c1.Init.OwnAddress1      = 0;
  hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2      = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;

  (void)HAL_I2C_Init(&hi2c1);
}

void app_i2c_init(void)
{
  /* Default: the pair the board documentation labels SDA/SCL */
  app_i2c_set_pins(GPIO_PIN_8, GPIO_PIN_7);   /* SCL = PB8, SDA = PB7 */
}
