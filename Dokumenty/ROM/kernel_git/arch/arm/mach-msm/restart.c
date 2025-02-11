/* Copyright (c) 2010-2013, The Linux Foundation. All rights reserved.
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
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/mfd/pmic8058.h>
#include <linux/mfd/pmic8901.h>
#include <linux/mfd/pm8xxx/misc.h>
#include <linux/qpnp/power-on.h>

#include <asm/mach-types.h>
#include <asm/cacheflush.h>

#include <mach/msm_iomap.h>
#include <mach/restart.h>
#include <mach/socinfo.h>
#include <mach/irqs.h>
#include <mach/scm.h>
#include "msm_watchdog.h"
#include "timer.h"
#include "wdog_debug.h"

#ifdef CONFIG_KEXEC_HARDBOOT
#include <asm/kexec.h>
#endif

#ifdef CONFIG_LGE_HANDLE_PANIC
#include <mach/lge_handle_panic.h>
#endif

#define WDT0_RST	0x38
#define WDT0_EN		0x40
#define WDT0_BARK_TIME	0x4C
#define WDT0_BITE_TIME	0x5C

#define PSHOLD_CTL_SU (MSM_TLMM_BASE + 0x820)

#define RESTART_REASON_ADDR 0x65C
#define DLOAD_MODE_ADDR     0x0
#define EMERGENCY_DLOAD_MODE_ADDR    0xFE0
#define EMERGENCY_DLOAD_MAGIC1    0x322A4F99
#define EMERGENCY_DLOAD_MAGIC2    0xC67E4350
#define EMERGENCY_DLOAD_MAGIC3    0x77777777

#define SCM_IO_DISABLE_PMIC_ARBITER	1

#ifdef CONFIG_MSM_RESTART_V2
#define use_restart_v2()	1
#else
#define use_restart_v2()	0
#endif

static int restart_mode;
void *restart_reason;

int pmic_reset_irq;
static void __iomem *msm_tmr0_base;

#ifdef CONFIG_MSM_DLOAD_MODE
static int in_panic;
static void *dload_mode_addr;
static bool dload_mode_enabled;
static void *emergency_dload_mode_addr;

/* Download mode master kill-switch */
static int dload_set(const char *val, struct kernel_param *kp);
static int download_mode = 0;
module_param_call(download_mode, dload_set, param_get_int,
			&download_mode, 0644);
static int panic_prep_restart(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call	= panic_prep_restart,
};

static void set_dload_mode(int on)
{
	pr_err("%s(%d)\n", __func__, on);
	if (dload_mode_addr) {
		__raw_writel(on ? 0xE47B337D : 0, dload_mode_addr);
		__raw_writel(on ? 0xCE14091A : 0,
		       dload_mode_addr + sizeof(unsigned int));
		mb();
		dload_mode_enabled = on;
	}
}

static bool get_dload_mode(void)
{
	pr_err("%s(%d)\n", __func__, dload_mode_enabled);
	return dload_mode_enabled;
}

static void enable_emergency_dload_mode(void)
{
	if (emergency_dload_mode_addr) {
		__raw_writel(EMERGENCY_DLOAD_MAGIC1,
				emergency_dload_mode_addr);
		__raw_writel(EMERGENCY_DLOAD_MAGIC2,
				emergency_dload_mode_addr +
				sizeof(unsigned int));
		__raw_writel(EMERGENCY_DLOAD_MAGIC3,
				emergency_dload_mode_addr +
				(2 * sizeof(unsigned int)));

		/* Need disable the pmic wdt, then the emergency dload mode
		 * will not auto reset. */
		qpnp_pon_wd_config(0);
		mb();
	}
}

static int dload_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = download_mode;

	ret = param_set_int(val, kp);

	if (ret)
		return ret;

	/* If download_mode is not zero or one, ignore. */
	if (download_mode >> 1) {
		download_mode = old_val;
		return -EINVAL;
	}

	set_dload_mode(download_mode);

	return 0;
}
#else
#define set_dload_mode(x) do {} while (0)

