/*
 * Copyright (c) 2020 Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nuvoton_npcm4xx_i2c

#include <drivers/clock_control.h>
#include <dt-bindings/i2c/i2c.h>
#include <drivers/i2c.h>
#include <soc.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(i2c_npcm4xx, LOG_LEVEL_ERR);

/* Configure TX and RX buffer size for I2C DMA
 * This setting applies to all (12c1a, 12c1b, 12c2a ...)
 */
#define CONFIG_I2C_MAX_TX_SIZE 256
#define CONFIG_I2C_MAX_RX_SIZE 256

/* Default maximum time we allow for an I2C transfer (unit:ms) */
#define I2C_TRANS_TIMEOUT K_MSEC(500)

/* Default max waiting time for i2c ready (unit:ms) */
#define I2C_WAITING_TIME K_MSEC(1000)

/* Hardware Timeout configuration (unit:ms) */
#define CONFIG_MASTER_HW_TIMEOUT_EN 'N'
#define CONFIG_MASTER_HW_TIMEOUT_CLK_LOW_TIME 25
#define CONFIG_MASTER_HW_TIMEOUT_CLK_CYCLE_TIME 50

/* when using Quick command (SMBUS), do not enable slave timeout */
#define CONFIG_SLAVE_HW_TIMEOUT_EN 'N'
#define CONFIG_SLAVE_HW_TIMEOUT_CLK_LOW_TIME 25
#define CONFIG_SLAVE_HW_TIMEOUT_CLK_CYCLE_TIME 50

/* Data abort timeout */
#define ABORT_TIMEOUT 10000

/* I2C operation state */
enum i2c_npcm4xx_oper_state {
	I2C_NPCM4XX_OPER_STA_IDLE,
	I2C_NPCM4XX_OPER_STA_START,
	I2C_NPCM4XX_OPER_STA_WRITE,
	I2C_NPCM4XX_OPER_STA_READ,
	I2C_NPCM4XX_OPER_STA_QUICK,
};

/* Device configuration */
struct i2c_npcm4xx_config {
	uintptr_t base;                 /* i2c controller base address */
	struct npcm4xx_clk_cfg clk_cfg; /* clock configuration */
	uint32_t default_bitrate;
	uint8_t irq;                    /* i2c controller irq */
};

/*rx_buf and tx_buf address must 4-align for DMA */
#pragma pack(4)
struct i2c_npcm4xx_data {
	struct k_sem lock_sem;  /* mutex of i2c controller */
	struct k_sem sync_sem;  /* semaphore used for synchronization */
	enum i2c_npcm4xx_oper_state master_oper_state;
	enum i2c_npcm4xx_oper_state slave_oper_state;
	uint32_t bitrate;
	uint32_t source_clk;
	uint16_t rx_cnt;
	uint16_t tx_cnt;
	uint8_t dev_addr; /* device address (8 bits) */
	uint8_t rx_buf[CONFIG_I2C_MAX_TX_SIZE];
	uint8_t tx_buf[CONFIG_I2C_MAX_RX_SIZE];
	uint8_t *rx_msg_buf;
	int err_code;
	struct i2c_slave_config *slave_cfg;
};
#pragma pack()

/* Driver convenience defines */
#define I2C_DRV_CONFIG(dev) ((const struct i2c_npcm4xx_config *)(dev)->config)

#define I2C_DRV_DATA(dev) ((struct i2c_npcm4xx_data *)(dev)->data)

#define I2C_INSTANCE(dev) (struct i2c_reg *)(I2C_DRV_CONFIG(dev)->base)


/* This macro should be set only when in Master mode or when requesting Master mode.
 * Set START bit to CTL1 register of I2C module, but exclude STOP bit, ACK bit.
 */
static inline void i2c_npcm4xx_start(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	inst->SMBnCTL1 = (inst->SMBnCTL1 & ~0x13) | BIT(NPCM4XX_SMBnCTL1_START);
}

static inline void i2c_npcm4xx_stop(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	(inst)->SMBnCTL1 = ((inst)->SMBnCTL1 & ~0x13) | BIT(NPCM4XX_SMBnCTL1_STOP);
}

static inline void i2c_npcm4xx_enable_stall(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	inst->SMBnCTL1 = (inst->SMBnCTL1 & ~0x13) | BIT(NPCM4XX_SMBnCTL1_STASTRE);
}

static inline void i2c_npcm4xx_disable_stall(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	inst->SMBnCTL1 = inst->SMBnCTL1 & ~(0x13 | BIT(NPCM4XX_SMBnCTL1_STASTRE));
}

