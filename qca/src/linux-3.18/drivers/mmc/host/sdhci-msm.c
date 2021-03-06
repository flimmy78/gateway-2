/*
 * drivers/mmc/host/sdhci-msm.c - Qualcomm SDHCI Platform driver
 *
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/mmc/mmc.h>
#include <linux/slab.h>

#include "sdhci-pltfm.h"

#define CORE_HC_MODE		0x78
#define HC_MODE_EN		0x1
#define CORE_POWER		0x0
#define CORE_SW_RST		BIT(7)
#define CORE_HC_SELECT_IN_EN   (1 << 18)
#define CORE_HC_SELECT_IN_MASK (7 << 19)

#define MAX_PHASES		16
#define CORE_DLL_LOCK		BIT(7)
#define CORE_DLL_EN		BIT(16)
#define CORE_CDR_EN		BIT(17)
#define CORE_CK_OUT_EN		BIT(18)
#define CORE_CDR_EXT_EN		BIT(19)
#define CORE_DLL_PDN		BIT(29)
#define CORE_DLL_RST		BIT(30)
#define CORE_DLL_CONFIG		0x100
#define CORE_DLL_STATUS		0x108
#define CORE_DLL_CONFIG2	0x1b4
#define CORE_DLL_CLK_DISABLE	BIT(21)

#define CORE_VENDOR_SPEC	0x10c
#define CORE_CLK_PWRSAVE	BIT(1)

#define VENDOR_CAPS0            0x11c
#define VOLTS_SUPP_1_8V         BIT(26)
#define VOLTS_SUPP_3_0V         BIT(25)

#define VENDOR_CAPS1            0x120
#define CAPS_SDR_104_SUPPORT    BIT(1)

#define CDR_SELEXT_SHIFT	20
#define CDR_SELEXT_MASK		(0xf << CDR_SELEXT_SHIFT)
#define CMUX_SHIFT_PHASE_SHIFT	24
#define CMUX_SHIFT_PHASE_MASK	(7 << CMUX_SHIFT_PHASE_SHIFT)
#define SDHCI_RETUNING_MODE		BIT(15)
#define SDHCI_ASYNC_INT_SUPPORT		BIT(29)
#define SDHCI_SYS_BUS_SUPPORT_64_BIT	BIT(28)
#define SDHCI_HS_SUPPORT		BIT(21)
#define SDHCI_ADMA2_SUPPORT		BIT(19)
#define SDHCI_SUPPORT_8_BIT		BIT(18)
#define SDHCI_MAX_BLK_LENGTH		BIT(16)
#define SDHCI_BASE_SDCLK_FREQ		0xc800
#define SDHCI_TIMEOUT_CLK_FREQ		0xb2

struct sdhci_msm_host {
	struct platform_device *pdev;
	void __iomem *core_mem;	/* MSM SDCC mapped address */
	struct clk *clk;	/* main SD/MMC bus clock */
	struct clk *pclk;	/* SDHC peripheral bus clock */
	struct clk *bus_clk;	/* SDHC bus voter clock */
	struct mmc_host *mmc;
	struct sdhci_pltfm_data sdhci_msm_pdata;
	bool emulation;
};

/* Platform specific tuning */
static inline int msm_dll_poll_ck_out_en(struct sdhci_host *host, u8 poll)
{
	u32 wait_cnt = 50;
	u8 ck_out_en;
	struct mmc_host *mmc = host->mmc;

	/* Poll for CK_OUT_EN bit.  max. poll time = 50us */
	ck_out_en = !!(readl_relaxed(host->ioaddr + CORE_DLL_CONFIG) &
			CORE_CK_OUT_EN);

	while (ck_out_en != poll) {
		if (--wait_cnt == 0) {
			dev_err(mmc_dev(mmc), "%s: CK_OUT_EN bit is not %d\n",
			       mmc_hostname(mmc), poll);
			return -ETIMEDOUT;
		}
		udelay(1);

		ck_out_en = !!(readl_relaxed(host->ioaddr + CORE_DLL_CONFIG) &
				CORE_CK_OUT_EN);
	}

	return 0;
}