static void enable_emergency_dload_mode(void)
{
	printk(KERN_ERR "dload mode is not enabled on target\n");
}

static bool get_dload_mode(void)
{
	return false;
}
#endif

void msm_set_restart_mode(int mode)
{
	restart_mode = mode;
}
EXPORT_SYMBOL(msm_set_restart_mode);

static bool scm_pmic_arbiter_disable_supported;
/*
 * Force the SPMI PMIC arbiter to shutdown so that no more SPMI transactions
 * are sent from the MSM to the PMIC.  This is required in order to avoid an
 * SPMI lockup on certain PMIC chips if PS_HOLD is lowered in the middle of
 * an SPMI transaction.
 */
static void halt_spmi_pmic_arbiter(void)
{
	if (scm_pmic_arbiter_disable_supported) {
		pr_crit("Calling SCM to disable SPMI PMIC arbiter\n");
		scm_call_atomic1(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER, 0);
	}
}

static void __msm_power_off(int lower_pshold)
{
	printk(KERN_CRIT "Powering off the SoC\n");
#ifdef CONFIG_MSM_DLOAD_MODE
	set_dload_mode(0);
#endif
	pm8xxx_reset_pwr_off(0);
	qpnp_pon_system_pwr_off(PON_POWER_OFF_SHUTDOWN);

	if (lower_pshold) {
		if (!use_restart_v2()) {
			__raw_writel(0, PSHOLD_CTL_SU);
		} else {
			halt_spmi_pmic_arbiter();
			__raw_writel(0, MSM_MPM2_PSHOLD_BASE);
		}

		mdelay(10000);
		printk(KERN_ERR "Powering off has failed\n");
	}
	return;
}

static void msm_power_off(void)
{
	/* MSM initiated power off, lower ps_hold */
	__msm_power_off(1);
}

static void cpu_power_off(void *data)
{
	int rc;

	pr_err("PMIC Initiated shutdown %s cpu=%d\n", __func__,
						smp_processor_id());
	if (smp_processor_id() == 0) {
		/*
		 * PMIC initiated power off, do not lower ps_hold, pmic will
		 * shut msm down
		 */
		__msm_power_off(0);

		pet_watchdog();
		pr_err("Calling scm to disable arbiter\n");
		/* call secure manager to disable arbiter and never return */
		rc = scm_call_atomic1(SCM_SVC_PWR,
						SCM_IO_DISABLE_PMIC_ARBITER, 1);

		pr_err("SCM returned even when asked to busy loop rc=%d\n", rc);
		pr_err("waiting on pmic to shut msm down\n");
	}

	preempt_disable();
	while (1)
		;
}

static irqreturn_t resout_irq_handler(int irq, void *dev_id)
{
	pr_warn("%s PMIC Initiated shutdown\n", __func__);
	oops_in_progress = 1;
	smp_call_function_many(cpu_online_mask, cpu_power_off, NULL, 0);
	if (smp_processor_id() == 0)
		cpu_power_off(NULL);
	preempt_disable();
	while (1)
		;
	return IRQ_HANDLED;
}


extern unsigned int set_ram_test_flag;
static void msm_restart_prepare(const char *cmd)
{
#if defined(CONFIG_MACH_MSM8926_AKA_CN) || defined(CONFIG_MACH_MSM8926_AKA_KR) || defined(CONFIG_MACH_MSM8926_B2LN_KR) || defined(CONFIG_MACH_MSM8926_JAGN_KR)
	int Reset_type = 0; //default Hard reset
#endif
#ifdef CONFIG_MSM_DLOAD_MODE

	/* This looks like a normal reboot at this point. */
	set_dload_mode(0);

	/* Write download mode flags if we're panic'ing */
	set_dload_mode(in_panic);

#ifndef CONFIG_LGE_HANDLE_PANIC
	/* Write download mode flags if restart_mode says so */
	if (restart_mode == RESTART_DLOAD)
		set_dload_mode(1);
#endif

	/* Kill download mode if master-kill switch is set */
	if (!download_mode)
		set_dload_mode(0);
#endif

	pm8xxx_reset_pwr_off(1);

	/* Hard reset the PMIC unless memory contents must be maintained. */
#if defined(CONFIG_MACH_MSM8926_AKA_CN) || defined(CONFIG_MACH_MSM8926_AKA_KR) || defined(CONFIG_MACH_MSM8926_B2LN_KR) || defined(CONFIG_MACH_MSM8926_JAGN_KR)

	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)
		    ||!strncmp(cmd, "recovery", 8)
		    ||!strncmp(cmd, "fota", 4)
