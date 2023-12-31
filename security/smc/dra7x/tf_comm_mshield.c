/**
 * Copyright (c) 2011 Trusted Logic S.A.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <asm/div64.h>
#include <asm/system.h>
#include <asm/cputype.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/jiffies.h>
#include <linux/dma-mapping.h>
#include <linux/cpu.h>
#include <omap-secure.h>

#include <asm/cacheflush.h>

#include "s_version.h"
#include "tf_defs.h"
#include "tf_comm.h"
#include "tf_util.h"
#include "tf_conn.h"
#include "tf_zebra.h"
#include "tf_crypto.h"
#include "common.h"

/*--------------------------------------------------------------------------
 * Internal constants
 *-------------------------------------------------------------------------- */

/* RPC commands */
#define RPC_CMD_YIELD	0x00
#define RPC_CMD_INIT	0x01
#define RPC_CMD_TRACE	0x02

/* RPC return values to secure world */
#define RPC_SUCCESS			0x00000000
#define RPC_ERROR_BAD_PARAMETERS	0xFFFF0006
#define RPC_ERROR_CONNECTION_PROTOCOL	0xFFFF3020

/*
 * RPC call status
 *
 * 0: the secure world yielded due to an interrupt
 * 1: the secure world yielded on an RPC (no public world thread is handling it)
 * 2: the secure world yielded on an RPC and the response to that RPC is now in
 *    place
 */
#define RPC_ADVANCEMENT_NONE		0
#define RPC_ADVANCEMENT_PENDING		1
#define RPC_ADVANCEMENT_FINISHED	2

u32 g_RPC_advancement;
u32 g_secure_task_id;
u32 g_service_end;

/*
 * Secure ROMCode HAL API Identifiers
 */
struct omap_rom_hal_api_indexes {
	u32 sdp_runtime_init;
	u32 lm_paload;
	u32 lm_paunload;
	u32 task_mgr_rpcinit;
	u32 km_getsecureromcodecrc;
	u32 sec_l3_ram_resize;
	u32 hwaturnoff;
	u32 activatehwapwrmgrpatch;
	u32 setup_secure_scratch_mem;
	u32 free_secure_scratch_mem;
	u32 pa_image_decrypt;
};

static const struct omap_rom_hal_api_indexes *hal_indexes;

#ifdef CONFIG_ARCH_OMAP4
static const struct omap_rom_hal_api_indexes omap4_hal_indexes = {
	.sdp_runtime_init       = 0x04,
	.lm_paload              = 0x05,
	.lm_paunload            = 0x07,
	.task_mgr_rpcinit       = 0x08,
	.km_getsecureromcodecrc = 0x0b,
	.sec_l3_ram_resize      = 0x17,
	.hwaturnoff             = 0x29,
	.activatehwapwrmgrpatch = 0x2a,
};
#endif
#if (defined(CONFIG_SOC_OMAP5) || defined(CONFIG_SOC_DRA7XX))
static const struct omap_rom_hal_api_indexes omap5_hal_indexes = {
	.sdp_runtime_init       = 0x06,
	.lm_paload              = 0x07,
	.lm_paunload            = 0x09,
	.task_mgr_rpcinit       = 0x0a,
	.km_getsecureromcodecrc = 0x0d,
	.sec_l3_ram_resize      = 0x19,
	.hwaturnoff             = 0x209,
	.activatehwapwrmgrpatch = 0x20a,
	.setup_secure_scratch_mem = 0x212,
	.free_secure_scratch_mem  = 0x213,
	.pa_image_decrypt         = 0x214,
};
#endif

#define API_HAL_SDP_RUNTIMEINIT_INDEX        (hal_indexes->sdp_runtime_init)
#define API_HAL_LM_PALOAD_INDEX              (hal_indexes->lm_paload)
#define API_HAL_LM_PAUNLOADALL_INDEX         (hal_indexes->lm_paunload)
#define API_HAL_TASK_MGR_RPCINIT_INDEX       (hal_indexes->task_mgr_rpcinit)
#define API_HAL_KM_GETSECUREROMCODECRC_INDEX \
	(hal_indexes->km_getsecureromcodecrc)
#define API_HAL_SEC_L3_RAM_RESIZE_INDEX      (hal_indexes->sec_l3_ram_resize)
#define API_HAL_HWATURNOFF_INDEX             (hal_indexes->hwaturnoff)
#define API_HAL_ACTIVATEHWAPWRMGRPATCH_INDEX \
	(hal_indexes->activatehwapwrmgrpatch)