static int msm_config_cm_dll_phase(struct sdhci_host *host, u8 phase)
{
	int rc;
	static const u8 grey_coded_phase_table[] = {
		0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4,
		0xc, 0xd, 0xf, 0xe, 0xa, 0xb, 0x9, 0x8
	};
	unsigned long flags;
	u32 config;
	struct mmc_host *mmc = host->mmc;

	spin_lock_irqsave(&host->lock, flags);

	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~(CORE_CDR_EN | CORE_CK_OUT_EN);
	config |= (CORE_CDR_EXT_EN | CORE_DLL_EN);
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Wait until CK_OUT_EN bit of DLL_CONFIG register becomes '0' */
	rc = msm_dll_poll_ck_out_en(host, 0);
	if (rc)
		goto err_out;

	/*
	 * Write the selected DLL clock output phase (0 ... 15)
	 * to CDR_SELEXT bit field of DLL_CONFIG register.
	 */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~CDR_SELEXT_MASK;
	config |= grey_coded_phase_table[phase] << CDR_SELEXT_SHIFT;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Set CK_OUT_EN bit of DLL_CONFIG register to 1. */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_CK_OUT_EN), host->ioaddr + CORE_DLL_CONFIG);

	/* Wait until CK_OUT_EN bit of DLL_CONFIG register becomes '1' */
	rc = msm_dll_poll_ck_out_en(host, 1);
	if (rc)
		goto err_out;

	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_CDR_EN;
	config &= ~CORE_CDR_EXT_EN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
	goto out;

err_out:
	dev_err(mmc_dev(mmc), "%s: Failed to set DLL phase: %d\n",
	       mmc_hostname(mmc), phase);
out:
	spin_unlock_irqrestore(&host->lock, flags);
	return rc;
}

/*
 * Find out the greatest range of consecuitive selected
 * DLL clock output phases that can be used as sampling
 * setting for SD3.0 UHS-I card read operation (in SDR104
 * timing mode) or for eMMC4.5 card read operation (in HS200
 * timing mode).
 * Select the 3/4 of the range and configure the DLL with the
 * selected DLL clock output phase.
 */

static int msm_find_most_appropriate_phase(struct sdhci_host *host,
					   u8 *phase_table, u8 total_phases)
{
	int ret;
	u8 ranges[MAX_PHASES][MAX_PHASES] = { {0}, {0} };
	u8 phases_per_row[MAX_PHASES] = { 0 };
	int row_index = 0, col_index = 0, selected_row_index = 0, curr_max = 0;
	int i, cnt, phase_0_raw_index = 0, phase_15_raw_index = 0;
	bool phase_0_found = false, phase_15_found = false;
	struct mmc_host *mmc = host->mmc;

	if (!total_phases || (total_phases > MAX_PHASES)) {
		dev_err(mmc_dev(mmc), "%s: Invalid argument: total_phases=%d\n",
		       mmc_hostname(mmc), total_phases);
		return -EINVAL;
	}

	for (cnt = 0; cnt < total_phases; cnt++) {
		ranges[row_index][col_index] = phase_table[cnt];
		phases_per_row[row_index] += 1;
		col_index++;

		if ((cnt + 1) == total_phases) {
			continue;
		/* check if next phase in phase_table is consecutive or not */
		} else if ((phase_table[cnt] + 1) != phase_table[cnt + 1]) {
			row_index++;
			col_index = 0;
		}
	}

	if (row_index >= MAX_PHASES)
		return -EINVAL;

	/* Check if phase-0 is present in first valid window? */
	if (!ranges[0][0]) {
		phase_0_found = true;
		phase_0_raw_index = 0;
		/* Check if cycle exist between 2 valid windows */
		for (cnt = 1; cnt <= row_index; cnt++) {
			if (phases_per_row[cnt]) {
				for (i = 0; i < phases_per_row[cnt]; i++) {
					if (ranges[cnt][i] == 15) {
						phase_15_found = true;
						phase_15_raw_index = cnt;
						break;
					}
				}
			}
		}
	}

	/* If 2 valid windows form cycle then merge them as single window */
	if (phase_0_found && phase_15_found) {
		/* number of phases in raw where phase 0 is present */
		u8 phases_0 = phases_per_row[phase_0_raw_index];
		/* number of phases in raw where phase 15 is present */
		u8 phases_15 = phases_per_row[phase_15_raw_index];

		if (phases_0 + phases_15 >= MAX_PHASES)
			/*
			 * If there are more than 1 phase windows then total
			 * number of phases in both the windows should not be
			 * more than or equal to MAX_PHASES.
			 */
			return -EINVAL;

		/* Merge 2 cyclic windows */
		i = phases_15;
		for (cnt = 0; cnt < phases_0; cnt++) {
			ranges[phase_15_raw_index][i] =
			    ranges[phase_0_raw_index][cnt];
			if (++i >= MAX_PHASES)
				break;
		}

		phases_per_row[phase_0_raw_index] = 0;
		phases_per_row[phase_15_raw_index] = phases_15 + phases_0;
	}

	for (cnt = 0; cnt <= row_index; cnt++) {
		if (phases_per_row[cnt] > curr_max) {
			curr_max = phases_per_row[cnt];
			selected_row_index = cnt;
		}
	}

	i = (curr_max * 3) / 4;
	if (i)
		i--;

	ret = ranges[selected_row_index][i];

	if (ret >= MAX_PHASES) {
		ret = -EINVAL;
		dev_err(mmc_dev(mmc), "%s: Invalid phase selected=%d\n",
		       mmc_hostname(mmc), ret);
	}

	return ret;
}