#ifdef CONFIG_LGE_BNR_RECOVERY_REBOOT
		/* PC Sync B&R : Add restart reason */
		    ||!strncmp(cmd, "--bnr_recovery", 14)
#endif
		    ||!strncmp(cmd, "rtc", 3)
		    ||!strncmp(cmd, "oem-", 4)
		    ||!strncmp(cmd, "edl", 3)
#ifdef CONFIG_LGE_LCD_OFF_DIMMING
		    ||!strncmp(cmd, "LCD off", 7)
		    ||!strncmp(cmd, "FOTA OUT LCD off", 16)
		    ||!strncmp(cmd, "FOTA LCD off", 12)
#endif
		)
		    Reset_type = 1; //Warm reset;
	}
#ifdef CONFIG_LAF_G_DRIVER
	if (get_dload_mode() || Reset_type || (restart_mode == RESTART_DLOAD))
#else
	if (get_dload_mode() || Reset_type )
#endif
#else //CONFIG_MACH_MSM8926_AKA_CN  CONFIG_MACH_MSM8926_AKA_KR CONFIG_MACH_MSM8926_B2LN_KR CONFIG_MACH_MSM8926_JAGN_KR

#ifdef CONFIG_LAF_G_DRIVER
	if (get_dload_mode() || in_panic || (cmd != NULL && cmd[0] != '\0') || (restart_mode == RESTART_DLOAD))
#else
	if (get_dload_mode() || in_panic || (cmd != NULL && cmd[0] != '\0'))
#endif
#endif
		qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
	else
		qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET);

	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)) {
			__raw_writel(0x77665500, restart_reason);
		} else if (!strncmp(cmd, "recovery", 8)) {
			__raw_writel(0x77665502, restart_reason);
		} else if (!strncmp(cmd, "fota", 4)) {
			__raw_writel(0x77665566, restart_reason);
#ifdef CONFIG_LGE_BNR_RECOVERY_REBOOT
			/* PC Sync B&R : Add restart reason */
		} else if (!strncmp(cmd, "--bnr_recovery", 14)) {
			__raw_writel(0x77665555, restart_reason);
#endif
#ifdef CONFIG_LGE_LCD_OFF_DIMMING
        } else if (!strncmp(cmd, "FOTA LCD off", 12)) {
            __raw_writel(0x77665560, restart_reason);
		} else if (!strncmp(cmd, "FOTA OUT LCD off", 16)) {
            __raw_writel(0x77665561, restart_reason);
		} else if (!strncmp(cmd, "LCD off", 7)) {
            __raw_writel(0x77665562, restart_reason);
#endif
		} else if (!strcmp(cmd, "rtc")) {
			__raw_writel(0x77665503, restart_reason);
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned long code;
			code = simple_strtoul(cmd + 4, NULL, 16) & 0xff;
			__raw_writel(0x6f656d00 | code, restart_reason);
		} else if (!strncmp(cmd, "edl", 3)) {
			enable_emergency_dload_mode();
		} else {
			__raw_writel(0x77665501, restart_reason);
		}
	}