static inline void i2c_npcm4xx_nack(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	inst->SMBnCTL1 = (inst->SMBnCTL1 & ~0x13) | BIT(NPCM4XX_SMBnCTL1_ACK);
}

static void i2c_npcm4xx_start_DMA(const struct device *dev,
				  uint32_t addr, uint16_t len)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	/* address */
	inst->DMA_ADDR1 = (uint8_t)((addr) >> 0);
	inst->DMA_ADDR2 = (uint8_t)((addr) >> 8);
	inst->DMA_ADDR3 = (uint8_t)((addr) >> 16);
	inst->DMA_ADDR4 = (uint8_t)((addr) >> 24);

	/* length */
	inst->DATA_LEN1 = (uint8_t)((len) >> 0);
	inst->DATA_LEN2 = (uint8_t)((len) >> 8);

	/* clear interrupt bit */
	inst->DMA_CTRL = BIT(NPCM4XX_DMA_CTRL_DMA_INT_CLR);

	/* Set DMA enable */
	inst->DMA_CTRL = BIT(NPCM4XX_DMA_CTRL_DMA_EN);
}

/* return Negative Acknowledge when DMA received last byte. */
static void i2c_npcm4xx_DMA_lastbyte(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	inst->DMA_CTRL = (inst->DMA_CTRL & ~BIT(NPCM4XX_DMA_CTRL_DMA_INT_CLR)) |
			 BIT(NPCM4XX_DMA_CTRL_LAST_PEC);
}

static uint16_t i2c_npcm4xx_get_dma_cnt(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	return (uint16_t)(((uint16_t)(inst->DATA_CNT1) << 8) + (uint16_t)(inst->DATA_CNT2));

}

void Set_Cumulative_ClockLow_Timeout(const struct device *dev, uint8_t interval_ms)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	uint8_t div;

	div = data->source_clk / 1000 / 1000;
	div = div - 1;
	if ((div < 0x03) || (div > 0x3F)) {
		return;
	}
	inst->SMBnCTL2 |= BIT(NPCM4XX_SMBnCTL2_ENABLE);
	inst->TIMEOUT_EN = (div << NPCM4XX_TIMEOUT_EN_TO_CKDIV);
	inst->TIMEOUT_ST |= BIT(NPCM4XX_TIMEOUT_ST_T_OUTST1_EN);
	inst->TIMEOUT_ST = BIT(NPCM4XX_TIMEOUT_ST_T_OUTST1);
	inst->TIMEOUT_CTL2 = interval_ms;
}

void Set_Cumulative_ClockCycle_Timeout(const struct device *dev, uint8_t interval_ms)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	uint8_t div;

	div = data->source_clk / 1000 / 1000;
	div = div - 1;
	if ((div < 0x03) || (div > 0x3F)) {
		return;
	}
	inst->SMBnCTL2 |= BIT(NPCM4XX_SMBnCTL2_ENABLE);
	inst->TIMEOUT_EN = (div << NPCM4XX_TIMEOUT_EN_TO_CKDIV);
	inst->TIMEOUT_ST |= BIT(NPCM4XX_TIMEOUT_ST_T_OUTST2_EN);
	inst->TIMEOUT_ST = BIT(NPCM4XX_TIMEOUT_ST_T_OUTST2);
	inst->TIMEOUT_CTL1 = interval_ms;
}

static void i2c_npcm4xx_reset_module(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	uint32_t ctl1_tmp;
	uint32_t timeout_en_tmp;

	ctl1_tmp = inst->SMBnCTL1;
	timeout_en_tmp = inst->TIMEOUT_EN;

	/* Disable and then Enable I2C module */
	inst->SMBnCTL2 &= ~BIT(NPCM4XX_SMBnCTL2_ENABLE);
	inst->SMBnCTL2 |= BIT(NPCM4XX_SMBnCTL2_ENABLE);

	inst->SMBnCTL1 = (ctl1_tmp & (BIT(NPCM4XX_SMBnCTL1_INTEN) | BIT(NPCM4XX_SMBnCTL1_EOBINTE) |
				      BIT(NPCM4XX_SMBnCTL1_GCMEN) | BIT(NPCM4XX_SMBnCTL1_NMINTE)));
	inst->TIMEOUT_EN = timeout_en_tmp;

	data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
}

