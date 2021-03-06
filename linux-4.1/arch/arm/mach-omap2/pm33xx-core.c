/*
 * AM33XX Arch Power Management Routines
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com/
 *	Dave Gerlach
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/platform_data/gpio-omap.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/wkup_m3_ipc.h>
#include <linux/of.h>
#include <linux/rtc.h>

#include <asm/smp_scu.h>
#include <asm/suspend.h>

#include "control.h"
#include "cm33xx.h"
#include "pm.h"
#include "prm33xx.h"
#include "common.h"
#include "clockdomain.h"
#include "powerdomain.h"
#include "soc.h"
#include "sram.h"
#include "omap_hwmod.h"
#include "iomap.h"

static struct powerdomain *cefuse_pwrdm, *gfx_pwrdm, *per_pwrdm, *mpu_pwrdm;
static struct clockdomain *gfx_l4ls_clkdm;
static void __iomem *scu_base;
static struct omap_hwmod *rtc_oh;

static struct pinctrl_dev *pmx_dev;

static int __init am43xx_map_scu(void)
{
	scu_base = ioremap(scu_a9_get_base(), SZ_256);

	if (!scu_base)
		return -ENOMEM;

	return 0;
}

static int am33xx_check_off_mode_enable(void)
{
	/* off mode not supported on am335x so return 0 always */
	return 0;
}

static int am43xx_check_off_mode_enable(void)
{
	return enable_off_mode;
}

static int amx3_common_init(void)
{
	gfx_pwrdm = pwrdm_lookup("gfx_pwrdm");
	per_pwrdm = pwrdm_lookup("per_pwrdm");
	mpu_pwrdm = pwrdm_lookup("mpu_pwrdm");

	if ((!gfx_pwrdm) || (!per_pwrdm) || (!mpu_pwrdm))
		return -ENODEV;

	(void)clkdm_for_each(omap_pm_clkdms_setup, NULL);

	/* CEFUSE domain can be turned off post bootup */
	cefuse_pwrdm = pwrdm_lookup("cefuse_pwrdm");
	if (cefuse_pwrdm)
		omap_set_pwrdm_state(cefuse_pwrdm, PWRDM_POWER_OFF);
	else
		pr_err("PM: Failed to get cefuse_pwrdm\n");

	return 0;
}

static int am33xx_suspend_init(void (*do_sram_cpuidle)(u32 wfi_flags))
{
	int ret;

	gfx_l4ls_clkdm = clkdm_lookup("gfx_l4ls_gfx_clkdm");

	if (!gfx_l4ls_clkdm) {
		pr_err("PM: Cannot lookup gfx_l4ls_clkdm clockdomains\n");
		return -ENODEV;
	}

	am33xx_idle_init(true, do_sram_cpuidle);

	ret = amx3_common_init();

	return ret;
}

static int am43xx_suspend_init(void (*do_sram_cpuidle)(u32 wfi_flags))
{
	int ret = 0;

	pmx_dev = get_pinctrl_dev_from_devname("44e10800.pinmux");

	ret = am43xx_map_scu();
	if (ret) {
		pr_err("PM: Could not ioremap SCU\n");
		return ret;
	}

	am437x_idle_init();

	if (ret) {
		pr_err("PM: Could not ioremap GIC\n");
		return ret;
	}

	ret = amx3_common_init();

	return ret;
}

static int am33xx_pm_deinit(void)
{
	am33xx_idle_deinit();
	return 0;
}

static void amx3_pre_suspend_common(void)
{
	omap_set_pwrdm_state(gfx_pwrdm, PWRDM_POWER_OFF);
}

static void amx3_post_suspend_common(void)
{
	int status;
	/*
	 * Because gfx_pwrdm is the only one under MPU control,
	 * comment on transition status
	 */

	status = pwrdm_read_pwrst(gfx_pwrdm);
	if (status != PWRDM_POWER_OFF)
		pr_err("PM: GFX domain did not transition: %x\n", status);
}

static int am33xx_suspend(unsigned int state, int (*fn)(unsigned long),
			  unsigned long args)
{
	int ret = 0;

	amx3_pre_suspend_common();
	ret = cpu_suspend(args, fn);
	amx3_post_suspend_common();

	/*
	 * BUG: GFX_L4LS clock domain needs to be woken up to
	 * ensure thet L4LS clock domain does not get stuck in
	 * transition. If that happens L3 module does not get
	 * disabled, thereby leading to PER power domain
	 * transition failing
	 */

	clkdm_wakeup(gfx_l4ls_clkdm);
	clkdm_sleep(gfx_l4ls_clkdm);

	return ret;
}