static inline void msm_cm_dll_set_freq(struct sdhci_host *host)
{
	u32 mclk_freq = 0, config;

	/* Program the MCLK value to MCLK_FREQ bit field */
	if (host->clock <= 112000000)
		mclk_freq = 0;
	else if (host->clock <= 125000000)
		mclk_freq = 1;
	else if (host->clock <= 137000000)
		mclk_freq = 2;
	else if (host->clock <= 150000000)
		mclk_freq = 3;
	else if (host->clock <= 162000000)
		mclk_freq = 4;
	else if (host->clock <= 175000000)
		mclk_freq = 5;
	else if (host->clock <= 187000000)
		mclk_freq = 6;
	else if (host->clock <= 200000000)
		mclk_freq = 7;

	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~CMUX_SHIFT_PHASE_MASK;
	config |= mclk_freq << CMUX_SHIFT_PHASE_SHIFT;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
}

/* Initialize the DLL (Programmable Delay Line) */
static int msm_init_cm_dll(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	int wait_cnt = 50;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * Make sure that clock is always enabled when DLL
	 * tuning is in progress. Keeping PWRSAVE ON may
	 * turn off the clock.
	 */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC)
			& ~CORE_CLK_PWRSAVE), host->ioaddr + CORE_VENDOR_SPEC);

	/* Write 1 to DLL_RST bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_DLL_RST), host->ioaddr + CORE_DLL_CONFIG);

	/* Write 1 to DLL_PDN bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_DLL_PDN), host->ioaddr + CORE_DLL_CONFIG);
	msm_cm_dll_set_freq(host);

	/* Write 0 to DLL_RST bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			& ~CORE_DLL_RST), host->ioaddr + CORE_DLL_CONFIG);

	/* Write 0 to DLL_PDN bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			& ~CORE_DLL_PDN), host->ioaddr + CORE_DLL_CONFIG);

	/* Set DLL_EN bit to 1. */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_DLL_EN), host->ioaddr + CORE_DLL_CONFIG);

	/* Set CK_OUT_EN bit to 1. */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_CK_OUT_EN), host->ioaddr + CORE_DLL_CONFIG);

	/* Write 0 to DLL_CLOCK_DISABLE bit of DLL_CONFIG_2 register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG2)
		& ~CORE_DLL_CLK_DISABLE), host->ioaddr + CORE_DLL_CONFIG2);

	/* Wait until DLL_LOCK bit of DLL_STATUS register becomes '1' */
	while (!(readl_relaxed(host->ioaddr + CORE_DLL_STATUS) &
		 CORE_DLL_LOCK)) {
		/* max. wait for 50us sec for LOCK bit to be set */
		if (--wait_cnt == 0) {
			dev_err(mmc_dev(mmc), "%s: DLL failed to LOCK\n",
			       mmc_hostname(mmc));
			spin_unlock_irqrestore(&host->lock, flags);
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	spin_unlock_irqrestore(&host->lock, flags);
	return 0;
}

static int sdhci_msm_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	int tuning_seq_cnt = 3;
	u8 phase, *data_buf, tuned_phases[16], tuned_phase_cnt = 0;
	const u8 *tuning_block_pattern = tuning_blk_pattern_4bit;
	int size = sizeof(tuning_blk_pattern_4bit);
	int rc;
	struct mmc_host *mmc = host->mmc;
	struct mmc_ios ios = host->mmc->ios;

	/*
	 * Tuning is required for SDR104, HS200 and HS400 cards and
	 * if clock frequency is greater than 100MHz in these modes.
	 */
	if (host->clock <= 100 * 1000 * 1000 ||
	    !((ios.timing == MMC_TIMING_MMC_HS200) ||
	      (ios.timing == MMC_TIMING_UHS_SDR104)))
		return 0;

	if ((opcode == MMC_SEND_TUNING_BLOCK_HS200) &&
	    (mmc->ios.bus_width == MMC_BUS_WIDTH_8)) {
		tuning_block_pattern = tuning_blk_pattern_8bit;
		size = sizeof(tuning_blk_pattern_8bit);
	}

	data_buf = kmalloc(size, GFP_KERNEL);
	if (!data_buf)
		return -ENOMEM;

retry:
	/* First of all reset the tuning block */
	rc = msm_init_cm_dll(host);
	if (rc)
		goto out;

	phase = 0;
	do {
		struct mmc_command cmd = { 0 };
		struct mmc_data data = { 0 };
		struct mmc_request mrq = {
			.cmd = &cmd,
			.data = &data
		};
		struct scatterlist sg;

		/* Set the phase in delay line hw block */
		rc = msm_config_cm_dll_phase(host, phase);
		if (rc)
			goto out;

		cmd.opcode = opcode;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		data.blksz = size;
		data.blocks = 1;
		data.flags = MMC_DATA_READ;
		data.timeout_ns = NSEC_PER_SEC;	/* 1 second */

		data.sg = &sg;
		data.sg_len = 1;
		sg_init_one(&sg, data_buf, size);
		memset(data_buf, 0, size);
		mmc_wait_for_req(mmc, &mrq);

		if (!cmd.error && !data.error &&
		    !memcmp(data_buf, tuning_block_pattern, size)) {
			/* Tuning is successful at this tuning point */
			tuned_phases[tuned_phase_cnt++] = phase;
			dev_dbg(mmc_dev(mmc), "%s: Found good phase = %d\n",
				 mmc_hostname(mmc), phase);
		}
	} while (++phase < ARRAY_SIZE(tuned_phases));

	if (tuned_phase_cnt) {
		rc = msm_find_most_appropriate_phase(host, tuned_phases,
						     tuned_phase_cnt);
		if (rc < 0)
			goto out;
		else
			phase = rc;

		/*
		 * Finally set the selected phase in delay
		 * line hw block.
		 */
		rc = msm_config_cm_dll_phase(host, phase);
		if (rc)
			goto out;
		dev_dbg(mmc_dev(mmc), "%s: Setting the tuning phase to %d\n",
			 mmc_hostname(mmc), phase);
	} else {
		if (--tuning_seq_cnt)
			goto retry;
		/* Tuning failed */
		dev_dbg(mmc_dev(mmc), "%s: No tuning point found\n",
		       mmc_hostname(mmc));
		rc = -EIO;
	}

out:
	kfree(data_buf);
	return rc;
}