static void i2c_npcm4xx_abort_data(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	uint16_t timeout = ABORT_TIMEOUT;

	/* Generate a STOP condition */
	i2c_npcm4xx_stop(dev);

	/* Clear NEGACK, STASTR and BER bits */
	inst->SMBnST = (BIT(NPCM4XX_SMBnST_STASTR) |
			BIT(NPCM4XX_SMBnST_NEGACK) |
			BIT(NPCM4XX_SMBnST_BER));

	/* Wait till STOP condition is generated */
	while (--timeout) {
		if ((inst->SMBnCTL1 & BIT(NPCM4XX_SMBnCTL1_STOP)) == 0x00) {
			break;
		}
	}

	/* Clear BB (BUS BUSY) bit */
	inst->SMBnCST = BIT(NPCM4XX_SMBnCST_BB);
}

static uint16_t i2c_npcm4xx_get_DMA_cnt(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	return (uint16_t)(((uint16_t)(inst->DATA_CNT1) << 8) + (uint16_t)(inst->DATA_CNT2));
}

static void i2c_npcm4xx_set_baudrate(const struct device *dev, uint32_t bus_freq)
{
	uint32_t reg_tmp;
	const struct i2c_npcm4xx_config *const config = I2C_DRV_CONFIG(dev);
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	const struct device *const clk_dev = device_get_binding(NPCM4XX_CLK_CTRL_NAME);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);

	if (clock_control_get_rate(clk_dev, (clock_control_subsys_t *)&config->clk_cfg,
				   &data->source_clk) != 0) {
		LOG_ERR("Get %s clock source.", dev->name);
	}

	LOG_DBG("i2c clock source: %d", data->source_clk);

	reg_tmp = (data->source_clk) / (bus_freq * 4);

	if (bus_freq < 400000) {
		if (reg_tmp < 8) {
			reg_tmp = 8;
		} else if (reg_tmp > 511) {
			reg_tmp = 511;
		}

		/* Disable fast mode and fast mode plus */
		inst->SMBnCTL3 &= ~BIT(NPCM4XX_SMBnCTL3_400K_MODE);
		SET_FIELD(inst->SMBnCTL2, NPCM4XX_SMBnCTL2_SCLFRQ60_FIELD, reg_tmp & 0x7f);
		SET_FIELD(inst->SMBnCTL3, NPCM4XX_SMBnCTL3_SCLFRQ87_FIELD, reg_tmp >> 7);

		/* Set HLDT (48MHz, HLDT = 17, Hold Time = 360ns) */
		if (data->source_clk >= 40000000) {
			inst->SMBnCTL4 = 17;
		} else if (data->source_clk >= 20000000) {
			inst->SMBnCTL4 = 9;
		} else {
			inst->SMBnCTL4 = 7;
		}
	} else {
		if (reg_tmp < 5) {
			reg_tmp = 5;
		} else if (reg_tmp > 255) {
			reg_tmp = 255;
		}

		/* Enable fast mode and fast mode plus */
		inst->SMBnCTL3 |= BIT(NPCM4XX_SMBnCTL3_400K_MODE);
		SET_FIELD(inst->SMBnCTL2, NPCM4XX_SMBnCTL2_SCLFRQ60_FIELD, 0);
		SET_FIELD(inst->SMBnCTL3, NPCM4XX_SMBnCTL3_SCLFRQ87_FIELD, 0);
		inst->SMBnSCLHT = reg_tmp - 3;
		inst->SMBnSCLLT = reg_tmp - 1;

		/* Set HLDT (48MHz, HLDT = 17, Hold Time = 360ns) */
		if (data->source_clk >= 40000000) {
			inst->SMBnCTL4 = 17;
		} else if (data->source_clk >= 20000000) {
			inst->SMBnCTL4 = 9;
		} else {
			inst->SMBnCTL4 = 7;
		}
	}
}

static void i2c_npcm4xx_notify(const struct device *dev, int error)
{
#if (CONFIG_MASTER_HW_TIMEOUT_EN == 'Y')
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
#endif
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);

#if (CONFIG_MASTER_HW_TIMEOUT_EN == 'Y')
	/* Disable HW Timeout */
	inst->TIMEOUT_EN &= ~BIT(NPCM4XX_TIMEOUT_EN_TIMEOUT_EN);
#endif

	data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
	data->err_code = error;

	k_sem_give(&data->sync_sem);
}

static int i2c_npcm4xx_wait_completion(const struct device *dev)
{
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	int ret;

	ret = k_sem_take(&data->sync_sem, I2C_TRANS_TIMEOUT);
	if (ret != 0) {
		i2c_npcm4xx_reset_module(dev);
		data->err_code = -ETIMEDOUT;
	}

	return data->err_code;
}

/* NPCX specific I2C controller functions */
static int i2c_npcm4xx_mutex_lock(const struct device *dev, k_timeout_t timeout)
{
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);

	return k_sem_take(&data->lock_sem, timeout);
}