#define API_HAL_SETUPSECURESCRATCHMEM_INDEX \
	(hal_indexes->setup_secure_scratch_mem)
#define API_HAL_FREESECURESCRATCHMEM_INDEX \
	(hal_indexes->free_secure_scratch_mem)
#define API_HAL_PAIMAGEDECRYPT_INDEX \
	(hal_indexes->pa_image_decrypt)

/* SE entry flags */
#define FLAG_START_HAL_CRITICAL     0x4
#define FLAG_IRQFIQ_MASK            0x3
#define FLAG_IRQ_ENABLE             0x2
#define FLAG_FIQ_ENABLE             0x1

#define SMICODEPUB_IRQ_END	0xFE
#define SMICODEPUB_FIQ_END	0xFD
#define SMICODEPUB_RPC_END	0xFC

struct tf_ns_pa_info {
	void *certificate;
	void *parameters;
	void *results;
};

#ifdef CONFIG_HAS_WAKELOCK
static struct wake_lock g_tf_wake_lock;
#endif

static struct clockdomain *smc_l4_sec_clkdm;

static u32 tf_secure_dispatcher(u32 app_id, u32 flags, u32 nargs,
	u32 arg1, u32 arg2, u32 arg3, u32 arg4);

int tf_early_init(void)
{
	g_secure_task_id = 0;

	dpr_info("SMC early init\n");

#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&g_tf_wake_lock, WAKE_LOCK_SUSPEND, TF_DEVICE_BASE_NAME);
#endif

#if (defined(CONFIG_SOC_OMAP5) || defined(CONFIG_SOC_DRA7XX))
	if (soc_is_omap54xx() || soc_is_dra7xx()) {
		smc_l4_sec_clkdm = clkdm_lookup("l4sec_clkdm");
		hal_indexes = &omap5_hal_indexes;
	} else
#endif
#ifdef CONFIG_ARCH_OMAP4
	if (cpu_is_omap44xx()) {
		smc_l4_sec_clkdm = clkdm_lookup("l4_secure_clkdm");
		hal_indexes = &omap4_hal_indexes;
	} else
#endif
	{
		return -ENOENT;
	}

	if (smc_l4_sec_clkdm == NULL)
		return -EFAULT;

	dpr_info("SMC early init successful\n");
	return 0;
}

/*
 * The timeout timer used to power off clocks
 */
#define INACTIVITY_TIMER_TIMEOUT 10 /* ms */

static DEFINE_SPINLOCK(clk_timer_lock);
static struct timer_list tf_crypto_clock_timer;
static int tf_crypto_clock_enabled;

void tf_clock_timer_init(void)
{
	init_timer(&tf_crypto_clock_timer);
	tf_crypto_clock_enabled = 0;

	/* HWA Clocks Patch init */
	if (!tf_is_omap5_es2())
		tf_secure_dispatcher(API_HAL_ACTIVATEHWAPWRMGRPATCH_INDEX,
			0, 0, 0, 0, 0, 0);
}

u32 tf_try_disabling_secure_hwa_clocks(u32 mask)
{
	return tf_secure_dispatcher(API_HAL_HWATURNOFF_INDEX,
		FLAG_START_HAL_CRITICAL, 1, mask, 0, 0, 0);
}

static void tf_clock_timer_cb(unsigned long data)
{
	unsigned long flags;
	u32 ret = 0;

	spin_lock_irqsave(&clk_timer_lock, flags);

	/*
	 * If one of the HWA is used (by secure or public) the timer
	 * function cuts all the HWA clocks
	 */
	if (tf_crypto_clock_enabled) {
		dprintk(KERN_INFO "%s; tf_crypto_clock_enabled = %d\n",
			__func__, tf_crypto_clock_enabled);
		goto restart;
	}

	ret = tf_crypto_turn_off_clocks();

	/*
	 * From MShield-DK 1.3.3 sources:
	 *
	 * Digest: 1 << 0
	 * DES   : 1 << 1
	 * AES1  : 1 << 2
	 * AES2  : 1 << 3
	 * RNG   : 1 << 4
	 * PKA   : 1 << 5
	 *
	 * Clock patch active: 1 << 7
	 */
	if (ret & 0x3f)
		goto restart;

#ifdef CONFIG_HAS_WAKELOCK
	wake_unlock(&g_tf_wake_lock);
#endif
	clkdm_allow_idle(smc_l4_sec_clkdm);

	spin_unlock_irqrestore(&clk_timer_lock, flags);

	return;

restart:
	dprintk("%s: will wait one more time ret=0x%x\n", __func__, ret);
	mod_timer(&tf_crypto_clock_timer,
		jiffies + msecs_to_jiffies(INACTIVITY_TIMER_TIMEOUT));

	spin_unlock_irqrestore(&clk_timer_lock, flags);
}