#ifdef CONFIG_LGE_HANDLE_PANIC
	else {
		__raw_writel(0x77665503, restart_reason);
	}

	if (restart_mode == RESTART_DLOAD)
	{
	 if(set_ram_test_flag)  //add ram test flag ..
	 {
	 	lge_set_restart_reason(0xABCDEFAB);
		set_ram_test_flag=0;
	 }	
	 else
	 {
	 	lge_set_restart_reason(LAF_DLOAD_MODE);
	 }
	}
	if (in_panic) {
		lge_set_panic_reason();
	}
	{// write fb addr for crash handler display
		extern unsigned long msm_fb_phys_addr_backup;
		lge_set_fb1_addr((unsigned int) msm_fb_phys_addr_backup);
	}
#endif
	flush_cache_all();
	outer_flush_all();
}

void msm_restart(char mode, const char *cmd)
{
	printk(KERN_NOTICE "Going down for restart now\n");

	msm_restart_prepare(cmd);

	if (!use_restart_v2()) {
		__raw_writel(0, msm_tmr0_base + WDT0_EN);
		if (!(machine_is_msm8x60_fusion() ||
		      machine_is_msm8x60_fusn_ffa())) {
			mb();
			 /* Actually reset the chip */
			__raw_writel(0, PSHOLD_CTL_SU);
			mdelay(5000);
			pr_notice("PS_HOLD didn't work, falling back to watchdog\n");
		}

#ifdef CONFIG_KEXEC_HARDBOOT
static void msm_kexec_hardboot_hook(void)
{
	set_dload_mode(0);

	// Set PMIC to restart-on-poweroff
	pm8xxx_reset_pwr_off(1);

	// These are executed on normal reboot, but with kexec-hardboot,
	// they reboot/panic the system immediately.
#if 0
	qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);

	/* Needed to bypass debug image on some chips */
	msm_disable_wdog_debug();
	halt_spmi_pmic_arbiter();
#endif
}
#endif

		__raw_writel(1, msm_tmr0_base + WDT0_RST);
		__raw_writel(5*0x31F3, msm_tmr0_base + WDT0_BARK_TIME);
		__raw_writel(0x31F3, msm_tmr0_base + WDT0_BITE_TIME);
		__raw_writel(1, msm_tmr0_base + WDT0_EN);
	} else {
		/* Needed to bypass debug image on some chips */
		msm_disable_wdog_debug();
		halt_spmi_pmic_arbiter();
		__raw_writel(0, MSM_MPM2_PSHOLD_BASE);
	}

	mdelay(10000);
	printk(KERN_ERR "Restarting has failed\n");
}

#ifdef CONFIG_KEXEC_HARDBOOT
	kexec_hardboot_hook = msm_kexec_hardboot_hook;
#endif

static int __init msm_pmic_restart_init(void)
{
	int rc;

	if (use_restart_v2())
		return 0;

	if (pmic_reset_irq != 0) {
		rc = request_any_context_irq(pmic_reset_irq,
					resout_irq_handler, IRQF_TRIGGER_HIGH,
					"restart_from_pmic", NULL);
		if (rc < 0)
			pr_err("pmic restart irq fail rc = %d\n", rc);
	} else {
		pr_warn("no pmic restart interrupt specified\n");
	}

	return 0;
}

late_initcall(msm_pmic_restart_init);

static int __init msm_restart_init(void)
{
#ifdef CONFIG_MSM_DLOAD_MODE
	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);
	dload_mode_addr = MSM_IMEM_BASE + DLOAD_MODE_ADDR;
	emergency_dload_mode_addr = MSM_IMEM_BASE +
		EMERGENCY_DLOAD_MODE_ADDR;
	set_dload_mode(download_mode);
#endif
	msm_tmr0_base = msm_timer_get_timer0_base();
	restart_reason = MSM_IMEM_BASE + RESTART_REASON_ADDR;
	pm_power_off = msm_power_off;

	if (scm_is_call_available(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER) > 0)
		scm_pmic_arbiter_disable_supported = true;

#ifdef CONFIG_LGE_HANDLE_PANIC
	/* Set default restart_reason to TZ crash.
	 * If can't be set explicit, it causes by TZ */
	__raw_writel(LGE_RB_MAGIC | LGE_ERR_TZ, restart_reason);
#endif

	return 0;
}
early_initcall(msm_restart_init);