static void i2c_npcm4xx_mutex_unlock(const struct device *dev)
{
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);

	k_sem_give(&data->lock_sem);
}

static void i2c_npcm4xx_master_isr(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);

	/* --------------------------------------------- */
	/* Timeout occurred                              */
	/* --------------------------------------------- */
#if (CONFIG_MASTER_HW_TIMEOUT_EN == 'Y')
	if (inst->TIMEOUT_ST & BIT(NPCM4XX_TIMEOUT_ST_T_OUTST1)) {
		/* clear timeout flag */
		inst->TIMEOUT_ST = BIT(NPCM4XX_TIMEOUT_ST_T_OUTST1);
		i2c_npcm4xx_reset_module(dev);
		i2c_npcm4xx_notify(dev, -ETIMEDOUT);
	}
	if (inst->TIMEOUT_ST & BIT(NPCM4XX_TIMEOUT_ST_T_OUTST2)) {
		/* clear timeout flag */
		inst->TIMEOUT_ST = BIT(NPCM4XX_TIMEOUT_ST_T_OUTST2);
		i2c_npcm4xx_reset_module(dev);
		i2c_npcm4xx_notify(dev, -ETIMEDOUT);
	}
#endif

	/* --------------------------------------------- */
	/* NACK occurred                                 */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_NEGACK)) {
		i2c_npcm4xx_abort_data(dev);
		/* Clear DMA flag */
		inst->DMA_CTRL = BIT(NPCM4XX_DMA_CTRL_DMA_INT_CLR);
		data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
		i2c_npcm4xx_notify(dev, -ENXIO);
	}

	/* --------------------------------------------- */
	/* BUS ERROR occurred                            */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_BER)) {
		i2c_npcm4xx_abort_data(dev);
		data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
		i2c_npcm4xx_reset_module(dev);
		i2c_npcm4xx_notify(dev, -EAGAIN);
	}

	/* --------------------------------------------- */
	/* SDA status is set - transmit or receive       */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_SDAST)) {
		if (data->master_oper_state == I2C_NPCM4XX_OPER_STA_START) {
			if (data->tx_cnt == 0 && data->rx_cnt == 0) {
				/* Quick command (SMBUS protocol) */
				data->master_oper_state = I2C_NPCM4XX_OPER_STA_QUICK;
				i2c_npcm4xx_enable_stall(dev);
				/* quick read or quick write is determined by address */
				inst->SMBnSDA = data->dev_addr;
			} else if (data->tx_cnt == 0 && data->rx_cnt > 0) {
				/* receive mode */
				data->master_oper_state = I2C_NPCM4XX_OPER_STA_READ;
				i2c_npcm4xx_enable_stall(dev);
				/* send read address */
				inst->SMBnSDA = data->dev_addr | 0x1;
			} else if (data->tx_cnt > 0) {
				/* transmit mode */
				data->master_oper_state = I2C_NPCM4XX_OPER_STA_WRITE;
				/* send write address */
				inst->SMBnSDA = data->dev_addr & 0xFE;
			}
		} else if (data->master_oper_state == I2C_NPCM4XX_OPER_STA_WRITE) {
			/* Set DMA register to send data */
			i2c_npcm4xx_start_DMA(dev, (uint32_t)data->tx_buf, data->tx_cnt);
		} else {
			/* Error */
		}
	}

	/* --------------------------------------------- */
	/* stall occurred                                */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_STASTR)) {
		if (data->master_oper_state == I2C_NPCM4XX_OPER_STA_READ) {
			/* Set DMA register to read data */
			i2c_npcm4xx_DMA_lastbyte(dev);
			i2c_npcm4xx_start_DMA(dev, (uint32_t)data->rx_buf, data->rx_cnt);
		} else if (data->master_oper_state == I2C_NPCM4XX_OPER_STA_QUICK) {
			i2c_npcm4xx_stop(dev);
			data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
			i2c_npcm4xx_notify(dev, 0);
		} else {
			/* error */
		}

		i2c_npcm4xx_disable_stall(dev);
		/* clear STASTR flag */
		inst->SMBnST = BIT(NPCM4XX_SMBnST_STASTR);
	}

	/* --------------------------------------------- */
	/* DMA IRQ occurred                              */
	/* --------------------------------------------- */
	if (inst->DMA_CTRL & BIT(NPCM4XX_DMA_CTRL_DMA_IRQ)) {
		if (data->master_oper_state == I2C_NPCM4XX_OPER_STA_WRITE) {
			/* Transmit mode */
			if (data->rx_cnt == 0) {
				/* no need to receive data */
				i2c_npcm4xx_stop(dev);
				data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
				i2c_npcm4xx_notify(dev, 0);
			} else {
				data->master_oper_state = I2C_NPCM4XX_OPER_STA_READ;
				i2c_npcm4xx_enable_stall(dev);
				i2c_npcm4xx_start(dev);
				inst->SMBnSDA = (data->dev_addr | 0x1);
			}
		} else {
			/* received mode */
			i2c_npcm4xx_stop(dev);
			data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
			data->rx_cnt = i2c_npcm4xx_get_DMA_cnt(dev);
			i2c_npcm4xx_notify(dev, 0);
		}
		/* Clear DMA flag */
		inst->DMA_CTRL = BIT(NPCM4XX_DMA_CTRL_DMA_INT_CLR);
	}
}