static void sdhci_msm_toggle_cdr(struct sdhci_host *host, bool enable)
{
	u32 config;
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);

	if (enable) {
		config |= CORE_CDR_EN;
		config &= ~CORE_CDR_EXT_EN;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
	} else {
		config &= ~CORE_CDR_EN;
		config |= CORE_CDR_EXT_EN;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
	}
}

static const struct of_device_id sdhci_msm_dt_match[] = {
	{ .compatible = "qcom,sdhci-msm-v4" },
	{},
};

MODULE_DEVICE_TABLE(of, sdhci_msm_dt_match);

static struct sdhci_ops sdhci_msm_ops = {
	.platform_execute_tuning = sdhci_msm_execute_tuning,
	.reset = sdhci_reset,
	.set_clock = sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
	.toggle_cdr = sdhci_msm_toggle_cdr,
};

static int sdhci_msm_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_msm_host *msm_host;
	struct resource *core_memres;
	int ret;
	u16 host_version;
	struct device_node *np = pdev->dev.of_node;

	msm_host = devm_kzalloc(&pdev->dev, sizeof(*msm_host), GFP_KERNEL);
	if (!msm_host)
		return -ENOMEM;

	msm_host->sdhci_msm_pdata.ops = &sdhci_msm_ops;
	host = sdhci_pltfm_init(pdev, &msm_host->sdhci_msm_pdata, 0);
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	pltfm_host->priv = msm_host;
	msm_host->mmc = host->mmc;
	msm_host->pdev = pdev;

	msm_host->emulation = of_property_read_bool(np, "qca,emulation");

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto pltfm_free;

	sdhci_get_of_property(pdev);

	/* Setup SDCC bus voter clock. */
	msm_host->bus_clk = devm_clk_get(&pdev->dev, "bus");
	if (!IS_ERR(msm_host->bus_clk)) {
		/* Vote for max. clk rate for max. performance */
		ret = clk_set_rate(msm_host->bus_clk, INT_MAX);
		if (ret)
			goto pltfm_free;
		ret = clk_prepare_enable(msm_host->bus_clk);
		if (ret)
			goto pltfm_free;
	}

	/* Setup main peripheral bus clock */
	msm_host->pclk = devm_clk_get(&pdev->dev, "iface");
	if (IS_ERR(msm_host->pclk)) {
		ret = PTR_ERR(msm_host->pclk);
		dev_err(&pdev->dev, "Perpheral clk setup failed (%d)\n", ret);
		goto bus_clk_disable;
	}

	ret = clk_prepare_enable(msm_host->pclk);
	if (ret)
		goto bus_clk_disable;

	/* Setup SDC MMC clock */
	msm_host->clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(msm_host->clk)) {
		ret = PTR_ERR(msm_host->clk);
		dev_err(&pdev->dev, "SDC MMC clk setup failed (%d)\n", ret);
		goto pclk_disable;
	}

	ret = clk_prepare_enable(msm_host->clk);
	if (ret)
		goto pclk_disable;

	core_memres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	msm_host->core_mem = devm_ioremap_resource(&pdev->dev, core_memres);

	if (IS_ERR(msm_host->core_mem)) {
		dev_err(&pdev->dev, "Failed to remap registers\n");
		ret = PTR_ERR(msm_host->core_mem);
		goto clk_disable;
	}


	if (msm_host->emulation) {
		/*Set 1.8V,3V support*/
		writel_relaxed((readl_relaxed(host->ioaddr + VENDOR_CAPS0)
					| VOLTS_SUPP_1_8V
					| VOLTS_SUPP_3_0V),
				host->ioaddr + VENDOR_CAPS0);

		/*Set timeout caps*/
		writel_relaxed((readl_relaxed(host->ioaddr + VENDOR_CAPS0)
					| 0x32),
				host->ioaddr + VENDOR_CAPS0);

		/*Set base clock 10 MHz*/
		writel_relaxed((readl_relaxed(host->ioaddr + VENDOR_CAPS0)
					| ((0xA) << 8)),
				host->ioaddr + VENDOR_CAPS0);

		/* Remove SDR104 support*/
		writel_relaxed((readl_relaxed(host->ioaddr + VENDOR_CAPS1)
					& ~(CAPS_SDR_104_SUPPORT)),
				host->ioaddr + VENDOR_CAPS1);
	}

	/* Set missing caps quirks */
	host->quirks  |= SDHCI_QUIRK_MISSING_CAPS;

	/* Enable SDCC supported capabilities */
	host->caps = SDHCI_CAN_VDD_300 | SDHCI_CAN_VDD_180 |
			SDHCI_ASYNC_INT_SUPPORT |
			SDHCI_SYS_BUS_SUPPORT_64_BIT | SDHCI_HS_SUPPORT |
			SDHCI_ADMA2_SUPPORT | SDHCI_SUPPORT_8_BIT |
			SDHCI_MAX_BLK_LENGTH | SDHCI_TIMEOUT_CLK_UNIT |
			SDHCI_BASE_SDCLK_FREQ | SDHCI_TIMEOUT_CLK_FREQ;

	/* Enable SD card supported modes */
	host->caps1 = SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
		SDHCI_SUPPORT_DDR50 | SDHCI_RETUNING_MODE;

	/* Reset the core and Enable SDHC mode */
	writel_relaxed(readl_relaxed(msm_host->core_mem + CORE_POWER) |
		       CORE_SW_RST, msm_host->core_mem + CORE_POWER);

	/* SW reset can take upto 10HCLK + 15MCLK cycles. (min 40us) */
	usleep_range(1000, 5000);
	if (readl(msm_host->core_mem + CORE_POWER) & CORE_SW_RST) {
		dev_err(&pdev->dev, "Stuck in reset\n");
		ret = -ETIMEDOUT;
		goto clk_disable;
	}

	/* Set HC_MODE_EN bit in HC_MODE register */
	writel_relaxed(HC_MODE_EN, (msm_host->core_mem + CORE_HC_MODE));

	host->quirks |= SDHCI_QUIRK_SINGLE_POWER_WRITE;
	host->quirks2 |= SDHCI_QUIRK2_USE_MAX_DISCARD_SIZE;

	host_version = readw_relaxed((host->ioaddr + SDHCI_HOST_VERSION));
	dev_dbg(&pdev->dev, "Host Version: 0x%x Vendor Version 0x%x\n",
		host_version, ((host_version & SDHCI_VENDOR_VER_MASK) >>
			       SDHCI_VENDOR_VER_SHIFT));

	ret = sdhci_add_host(host);

	if (msm_host->emulation) {
		/* Enable controller */
		writel_relaxed(readl_relaxed(msm_host->core_mem + CORE_POWER) |
			0x1, msm_host->core_mem + CORE_POWER);
	}

	if (ret)
		goto clk_disable;

	return 0;

clk_disable:
	clk_disable_unprepare(msm_host->clk);
pclk_disable:
	clk_disable_unprepare(msm_host->pclk);
bus_clk_disable:
	if (!IS_ERR(msm_host->bus_clk))
		clk_disable_unprepare(msm_host->bus_clk);
pltfm_free:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int sdhci_msm_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = pltfm_host->priv;
	int dead = (readl_relaxed(host->ioaddr + SDHCI_INT_STATUS) ==
		    0xffffffff);

	sdhci_remove_host(host, dead);
	sdhci_pltfm_free(pdev);
	clk_disable_unprepare(msm_host->clk);
	clk_disable_unprepare(msm_host->pclk);
	if (!IS_ERR(msm_host->bus_clk))
		clk_disable_unprepare(msm_host->bus_clk);
	return 0;
}

static struct platform_driver sdhci_msm_driver = {
	.probe = sdhci_msm_probe,
	.remove = sdhci_msm_remove,
	.driver = {
		   .name = "sdhci_msm",
		   .of_match_table = sdhci_msm_dt_match,
	},
};

module_platform_driver(sdhci_msm_driver);

MODULE_DESCRIPTION("Qualcomm Secure Digital Host Controller Interface driver");
MODULE_LICENSE("GPL v2");
