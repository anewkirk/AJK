/*
 * Freescale eSDHC controller driver.
 *
 * Copyright (c) 2007 Freescale Semiconductor, Inc.
 * Copyright (c) 2009 MontaVista Software, Inc.
 *
 * Authors: Xiaobo Xie <X.Xie@freescale.com>
 *	    Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mmc/host.h>
#include "sdhci-of.h"
#include "sdhci.h"
#include "sdhci-esdhc.h"

#define VENDOR_V_22	0x12
#define VENDOR_V_23	0x13
static u32 esdhc_readl(struct sdhci_host *host, int reg)
{
	u32 ret;

	ret = in_be32(host->ioaddr + reg);
	/*
	 * The bit of ADMA flag in eSDHC is not compatible with standard
	 * SDHC register, so set fake flag SDHCI_CAN_DO_ADMA2 when ADMA is
	 * supported by eSDHC.
	 * And for many FSL eSDHC controller, the reset value of field
	 * SDHCI_CAN_DO_ADMA1 is one, but some of them can't support ADMA,
	 * only these vendor version is greater than 2.2/0x12 support ADMA.
	 * For FSL eSDHC, must aligned 4-byte, so use 0xFC to read the
	 * the verdor version number, oxFE is SDHCI_HOST_VERSION.
	 */
	if ((reg == SDHCI_CAPABILITIES) && (ret & SDHCI_CAN_DO_ADMA1)) {
		u32 tmp = in_be32(host->ioaddr + SDHCI_SLOT_INT_STATUS);
		tmp = (tmp & SDHCI_VENDOR_VER_MASK) >> SDHCI_VENDOR_VER_SHIFT;
		if (tmp > VENDOR_V_22)
			ret |= SDHCI_CAN_DO_ADMA2;
	}

	return ret;
}

static u16 esdhc_readw(struct sdhci_host *host, int reg)
{
	u16 ret;

	if (unlikely(reg == SDHCI_HOST_VERSION))
		ret = in_be16(host->ioaddr + reg);
	else
		ret = sdhci_be32bs_readw(host, reg);
	return ret;
}

static void esdhc_writel(struct sdhci_host *host, u32 val, int reg)
{
	/*
	 * Enable IRQSTATEN[BGESEN] is just to set IRQSTAT[BGE]
	 * when SYSCTL[RSTD]) is set for some special operations.
	 * No any impact other operation.
	 */
	if (reg == SDHCI_INT_ENABLE)
		val |= SDHCI_INT_BLK_GAP;
	sdhci_be32bs_writel(host, val, reg);
}

static void esdhc_writew(struct sdhci_host *host, u16 val, int reg)
{
	if (reg == SDHCI_BLOCK_SIZE) {
		/*
		 * Two last DMA bits are reserved, and first one is used for
		 * non-standard blksz of 4096 bytes that we don't support
		 * yet. So clear the DMA boundary bits.
		 */
		val &= ~SDHCI_MAKE_BLKSZ(0x7, 0);
	}
	sdhci_be32bs_writew(host, val, reg);
}

static void esdhc_writeb(struct sdhci_host *host, u8 val, int reg)
{
	/* Prevent SDHCI core from writing reserved bits (e.g. HISPD). */
	if (reg == SDHCI_HOST_CONTROL)
		val &= ~ESDHC_HOST_CONTROL_RES;
	sdhci_be32bs_writeb(host, val, reg);
}

/*
 * For Abort or Suspend after Stop at Block Gap, ignore the ADMA
 * error(IRQSTAT[ADMAE]) if both Transfer Complete(IRQSTAT[TC])
 * and Block Gap Event(IRQSTAT[BGE]) are also set.
 * For Continue, apply soft reset for data(SYSCTL[RSTD]);
 * and re-issue the entire read transaction from beginning.
 */