static void i2c_npcm4xx_slave_isr(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	const struct i2c_slave_callbacks *slave_cb =
		data->slave_cfg->callbacks;
	uint16_t len;
	uint8_t overflow_data;


#if (CONFIG_SLAVE_HW_TIMEOUT_EN == 'Y')
	/* --------------------------------------------- */
	/* Timeout occurred                              */
	/* --------------------------------------------- */
	if (inst->TIMEOUT_ST & BIT(NPCM4XX_TIMEOUT_ST_T_OUTST1)) {
		LOG_ERR("slave: HW timeout");
		data->slave_oper_state = I2C_NPCM4XX_OPER_STA_START;
		inst->TIMEOUT_ST = BIT(NPCM4XX_TIMEOUT_ST_T_OUTST1);
		i2c_npcm4xx_reset_module(dev);
	}
	if (inst->TIMEOUT_ST & BIT(NPCM4XX_TIMEOUT_ST_T_OUTST2)) {
		LOG_ERR("slave: HW timeout");
		data->slave_oper_state = I2C_NPCM4XX_OPER_STA_START;
		inst->TIMEOUT_ST = BIT(NPCM4XX_TIMEOUT_ST_T_OUTST2);
		i2c_npcm4xx_reset_module(dev);
	}
#endif

	/* --------------------------------------------- */
	/* NACK occurred                                 */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_NEGACK)) {
		inst->SMBnST = BIT(NPCM4XX_SMBnST_NEGACK);
	}

	/* --------------------------------------------- */
	/* BUS ERROR occurred                            */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_BER)) {
		if (data->slave_oper_state != I2C_NPCM4XX_OPER_STA_QUICK) {
			LOG_ERR("slave: bus error");
		}
		data->slave_oper_state = I2C_NPCM4XX_OPER_STA_START;
		/* clear BER */
		inst->SMBnST = BIT(NPCM4XX_SMBnST_BER);
		i2c_npcm4xx_reset_module(dev);
	}

	/* --------------------------------------------- */
	/* DMA IRQ occurred                              */
	/* --------------------------------------------- */
	if (inst->DMA_CTRL & BIT(NPCM4XX_DMA_CTRL_DMA_IRQ)) {
		if (data->slave_oper_state == I2C_NPCM4XX_OPER_STA_READ) {
			/* if DMA overflow, send NACK to Master, and next IRQ is SDAST alert */
			i2c_npcm4xx_nack(dev);
		}
		/* clear DMA flag */
		inst->DMA_CTRL = BIT(NPCM4XX_DMA_CTRL_DMA_INT_CLR);
	}

	/* --------------------------------------------- */
	/* Address match occurred                        */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_NMATCH)) {
		if (inst->SMBnST & BIT(NPCM4XX_SMBnST_XMIT)) {
			/* slave received Read-Address */
			if (data->slave_oper_state != I2C_NPCM4XX_OPER_STA_START) {
				/* slave received data before */
				len = 0;
				while (len < i2c_npcm4xx_get_dma_cnt(dev)) {
					slave_cb->write_received(data->slave_cfg,
								 data->rx_buf[len]);
					len++;
				}
			}

			/* prepare tx data */
			if (slave_cb->read_requested(data->slave_cfg, data->tx_buf) == 0) {
				len = 0;
				while (++len < sizeof(data->tx_buf)) {
					if (slave_cb->read_processed(data->slave_cfg,
							data->tx_buf + len) != 0) {
						break;
					}
				}
				data->tx_cnt = len;
			} else {
				/* slave has no data to send */
				data->tx_cnt = 0;
			}

			data->slave_oper_state = I2C_NPCM4XX_OPER_STA_WRITE;

			if (data->tx_cnt != 0) {
				/* Set DMA register to send data */
				i2c_npcm4xx_start_DMA(dev, (uint32_t)data->tx_buf, data->tx_cnt);
			} else {
				data->slave_oper_state = I2C_NPCM4XX_OPER_STA_QUICK;
			}
		} else {
			/* slave received Write-Address */
			data->slave_oper_state = I2C_NPCM4XX_OPER_STA_READ;
			/* Set DMA register to get data */
			i2c_npcm4xx_start_DMA(dev, (uint32_t)data->rx_buf, sizeof(data->rx_buf));
			slave_cb->write_requested(data->slave_cfg);
		}
		/* Clear address match bit & SDA pull high */
		inst->SMBnST = BIT(NPCM4XX_SMBnST_NMATCH);
	}

	/* --------------------------------------------- */
	/* SDA status is set - transmit or receive       */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_SDAST)) {
		if (data->slave_oper_state == I2C_NPCM4XX_OPER_STA_READ) {
			/* Over Flow */
			overflow_data = inst->SMBnSDA;

			len = 0;
			while (len < i2c_npcm4xx_get_dma_cnt(dev)) {
				if (slave_cb->write_received(data->slave_cfg, data->rx_buf[len])) {
					break;
				}
				len++;
			}
			slave_cb->write_received(data->slave_cfg, overflow_data);
			data->slave_oper_state = I2C_NPCM4XX_OPER_STA_START;
		} else {
			/* No Enough DMA data to send */
			inst->SMBnSDA = 0xFF;
		}
	}

	/* --------------------------------------------- */
	/* Slave STOP occurred                           */
	/* --------------------------------------------- */
	if (inst->SMBnST & BIT(NPCM4XX_SMBnST_SLVSTP)) {
		if (data->slave_oper_state == I2C_NPCM4XX_OPER_STA_READ) {
			len = 0;
			while (len < i2c_npcm4xx_get_dma_cnt(dev)) {
				slave_cb->write_received(data->slave_cfg, data->rx_buf[len]);
				len++;
			}
		}
		if (data->slave_oper_state != I2C_NPCM4XX_OPER_STA_IDLE) {
			slave_cb->stop(data->slave_cfg);
		}
		data->slave_oper_state = I2C_NPCM4XX_OPER_STA_START;
		/* clear STOP flag */
		inst->SMBnST = BIT(NPCM4XX_SMBnST_SLVSTP);
	}
}