void tf_clock_timer_start(void)
{
	unsigned long flags;

	if (tf_is_omap5_es2())
		return;

	spin_lock_irqsave(&clk_timer_lock, flags);

	tf_crypto_clock_enabled++;
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&g_tf_wake_lock);
#endif
	clkdm_wakeup(smc_l4_sec_clkdm);

	/* Stop the timer if already running */
	if (timer_pending(&tf_crypto_clock_timer))
		del_timer(&tf_crypto_clock_timer);

	/* Configure the timer */
	tf_crypto_clock_timer.expires =
		 jiffies + msecs_to_jiffies(INACTIVITY_TIMER_TIMEOUT);
	tf_crypto_clock_timer.function = tf_clock_timer_cb;

	add_timer(&tf_crypto_clock_timer);

	spin_unlock_irqrestore(&clk_timer_lock, flags);
}

void tf_clock_timer_stop(void)
{
	unsigned long flags;

	if (tf_is_omap5_es2())
		return;

	spin_lock_irqsave(&clk_timer_lock, flags);
	tf_crypto_clock_enabled--;
	spin_unlock_irqrestore(&clk_timer_lock, flags);
}

/*
 * Function responsible for formatting parameters to pass from NS world to
 * S world
 */
static u32 tf_secure_dispatcher(u32 app_id, u32 flags, u32 nargs,
	u32 arg1, u32 arg2, u32 arg3, u32 arg4)
{
	u32 ret;
	unsigned long iflags;
	u32 pub2sec_args[5] = {0, 0, 0, 0, 0};

	pub2sec_args[0] = nargs;
	pub2sec_args[1] = arg1;
	pub2sec_args[2] = arg2;
	pub2sec_args[3] = arg3;
	pub2sec_args[4] = arg4;

	/* Make sure parameters are visible to the secure world */
	dmac_flush_range((void *)&pub2sec_args,
		(void *)(((u32)&pub2sec_args) + sizeof(pub2sec_args)));
	outer_clean_range(__pa(pub2sec_args),
		__pa(pub2sec_args) + 5*sizeof(u32));
	wmb();

	/*
	 * Put L4 Secure clock domain to SW_WKUP so that modules are accessible
	 * Not relevant for OMAP5 ES2+ as HW modules support the HW AUTO mode
	 */
	if (!tf_is_omap5_es2())
		clkdm_wakeup(smc_l4_sec_clkdm);

	local_irq_save(iflags);

	/* proc_id is always 0 */
	ret = schedule_secure_world(app_id, 0, flags, __pa(pub2sec_args));

	local_irq_restore(iflags);

	/* Restore the HW_SUP on L4 Sec clock domain so hardware can idle */
	if (!tf_is_omap5_es2() && (app_id != API_HAL_HWATURNOFF_INDEX) &&
	    (!timer_pending(&tf_crypto_clock_timer))) {
		(void) tf_crypto_turn_off_clocks();
		clkdm_allow_idle(smc_l4_sec_clkdm);
	}
	return ret;
}