static void esdhci_of_adma_workaround(struct sdhci_host *host, u32 intmask)
{
	u32 tmp;
	bool applicable;
	dma_addr_t dmastart;
	dma_addr_t dmanow;

	tmp = in_be32(host->ioaddr + SDHCI_SLOT_INT_STATUS);
	tmp = (tmp & SDHCI_VENDOR_VER_MASK) >> SDHCI_VENDOR_VER_SHIFT;

	applicable = (intmask & SDHCI_INT_DATA_END) &&
		(intmask & SDHCI_INT_BLK_GAP) &&
		(tmp == VENDOR_V_23);
	if (!applicable)
		return;

	host->data->error = 0;
	dmastart = sg_dma_address(host->data->sg);
	dmanow = dmastart + host->data->bytes_xfered;
	/*
	 * Force update to the next DMA block boundary.
	 */
	dmanow = (dmanow & ~(SDHCI_DEFAULT_BOUNDARY_SIZE - 1)) +
		SDHCI_DEFAULT_BOUNDARY_SIZE;
	host->data->bytes_xfered = dmanow - dmastart;
	sdhci_writel(host, dmanow, SDHCI_DMA_ADDRESS);
}

static int esdhc_of_enable_dma(struct sdhci_host *host)
{
	setbits32(host->ioaddr + ESDHC_DMA_SYSCTL, ESDHC_DMA_SNOOP);
	return 0;
}

static unsigned int esdhc_of_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_of_host *of_host = sdhci_priv(host);

	return of_host->clock;
}

static unsigned int esdhc_of_get_min_clock(struct sdhci_host *host)
{
	struct sdhci_of_host *of_host = sdhci_priv(host);

	return of_host->clock / 256 / 16;
}

static void esdhc_of_resume(struct sdhci_host *host)
{
	esdhc_of_enable_dma(host);
	sdhci_be32bs_writel(host, esdhc_proctl, SDHCI_HOST_CONTROL);
}
#endif

static void esdhc_of_platform_init(struct sdhci_host *host)
{
	u32 vvn;

	vvn = in_be32(host->ioaddr + SDHCI_SLOT_INT_STATUS);
	vvn = (vvn & SDHCI_VENDOR_VER_MASK) >> SDHCI_VENDOR_VER_SHIFT;
	if (vvn == VENDOR_V_22)
		host->quirks2 |= SDHCI_QUIRK2_HOST_NO_CMD23;
}

static struct sdhci_ops sdhci_esdhc_ops = {
	.read_l = esdhc_readl,
	.read_w = esdhc_readw,
	.read_b = esdhc_readb,
	.write_l = esdhc_writel,
	.write_w = esdhc_writew,
	.write_b = esdhc_writeb,
	.set_clock = esdhc_of_set_clock,
	.enable_dma = esdhc_of_enable_dma,
	.get_max_clock = esdhc_of_get_max_clock,
	.get_min_clock = esdhc_of_get_min_clock,
	.platform_init = esdhc_of_platform_init,
#ifdef CONFIG_PM
	.platform_suspend = esdhc_of_suspend,
	.platform_resume = esdhc_of_resume,
#endif
	.adma_workaround = esdhci_of_adma_workaround,
};

static struct sdhci_pltfm_data sdhci_esdhc_pdata = {
	/*
	 * card detection could be handled via GPIO
	 * eSDHC cannot support End Attribute in NOP ADMA descriptor
	 */
	.quirks = ESDHC_DEFAULT_QUIRKS | SDHCI_QUIRK_BROKEN_CARD_DETECTION
		| SDHCI_QUIRK_NO_CARD_NO_RESET,
	.ops = {
		.read_l = sdhci_be32bs_readl,
		.read_w = esdhc_readw,
		.read_b = sdhci_be32bs_readb,
		.write_l = sdhci_be32bs_writel,
		.write_w = esdhc_writew,
		.write_b = esdhc_writeb,
		.set_clock = esdhc_set_clock,
		.enable_dma = esdhc_of_enable_dma,
		.get_max_clock = esdhc_of_get_max_clock,
		.get_min_clock = esdhc_of_get_min_clock,
	},
};