static void i2c_set_slave_addr(const struct device *dev, uint8_t slave_addr)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);

	/* set slave addr 1 */
	inst->SMBnADDR1 = (slave_addr | BIT(NPCM4XX_SMBnADDR_SAEN));

	/* Enable I2C address match interrupt */
	inst->SMBnCTL1 |= BIT(NPCM4XX_SMBnCTL1_NMINTE);
}


static int i2c_npcm4xx_slave_register(const struct device *dev,
				      struct i2c_slave_config *cfg)
{
	struct i2c_npcm4xx_data *data = I2C_DRV_DATA(dev);
	int ret = 0;

	if (!cfg) {
		return -EINVAL;
	}
	if (cfg->flags & I2C_SLAVE_FLAGS_ADDR_10_BITS) {
		return -ENOTSUP;
	}
	if (i2c_npcm4xx_mutex_lock(dev, I2C_WAITING_TIME) != 0) {
		return -EBUSY;
	}

	if (data->slave_cfg) {
		/* slave is already registered */
		ret = -EBUSY;
		goto exit;
	}

	data->slave_cfg = cfg;
	data->slave_oper_state = I2C_NPCM4XX_OPER_STA_START;
	/* set slave addr, cfg->address is 7 bit address */
	i2c_set_slave_addr(dev, cfg->address);

exit:
	i2c_npcm4xx_mutex_unlock(dev);
	return ret;
}

static int i2c_npcm4xx_slave_unregister(const struct device *dev,
					struct i2c_slave_config *config)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *data = I2C_DRV_DATA(dev);


	if (!data->slave_cfg) {
		return -EINVAL;
	}

	if (data->slave_oper_state != I2C_NPCM4XX_OPER_STA_START &&
	    data->master_oper_state != I2C_NPCM4XX_OPER_STA_IDLE) {
		return -EBUSY;
	}

	if (i2c_npcm4xx_mutex_lock(dev, I2C_WAITING_TIME) != 0) {
		return -EBUSY;
	}

	/* clear slave addr 1 */
	inst->SMBnADDR1 = 0;

	/* Disable I2C address match interrupt */
	inst->SMBnCTL1 &= ~BIT(NPCM4XX_SMBnCTL1_NMINTE);

	/* clear all interrupt status */
	inst->SMBnST = 0xFF;

	data->slave_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;
	data->slave_cfg = 0;

	i2c_npcm4xx_mutex_unlock(dev);

	return 0;
}