/* Yields the Secure World */
int tf_schedule_secure_world(struct tf_comm *comm)
{
	int status = 0;
	int ret;
	unsigned long iflags;
	u32 appli_id;

	tf_set_current_time(comm);

	local_irq_save(iflags);

	switch (g_RPC_advancement) {
	case  RPC_ADVANCEMENT_NONE:
		/* Return from IRQ */
		appli_id = SMICODEPUB_IRQ_END;
		break;
	case  RPC_ADVANCEMENT_PENDING:
		/* nothing to do in this case */
		goto exit;
	default:
	case RPC_ADVANCEMENT_FINISHED:
		appli_id = SMICODEPUB_RPC_END;
		g_RPC_advancement = RPC_ADVANCEMENT_NONE;
		break;
	}

	tf_clock_timer_start();

	g_service_end = 1;
	/* yield to the Secure World */
	ret = tf_secure_dispatcher(appli_id, /* app_id */
	   0, 0,        /* flags, nargs */
	   0, 0, 0, 0); /* arg1, arg2, arg3, arg4 */
	if (g_service_end != 0) {
		dpr_err("Service End ret=%X\n", ret);

		if (ret == 0) {
			dmac_flush_range((void *)comm->l1_buffer,
				(void *)(((u32)(comm->l1_buffer)) +
					PAGE_SIZE));
			outer_inv_range(__pa(comm->l1_buffer),
				__pa(comm->l1_buffer) +
				PAGE_SIZE);

			ret = comm->l1_buffer->exit_code;

			dpr_err("SMC PA failure ret=%X\n", ret);
			if (ret == 0)
				ret = -EFAULT;
		}
		clear_bit(TF_COMM_FLAG_PA_AVAILABLE, &comm->flags);
		tf_secure_dispatcher(API_HAL_LM_PAUNLOADALL_INDEX,
			FLAG_START_HAL_CRITICAL, 0, 0, 0, 0, 0);
		status = ret;
	}

	tf_clock_timer_stop();

exit:
	local_irq_restore(iflags);

	return status;
}

/* Initializes the SE (SDP, SRAM resize, RPC handler) */
static int tf_se_init(struct tf_comm *comm,
	u32 sdp_backing_store_addr, u32 sdp_bkext_store_addr)
{
	int error;
	unsigned int crc;
	unsigned int sram_size;

	if (comm->se_initialized) {
		dpr_info("%s: SE already initialized... nothing to do\n",
			__func__);
		return 0;
	}

	/* Secure CRC read */
	dpr_info("%s: Secure CRC Read...\n", __func__);

	crc = omap_secure_dispatcher(
		API_HAL_KM_GETSECUREROMCODECRC_INDEX,
		0, 0, 0, 0, 0, 0);
	pr_info("SMC: SecureCRC=0x%08X\n", crc);

	if (soc_is_omap54xx() || soc_is_dra7xx())
		sram_size = 120*1024;
	else if (cpu_is_omap44xx())
		sram_size = 52*1024;
	else
		return -EFAULT;

	/* SRAM resize */
	dpr_info("%s: SRAM resize (0x%08X)...\n", __func__, sram_size);
	error = omap_secure_dispatcher(
		API_HAL_SEC_L3_RAM_RESIZE_INDEX,
		FLAG_FIQ_ENABLE | FLAG_START_HAL_CRITICAL, 1,
		sram_size, 0, 0, 0);

	if (error == API_HAL_RET_VALUE_OK) {
		dpr_info("%s: SRAM resize OK\n", __func__);
	} else {
		dpr_err("%s: SRAM resize failed [0x%x]\n", __func__, error);
		goto error;
	}

	/* SDP init */
	dpr_info("%s: SDP runtime init...\n"
		"(sdp_backing_store_addr=%x, sdp_bkext_store_addr=%x)\n",
		__func__,
		sdp_backing_store_addr, sdp_bkext_store_addr);
	error = omap_secure_dispatcher(API_HAL_SDP_RUNTIMEINIT_INDEX,
		FLAG_FIQ_ENABLE | FLAG_START_HAL_CRITICAL, 2,
		sdp_backing_store_addr, sdp_bkext_store_addr, 0, 0);

	if (error == API_HAL_RET_VALUE_OK) {
		dpr_info("%s: SDP runtime init OK\n", __func__);
	} else {
		dpr_err("%s: SDP runtime init failed [0x%x]\n",
			__func__, error);
		goto error;
	}

	/* RPC init */
	dpr_info("%s: RPC init...\n", __func__);
	error = omap_secure_dispatcher(API_HAL_TASK_MGR_RPCINIT_INDEX,
		FLAG_START_HAL_CRITICAL, 1,
		(u32) (u32(*const) (u32, u32, u32, u32)) &rpc_handler, 0, 0, 0);

	if (error == API_HAL_RET_VALUE_OK) {
		dpr_info("%s: RPC init OK\n", __func__);
	} else {
		dpr_err("%s: RPC init failed [0x%x]\n", __func__, error);
		goto error;
	}

	comm->se_initialized = true;

	return 0;

error:
	return -EFAULT;
}