static int am43xx_suspend(unsigned int state, int (*fn)(unsigned long),
			  unsigned long args)
{
	int ret = 0;

	amx3_pre_suspend_common();
	scu_power_mode(scu_base, SCU_PM_POWEROFF);
	ret = cpu_suspend(args, fn);
	scu_power_mode(scu_base, SCU_PM_NORMAL);

	if (!am43xx_check_off_mode_enable())
		amx3_post_suspend_common();

	return ret;
}

static int am33xx_cpu_suspend(int (*fn)(unsigned long), unsigned long args)
{
	int ret = 0;

	ret = cpu_suspend(args, fn);

	return ret;
}

static void common_save_context(void)
{
	omap2_gpio_prepare_for_idle(1);
	pinmux_save_context(pmx_dev, "am33xx_pmx_per");
	clks_save_context();
	pwrdms_save_context();
	omap_hwmods_save_context();
	clkdm_save_context();
}

static void common_restore_context(void)
{
	clks_restore_context();
	clkdm_restore_context();
	pwrdms_restore_context();
	omap_hwmods_restore_context();
	pinmux_restore_context(pmx_dev, "am33xx_pmx_per");
	pwrdms_lost_power();
	omap2_gpio_resume_after_idle();
}

static void am33xx_save_context(void)
{
	common_save_context();
	omap_intc_save_context();
	am33xx_control_save_context();
}

static void am33xx_restore_context(void)
{
	common_restore_context();
	am33xx_control_restore_context();
	omap_intc_restore_context();
}

static void am43xx_save_context(void)
{
	common_save_context();
	am43xx_control_save_context();
}

static void am43xx_restore_context(void)
{
	common_restore_context();
	am43xx_control_restore_context();

	writel_relaxed(0x0, AM33XX_L4_WK_IO_ADDRESS(0x44df2e14));
}

static void am43xx_prepare_rtc_suspend(void)
{
	omap_hwmod_enable(rtc_oh);
}

static void am43xx_prepare_rtc_resume(void)
{
	omap_hwmod_idle(rtc_oh);
}

void __iomem *am43xx_get_rtc_base_addr(void)
{
	rtc_oh = omap_hwmod_lookup("rtc");

	return omap_hwmod_get_mpu_rt_va(rtc_oh);
}

static struct am33xx_pm_ops am33xx_ops = {
	.init = am33xx_suspend_init,
	.deinit = am33xx_pm_deinit,
	.soc_suspend = am33xx_suspend,
	.cpu_suspend = am33xx_cpu_suspend,
	.save_context = am33xx_save_context,
	.restore_context = am33xx_restore_context,
	.prepare_rtc_suspend = am43xx_prepare_rtc_suspend,
	.prepare_rtc_resume = am43xx_prepare_rtc_resume,
	.check_off_mode_enable = am33xx_check_off_mode_enable,
	.get_rtc_base_addr = am43xx_get_rtc_base_addr,
};

static struct am33xx_pm_ops am43xx_ops = {
	.init = am43xx_suspend_init,
	.soc_suspend = am43xx_suspend,
	.save_context = am43xx_save_context,
	.restore_context = am43xx_restore_context,
	.prepare_rtc_suspend = am43xx_prepare_rtc_suspend,
	.prepare_rtc_resume = am43xx_prepare_rtc_resume,
	.check_off_mode_enable = am43xx_check_off_mode_enable,
	.get_rtc_base_addr = am43xx_get_rtc_base_addr,
};

struct am33xx_pm_ops *amx3_get_pm_ops(void)
{
	if (soc_is_am33xx())
		return &am33xx_ops;
	else if (soc_is_am43xx())
		return &am43xx_ops;
	else
		return NULL;
}
EXPORT_SYMBOL_GPL(amx3_get_pm_ops);

struct am33xx_pm_sram_addr *amx3_get_sram_addrs(void)
{
	if (soc_is_am33xx())
		return &am33xx_pm_sram;
	else if (soc_is_am43xx())
		return &am43xx_pm_sram;
	else
		return NULL;
}
EXPORT_SYMBOL_GPL(amx3_get_sram_addrs);
