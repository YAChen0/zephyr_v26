/*
 * Copyright (c) 2021 Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <kernel.h>
#include <device.h>
#include <init.h>
#include <soc.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(soc, CONFIG_SOC_LOG_LEVEL);

void z_platform_init(void)
{
}

static int soc_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	npcm4xx_sram_vector_table_copy();

	return 0;
}

SYS_INIT(soc_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