/* Check protocol version returned by the PA */
static u32 tf_rpc_init(struct tf_comm *comm)
{
	u32 protocol_version;
	u32 rpc_error = RPC_SUCCESS;

	dpr_info("%s(%p)\n", __func__, comm);

	spin_lock(&(comm->lock));

	protocol_version = comm->l1_buffer->protocol_version;

	if ((GET_PROTOCOL_MAJOR_VERSION(protocol_version))
			!= TF_S_PROTOCOL_MAJOR_VERSION) {
		dpr_err("SMC: Unsupported protocol version (0x%02x)\n",
			GET_PROTOCOL_MAJOR_VERSION(protocol_version));
		rpc_error = RPC_ERROR_CONNECTION_PROTOCOL;
	} else {
		rpc_error = RPC_SUCCESS;
	}

	spin_unlock(&(comm->lock));

	return rpc_error;
}

static u32 tf_rpc_trace(struct tf_comm *comm)
{
#ifdef CONFIG_SECURE_TRACE
	spin_lock(&(comm->lock));
	pr_info("SMC PA: %s", comm->l1_buffer->rpc_trace_buffer);
	spin_unlock(&(comm->lock));
#endif
	return RPC_SUCCESS;
}

/*
 * Handles RPC calls
 *
 * Returns:
 *  - RPC_NO if there was no RPC to execute
 *  - RPC_YIELD if there was a Yield RPC
 *  - RPC_NON_YIELD if there was a non-Yield RPC
 */

int tf_rpc_execute(struct tf_comm *comm)
{
	u32 rpc_command;
	u32 rpc_error = RPC_NO;

#ifdef CONFIG_TF_DRIVER_DEBUG_SUPPORT
#if (defined(CONFIG_SOC_OMAP5) || defined(CONFIG_SOC_DRA7XX))
	BUG_ON((smp_processor_id() & 0x00000003) != 0);
#else
	BUG_ON((hard_smp_processor_id() & 0x00000003) != 0);
#endif
#endif

	/* Lock the RPC */
	mutex_lock(&(comm->rpc_mutex));

	rpc_command = comm->l1_buffer->rpc_command;

	if (g_RPC_advancement == RPC_ADVANCEMENT_PENDING) {
		switch (rpc_command) {
		case RPC_CMD_YIELD:
			dpr_info("%s: RPC_CMD_YIELD\n", __func__);

			rpc_error = RPC_YIELD;
			comm->l1_buffer->rpc_status = RPC_SUCCESS;
			break;

		case RPC_CMD_TRACE:
			rpc_error = RPC_NON_YIELD;
			comm->l1_buffer->rpc_status = tf_rpc_trace(comm);
			break;

		default:
			comm->l1_buffer->rpc_status = RPC_SUCCESS;
			rpc_error = RPC_NON_YIELD;
			break;
		}
		g_RPC_advancement = RPC_ADVANCEMENT_FINISHED;
	}

	mutex_unlock(&(comm->rpc_mutex));

	return rpc_error;
}

/*--------------------------------------------------------------------------
 * Power management
 *-------------------------------------------------------------------------- */
 /*
 * Perform a Secure World shutdown operation.
 * The routine does not return if the operation succeeds.
 * the routine returns an appropriate error code if
 * the operation fails.
 */
int tf_pm_shutdown(struct tf_comm *comm)
{
#ifndef CONFIG_SOC_DRA7XX
	int error;
	union tf_command command;
	union tf_answer answer;

	dpr_info("%s()\n", __func__);

	memset(&command, 0, sizeof(command));

	command.header.message_type = TF_MESSAGE_TYPE_MANAGEMENT;
	command.header.message_size =
			(sizeof(struct tf_command_management) -
				sizeof(struct tf_command_header))/sizeof(u32);

	command.management.command = TF_MANAGEMENT_SHUTDOWN;

	error = tf_send_receive(
		comm,
		&command,
		&answer,
		NULL,
		false);

	if (error != 0) {
		dpr_err("%s(): tf_send_receive failed (error %d)!\n",
			__func__, error);
		return error;
	}

#ifdef CONFIG_TF_DRIVER_DEBUG_SUPPORT
	if (answer.header.error_code != 0)
		dpr_err("tf_driver: shutdown failed.\n");
	else
		dpr_info("tf_driver: shutdown succeeded.\n");
#endif

	return answer.header.error_code;
#else // CONFIG_SOC_DRA7XX
	/* DRA7xx SMC secure core does not handle power management messages */
	return 0;
#endif
}