/* I2C controller isr function */
static void i2c_npcm4xx_isr(const struct device *dev)
{
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
	struct i2c_npcm4xx_data *data = I2C_DRV_DATA(dev);

	if (data->master_oper_state != I2C_NPCM4XX_OPER_STA_IDLE) {
		i2c_npcm4xx_master_isr(dev);
	} else {
		if (data->slave_oper_state == I2C_NPCM4XX_OPER_STA_IDLE) {
			/* clear all interrupt status */
			inst->SMBnST = 0xFF;
		} else {
			i2c_npcm4xx_slave_isr(dev);
		}
	}
}


/* I2C controller driver registration */
static int i2c_npcm4xx_init(const struct device *dev)
{
	int ret;
	const struct i2c_npcm4xx_config *const config = I2C_DRV_CONFIG(dev);
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	const struct device *const clk_dev = device_get_binding(NPCM4XX_CLK_CTRL_NAME);
	struct i2c_reg *const inst = I2C_INSTANCE(dev);



	LOG_DBG("Device name: %s", dev->name);


	/* Turn on device clock first and get source clock freq. */
	if (clock_control_on(clk_dev, (clock_control_subsys_t *) &config->clk_cfg) != 0) {
		LOG_ERR("Turn on %s clock fail.", dev->name);
		return -EIO;
	}

	/* reset data */
	gdma_memset_u8((uint8_t *)data, 0, sizeof(struct i2c_npcm4xx_data));

	/* Set default baud rate for i2c*/
	data->bitrate = config->default_bitrate;
	LOG_DBG("bitrate: %d", data->bitrate);
	i2c_npcm4xx_set_baudrate(dev, data->bitrate);

	/* Enable I2C module */
	inst->SMBnCTL2 |= BIT(NPCM4XX_SMBnCTL2_ENABLE);
	/* Enable I2C interrupt */
	inst->SMBnCTL1 |= BIT(NPCM4XX_SMBnCTL1_INTEN);

	/* initialize mutux and semaphore for i2c controller */
	ret = k_sem_init(&data->lock_sem, 1, 1);
	ret = k_sem_init(&data->sync_sem, 0, 1);

	/* Initialize driver status machine */
	data->master_oper_state = I2C_NPCM4XX_OPER_STA_IDLE;

	return 0;
}

static int i2c_npcm4xx_configure(const struct device *dev, uint32_t dev_config)
{
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);

	if (!(dev_config & I2C_MODE_MASTER)) {
		return -ENOTSUP;
	}

	if (dev_config & I2C_ADDR_10_BITS) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		/* 100 Kbit/s */
		data->bitrate = I2C_BITRATE_STANDARD;
		break;

	case I2C_SPEED_FAST:
		/* 400 Kbit/s */
		data->bitrate = I2C_BITRATE_FAST;
		break;

	case I2C_SPEED_FAST_PLUS:
		/* 1 Mbit/s */
		data->bitrate = I2C_BITRATE_FAST_PLUS;
		break;

	default:
		/* Not supported */
		return -ERANGE;
	}

	i2c_npcm4xx_set_baudrate(dev, data->bitrate);

	return 0;
}

static int i2c_npcm4xx_combine_msg(const struct device *dev,
				   struct i2c_msg *msgs, uint8_t num_msgs)
{
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	uint8_t step = 0;
	uint8_t i;

	for (i = 0U; i < num_msgs; i++) {
		struct i2c_msg *msg = msgs + i;

		if ((msg->flags & I2C_MSG_RW_MASK) == I2C_MSG_WRITE) {
			/* support more than one write msg in a transfer */
			if (step == 0) {
				gdma_memcpy_u8(data->tx_buf + data->tx_cnt, msg->buf, msg->len);
				data->tx_cnt += msg->len;
			} else {
				return -1;
			}
		} else {
			/* just support one read msg in a transfer */
			if (step == 1) {
				return -1;
			}
			step = 1;
			data->rx_cnt = msg->len;
			data->rx_msg_buf = msg->buf;
		}
	}
	return 0;
}