int tf_pm_hibernate(struct tf_comm *comm)
{
	dpr_info("%s()\n", __func__);
	return 0;
}

int tf_pm_resume(struct tf_comm *comm)
{

	dpr_info("%s()\n", __func__);
#ifdef CONFIG_SMC_KERNEL_CRYPTO
	tf_aes_pm_resume();
#endif
	return 0;
}

/*--------------------------------------------------------------------------
 * Initialization
 *-------------------------------------------------------------------------- */

int tf_init(struct tf_comm *comm)
{
	spin_lock_init(&(comm->lock));
	comm->flags = 0;
	comm->l1_buffer = NULL;

	comm->se_initialized = false;

	init_waitqueue_head(&(comm->wait_queue));
	mutex_init(&(comm->rpc_mutex));

	if (tf_crypto_init() != 0)
		return -EFAULT;

	pr_info("%s\n", S_VERSION_STRING);

	register_smc_public_crypto_digest();
	register_smc_public_crypto_aes();

	return 0;
}

/* Start the SMC PA */
int tf_start(struct tf_comm *comm,
	u32 workspace_addr, u32 workspace_size,
	u8 *pa_buffer, u32 pa_size,
	u32 conf_descriptor,
	u32 conf_offset, u32 conf_size)
{
	struct tf_l1_shared_buffer *l1_shared_buffer = NULL;
	struct tf_ns_pa_info pa_info;
	int ret;
	u32 descr;
	u32 secure_scratch_mem_addr;
	u32 secure_scratch_mem_size;
	u32 pa_load_mem_addr;
	u32 pa_load_mem_size;
	u32 sdp_backing_store_addr;
	u32 sdp_bkext_store_addr;
	bool is_encrypted_pa = false;
#ifdef CONFIG_SMP
	long ret_affinity;
	cpumask_t saved_cpu_mask;
	cpumask_t local_cpu_mask = CPU_MASK_NONE;

	/* OMAP4 Secure ROM Code can only be called from CPU0. */
	cpu_set(0, local_cpu_mask);
	sched_getaffinity(0, &saved_cpu_mask);
	ret_affinity = sched_setaffinity(0, &local_cpu_mask);
	if (ret_affinity != 0)
		dpr_err("sched_setaffinity #1 -> 0x%lX", ret_affinity);
#endif
	/* 1MB for SDP backing store, at the end */
	workspace_size -= SZ_1M;
	sdp_backing_store_addr = workspace_addr + workspace_size;

	/* 128KB for SDP extended data (hashes), at the end */
	workspace_size -= 0x20000;
	sdp_bkext_store_addr = workspace_addr + workspace_size;

	/* Reserve 0x60000 memory to copy PA binary, at the end */
	workspace_size -= 0x60000;
	pa_load_mem_size = 0x60000;
	pa_load_mem_addr = workspace_addr + workspace_size;

	/* Reserve 0x80000 for encrypted PA, adjacent to firewalled region */
	workspace_size -= 0x80000;
	secure_scratch_mem_size = 0x80000;
	secure_scratch_mem_addr = workspace_addr + workspace_size;

	tf_clock_timer_start();

	if (test_bit(TF_COMM_FLAG_PA_AVAILABLE, &comm->flags)) {
		dpr_err("%s(%p): The SMC PA is already started\n",
			__func__, comm);

		ret = -EFAULT;
		goto error1;
	}

	if (sizeof(struct tf_l1_shared_buffer) != PAGE_SIZE) {
		dpr_err("%s(%p): The L1 structure size is incorrect!\n",
			__func__, comm);
		ret = -EFAULT;
		goto error1;
	}

	if (pa_size > pa_load_mem_size) {
		dpr_err("%s(%p): PA size is incorrect (%x)\n",
			__func__, comm, pa_size);
		ret = -EFAULT;
		goto error1;
	}

	if (pa_size > 4) {
		u8 header_magic[4];
		if (0 == copy_from_user(header_magic, pa_buffer, 4)) {
			if (header_magic[0] == 0x5b &&
			    header_magic[1] == 0xdd &&
			    header_magic[2] == 0x0b)
				is_encrypted_pa = true;
		}
	}

	{
		void *tmp;
		tmp = ioremap_nocache(pa_load_mem_addr, pa_size);
		if (copy_from_user(tmp, pa_buffer, pa_size)) {
			iounmap(tmp);
			dpr_err("%s(%p): Cannot access PA buffer (%p)\n",
				__func__, comm, (void *) pa_buffer);
			ret = -EFAULT;
			goto error1;
		}
		iounmap(tmp);
	}

	ret = tf_se_init(comm, sdp_backing_store_addr,
		sdp_bkext_store_addr);
	if (ret != 0) {
		dpr_err("%s(%p): SE initialization failed\n", __func__, comm);
		goto error1;
	}

	if (true == is_encrypted_pa) {
		/* Secure scratch memory init */
		dpr_info("%s: Secure Scratch Memory Setup...\n"
				"(secure_scratch_mem_addr=%x, secure_scratch_mem_size=%x)\n",
				__func__,
				secure_scratch_mem_addr, secure_scratch_mem_size);
		ret = omap_secure_dispatcher(API_HAL_SETUPSECURESCRATCHMEM_INDEX,
				FLAG_FIQ_ENABLE | FLAG_START_HAL_CRITICAL, 2,
				secure_scratch_mem_addr, secure_scratch_mem_size, 0, 0);

		if (ret == API_HAL_RET_VALUE_OK) {
			dpr_info("%s: Secure Scratch Memory Setup OK\n", __func__);
		} else {
			dpr_err("%s: Secure Scratch Memory Setup failed [0x%x]\n",
					__func__, ret);
			goto error1;
		}

		dpr_info("%s: PA Decrypt...\n"
				"(workspace_addr=%x, pa_size=%x)\n",
				__func__, pa_load_mem_addr, pa_size);
		ret = omap_secure_dispatcher(API_HAL_PAIMAGEDECRYPT_INDEX,
				FLAG_FIQ_ENABLE | FLAG_START_HAL_CRITICAL, 3,
				pa_load_mem_addr, pa_size, secure_scratch_mem_addr, 0);
		if (ret != API_HAL_RET_VALUE_OK) {
			dpr_err("SMC: Error decrypting the PA [0x%x]\n", ret);
			goto error1;
		} else {
			dpr_info("%s: PA Decrypt OK\n",	__func__);
		}
	}

	omap_secure_dispatcher_switch(tf_secure_dispatcher);

	l1_shared_buffer =
		(struct tf_l1_shared_buffer *)
			internal_get_zeroed_page(GFP_KERNEL);

	if (l1_shared_buffer == NULL) {
		dpr_err("%s(%p): Ouf of memory!\n", __func__, comm);

		ret = -ENOMEM;
		goto error1;
	}
	/* Ensure the page is mapped */
	__set_page_locked(virt_to_page(l1_shared_buffer));

	dpr_info("%s(%p): L1SharedBuffer={0x%08x, 0x%08x}\n",
		__func__, comm,
		(u32) l1_shared_buffer, (u32) __pa(l1_shared_buffer));

	descr = tf_get_l2_descriptor_common((u32) l1_shared_buffer,
			current->mm);
	pa_info.certificate = (void*)((is_encrypted_pa)?
		secure_scratch_mem_addr:pa_load_mem_addr);
	pa_info.parameters = (void *) ((uintptr_t) __pa(l1_shared_buffer));
	pa_info.results = (void *) ((uintptr_t)__pa(l1_shared_buffer));

	l1_shared_buffer->l1_shared_buffer_descr = descr & 0xFFF;

	l1_shared_buffer->backing_store_addr = sdp_backing_store_addr;
	l1_shared_buffer->backext_storage_addr = sdp_bkext_store_addr;
	l1_shared_buffer->workspace_addr = workspace_addr;
	l1_shared_buffer->workspace_size = workspace_size;
	l1_shared_buffer->debug_params = 1;

	dpr_info("%s(%p): System Configuration (%d bytes)\n",
		__func__, comm, conf_size);
	dpr_info("%s(%p): Starting PA (%d bytes)...\n",
		__func__, comm, pa_size);

	/*
	 * Make sure all data is visible to the secure world
	 */
	dmac_flush_range((void *)l1_shared_buffer,
		(void *)(((u32)l1_shared_buffer) + PAGE_SIZE));
	outer_clean_range(__pa(l1_shared_buffer),
		__pa(l1_shared_buffer) + PAGE_SIZE);

	dmac_flush_range((void *)&pa_info,
		(void *)(((u32)&pa_info) + sizeof(struct tf_ns_pa_info)));
	outer_clean_range(__pa(&pa_info),
		__pa(&pa_info) + sizeof(struct tf_ns_pa_info));
	wmb();

	spin_lock(&(comm->lock));
	comm->l1_buffer = l1_shared_buffer;
	comm->l1_buffer->conf_descriptor = conf_descriptor;
	comm->l1_buffer->conf_offset     = conf_offset;
	comm->l1_buffer->conf_size       = conf_size;
	spin_unlock(&(comm->lock));
	l1_shared_buffer = NULL;

	/*
	 * Set the OS current time in the L1 shared buffer first. The secure
	 * world uses it as itw boot reference time.
	 */
	tf_set_current_time(comm);

	/* Workaround for issue #6081 */
/*	disable_nonboot_cpus(); */

	/*
	 * Start the SMC PA
	 */
	ret = tf_secure_dispatcher(API_HAL_LM_PALOAD_INDEX,
		FLAG_IRQ_ENABLE | FLAG_FIQ_ENABLE | FLAG_START_HAL_CRITICAL, 1,
		__pa(&pa_info), 0, 0, 0);
	if (ret != API_HAL_RET_VALUE_OK) {
		pr_err("SMC: Error while loading the PA [0x%x]\n", ret);
		goto error2;
	}

	/* Loop until the first S Yield RPC is received */
loop:
	mutex_lock(&(comm->rpc_mutex));

	if (g_RPC_advancement == RPC_ADVANCEMENT_PENDING) {
		dpr_info("%s: Executing CMD=0x%x\n",
			__func__, comm->l1_buffer->rpc_command);

		switch (comm->l1_buffer->rpc_command) {
		case RPC_CMD_YIELD:
			dpr_info("%s: RPC_CMD_YIELD\n", __func__);
			set_bit(TF_COMM_FLAG_L1_SHARED_ALLOCATED,
				&(comm->flags));
			comm->l1_buffer->rpc_status = RPC_SUCCESS;
			break;

		case RPC_CMD_INIT:
			dpr_info("%s: RPC_CMD_INIT\n", __func__);
			comm->l1_buffer->rpc_status = tf_rpc_init(comm);
			break;

		case RPC_CMD_TRACE:
			comm->l1_buffer->rpc_status = tf_rpc_trace(comm);
			break;

		default:
			comm->l1_buffer->rpc_status = RPC_ERROR_BAD_PARAMETERS;
			break;
		}
		g_RPC_advancement = RPC_ADVANCEMENT_FINISHED;
	}

	mutex_unlock(&(comm->rpc_mutex));

	ret = tf_schedule_secure_world(comm);
	if (ret != 0) {
		pr_err("SMC: Error during PA initialization [0x%x]\n", ret);
		goto error2;
	}

	if (!test_bit(TF_COMM_FLAG_L1_SHARED_ALLOCATED, &(comm->flags)))
		goto loop;

	set_bit(TF_COMM_FLAG_PA_AVAILABLE, &comm->flags);
	wake_up(&(comm->wait_queue));
	ret = 0;

	/* Workaround for issue #6081 */
/*	enable_nonboot_cpus(); */

	goto exit;

error2:
	/* Workaround for issue #6081 */
/*	enable_nonboot_cpus(); */

	spin_lock(&(comm->lock));
	l1_shared_buffer = comm->l1_buffer;
	comm->l1_buffer = NULL;
	spin_unlock(&(comm->lock));

error1:
	if (l1_shared_buffer != NULL) {
		__clear_page_locked(virt_to_page(l1_shared_buffer));
		internal_free_page((unsigned long) l1_shared_buffer);
	}
	omap_secure_dispatcher_switch(NULL);

exit:
	tf_clock_timer_stop();
#ifdef CONFIG_SMP
	ret_affinity = sched_setaffinity(0, &saved_cpu_mask);
	if (ret_affinity != 0)
		dpr_err("sched_setaffinity #2 -> 0x%lX", ret_affinity);
#endif

	if (ret > 0)
		ret = -EFAULT;

	return ret;
}

void tf_terminate(struct tf_comm *comm)
{
	dpr_info("%s(%p)\n", __func__, comm);

	spin_lock(&(comm->lock));
	tf_crypto_terminate();
	spin_unlock(&(comm->lock));
}