static int i2c_npcm4xx_transfer(const struct device *dev, struct i2c_msg *msgs,
				uint8_t num_msgs, uint16_t addr)
{
#if (CONFIG_MASTER_HW_TIMEOUT_EN == 'Y')
	struct i2c_reg *const inst = I2C_INSTANCE(dev);
#endif
	struct i2c_npcm4xx_data *const data = I2C_DRV_DATA(dev);
	int ret;

	if (i2c_npcm4xx_mutex_lock(dev, I2C_WAITING_TIME) != 0) {
		return -EBUSY;
	}

	/* prepare data to transfer */
	data->rx_cnt = 0;
	data->tx_cnt = 0;
	data->dev_addr = addr << 1;
	data->master_oper_state = I2C_NPCM4XX_OPER_STA_START;
	data->err_code = 0;
	if (i2c_npcm4xx_combine_msg(dev, msgs, num_msgs) < 0) {
		i2c_npcm4xx_mutex_unlock(dev);
		return -EPROTONOSUPPORT;
	}

	if (data->rx_cnt == 0 && data->tx_cnt == 0) {
		/* Quick command */
		if (num_msgs != 1) {
			/* Quick command must have one msg */
			i2c_npcm4xx_mutex_unlock(dev);
			return -EPROTONOSUPPORT;
		}
		if ((msgs->flags & I2C_MSG_RW_MASK) == I2C_MSG_WRITE) {
			/* set address to write address */
			data->dev_addr = data->dev_addr & 0xFE;
		} else {
			/* set address to read address */
			data->dev_addr = data->dev_addr | 0x1;
		}
	}

#if (CONFIG_MASTER_HW_TIMEOUT_EN == 'Y')
	/* Set I2C HW timeout value */
	Set_Cumulative_ClockCycle_Timeout(dev, CONFIG_MASTER_HW_TIMEOUT_CLK_CYCLE_TIME);
	Set_Cumulative_ClockLow_Timeout(dev, CONFIG_MASTER_HW_TIMEOUT_CLK_LOW_TIME);
	/* Enable HW Timeout */
	inst->TIMEOUT_EN |= BIT(NPCM4XX_TIMEOUT_EN_TIMEOUT_EN);
#endif

	k_sem_reset(&data->sync_sem);

	i2c_npcm4xx_start(dev);

	ret = i2c_npcm4xx_wait_completion(dev);

	if (data->rx_cnt != 0) {
		gdma_memcpy_u8(data->rx_msg_buf, data->rx_buf, data->rx_cnt);
	}

	i2c_npcm4xx_mutex_unlock(dev);

	return ret;
}

static const struct i2c_driver_api i2c_npcm4xx_driver_api = {
	.configure = i2c_npcm4xx_configure,
	.transfer = i2c_npcm4xx_transfer,
	.slave_register = i2c_npcm4xx_slave_register,
	.slave_unregister = i2c_npcm4xx_slave_unregister,
};


/* I2C controller init macro functions */
#define I2C_NPCM4XX_CTRL_INIT_FUNC(inst) _CONCAT(i2c_npcm4xx_init_, inst)
#define I2C_NPCM4XX_CTRL_INIT_FUNC_DECL(inst) \
	static int i2c_npcm4xx_init_##inst(const struct device *dev)
#define I2C_NPCM4XX_CTRL_INIT_FUNC_IMPL(inst)			     \
	static int i2c_npcm4xx_init_##inst(const struct device *dev) \
	{							     \
		int ret;					     \
								     \
		ret = i2c_npcm4xx_init(dev);			     \
		IRQ_CONNECT(DT_INST_IRQN(inst),			     \
			    DT_INST_IRQ(inst, priority),	     \
			    i2c_npcm4xx_isr,			     \
			    DEVICE_DT_INST_GET(inst),		     \
			    0);					     \
		irq_enable(DT_INST_IRQN(inst));			     \
								     \
		return ret;					     \
	}


#define I2C_NPCM4XX_CTRL_INIT(inst)						 \
	I2C_NPCM4XX_CTRL_INIT_FUNC_DECL(inst);					 \
										 \
	static const struct i2c_npcm4xx_config i2c_npcm4xx_cfg_##inst = {	 \
		.base = DT_INST_REG_ADDR(inst),					 \
		.clk_cfg = NPCM4XX_DT_CLK_CFG_ITEM(inst),			 \
		.default_bitrate = DT_INST_PROP(inst, clock_frequency),		 \
		.irq = DT_INST_IRQN(inst),					 \
	};									 \
										 \
	static struct i2c_npcm4xx_data i2c_npcm4xx_data_##inst;			 \
										 \
	DEVICE_DT_INST_DEFINE(inst,						 \
			      I2C_NPCM4XX_CTRL_INIT_FUNC(inst),			 \
			      NULL,						 \
			      &i2c_npcm4xx_data_##inst, &i2c_npcm4xx_cfg_##inst, \
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	 \
			      &i2c_npcm4xx_driver_api);				 \
										 \
	I2C_NPCM4XX_CTRL_INIT_FUNC_IMPL(inst)

DT_INST_FOREACH_STATUS_OKAY(I2C_NPCM4XX_CTRL_INIT)
