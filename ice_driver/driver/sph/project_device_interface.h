/*
 * NNP-I Linux Driver
 * Copyright (c) 2019, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#ifndef _DEVICE_INTERFACE_INTERNAL_H_
#define _DEVICE_INTERFACE_INTERNAL_H_

#ifdef RING3_VALIDATION
#include <stdint.h>
#include <stdint_ext.h>
#endif

#include "cve_linux_internal.h"
#include "cve_driver_internal.h"

#define LLC_FREQ_MSR 0x620
#define LLC_MASK 0xFFFF8080
#define max_llc_ratio(a) ((a) & 0x7F)
#define min_llc_ratio(a) (((a) >> 8) & 0x7F)
#define MAX_LLCPMON_CONFIG 7
#define MAX_LLCPMON_PREDEF_CONFIG 6

struct hw_revision_t {
	u16	major_rev;
	u16	minor_rev;
};

extern struct kobject *icedrv_kobj;
int is_wd_error(u32 status);
void get_hw_revision(struct cve_device *cve_dev,
				struct hw_revision_t *hw_rev);
int do_reset_device(struct cve_device *cve_dev, uint8_t idc_reset);
void ice_di_start_llc_pmon(struct cve_device *dev, bool pmon_0_1);
void ice_di_read_llc_pmon(struct cve_device *dev);
void cve_print_mmio_regs(struct cve_device *cve_dev);
void store_ecc_err_count(struct cve_device *cve_dev);
int init_platform_data(struct cve_device *cve_dev);
void cleanup_platform_data(struct cve_device *cve_dev);
int project_hook_enable_msi_interrupt(struct cve_os_device *os_dev);
int icedrv_sysfs_init(void);
int hw_config_sysfs_init(struct cve_device *ice_dev);
void hw_config_sysfs_term(struct cve_device *ice_dev);
void icedrv_sysfs_term(void);
void perform_daemon_suspend(struct cve_device *ice_dev);
void perform_daemon_reset(struct cve_device *ice_dev);

/* Init cve_dump register in the device
 * inputs: os_dev - os device handle;
 * return: 0 on success, a negative error code on failure
 */
int project_hook_init_cve_dump_buffer(struct cve_device *dev);

/* free cve_dump register in the device
 * inputs: os_dev - os device handle;
 */
void project_hook_free_cve_dump_buffer(struct cve_device *dev);

struct kobject *get_icedrv_kobj(void);

#ifdef DEBUG_TENSILICA_ENABLE
inline void cve_decouple_debugger_reset(void);
#endif
#define project_hook_device_init(cve_dev)
#define project_hook_device_release(cve_dev)
#define project_hook_read_mmio_register(cve_dev)
#define project_hook_write_mmio_register(cve_dev)
#define project_hook_interrupt_handler_entry(cve_dev)
#define project_hook_interrupt_dpc_handler_entry(cve_dev)
#define project_hook_interrupt_dpc_handler_exit(cve_dev, status)

void project_hook_interrupt_handler_exit(struct cve_device *cve_dev,
		u32 status);
void project_hook_dispatch_new_job(struct cve_device *cve_dev,
					struct ice_network *ntw);
void ice_di_update_page_sz(struct cve_device *cve_dev, u32 *page_sz_array);
int cve_pt_llc_update(pt_entry_t *pt_entry, u32 llc_policy);

void cve_di_set_cve_dump_control_register(struct cve_device *cve_dev,
		uint8_t dumpTrigger, struct di_cve_dump_buffer ice_dump_buf);
void cve_di_set_cve_dump_configuration_register(
		struct cve_device *cve_dev,
		struct di_cve_dump_buffer ice_dump_buf);
int cve_sync_sgt_to_llc(struct sg_table *sgt);

void ice_di_disable_clk_squashing(struct cve_device *dev);

int set_ice_freq(void *ice_freq_config);

int configure_ice_frequency(struct cve_device *dev);

int __init_ice_iccp(struct cve_device *dev);
void __term_ice_iccp(struct cve_device *dev);
#define __no_op_return_success 0

#ifdef RING3_VALIDATION
#define init_icedrv_sysfs() __no_op_return_success
#define term_icedrv_sysfs() __no_op_stub
#define init_icedrv_hw_config(dev) __no_op_return_success
#define term_icedrv_hw_config(dev) __no_op_stub
#define init_ice_iccp(x) __no_op_return_success
#define term_ice_iccp(x) __no_op_stub
#else
#define init_icedrv_sysfs() icedrv_sysfs_init()
#define term_icedrv_sysfs() icedrv_sysfs_term()
#define init_icedrv_hw_config(dev) hw_config_sysfs_init(dev)
#define term_icedrv_hw_config(dev) hw_config_sysfs_term(dev)
#define init_ice_iccp(x) __init_ice_iccp(x)
#define term_ice_iccp(x) __term_ice_iccp(x)
#endif

int ice_di_get_core_blob_sz(void);

#if ICEDRV_ENABLE_HSLE_FLOW
#define __rdy_max_usleep (30000)
#define __rdy_min_usleep (10000)
#define __rdy_bit_max_trial (800)
#else
#define __rdy_max_usleep (3000)
#define __rdy_min_usleep (1000)
#define __rdy_bit_max_trial (8)
#endif /*ICEDRV_ENABLE_HSLE_FLOW*/

#define __wait_for_ice_rdy(dev, value, mask, offset) \
do {\
	int32_t count = __rdy_bit_max_trial;\
	while (count) {\
		value = cve_os_read_idc_mmio(dev, offset); \
		if ((value & mask) == mask)\
			break;\
		count--;\
		usleep_range(__rdy_min_usleep, __rdy_max_usleep);\
	} \
} while (0)

#define __select_val_for_correct_version(bstep_val, astep_val) \
	((ice_get_b_step_enable_flag()) ? bstep_val : astep_val)

#define __BSTEP_BAR1_ICE_REGION_SPILL_SZ 0x0
#define __ASTEP_BAR1_ICE_REGION_SPILL_SZ (0x4000) /*16K*/
#define __BAR1_ICE_REGION_SPILL_SZ \
	__select_val_for_correct_version(\
			__BSTEP_BAR1_ICE_REGION_SPILL_SZ,\
			__ASTEP_BAR1_ICE_REGION_SPILL_SZ)

#define __BSTEP_BAR1_IDC_ICE_ACCESS_WINDOW (0x20000) /*128K*/
#define __ASTEP_BAR1_IDC_ICE_ACCESS_WINDOW (0x10000) /*64K*/
#define __BAR1_IDC_ICE_ACCESS_WINDOW \
	__select_val_for_correct_version(\
			__BSTEP_BAR1_IDC_ICE_ACCESS_WINDOW,\
			__ASTEP_BAR1_IDC_ICE_ACCESS_WINDOW)

#define __BSTEP_BAR1_ICE_SZ (0x8000) /*32K*/
#define __ASTEP_BAR1_ICE_SZ (0x4000) /*16K*/
#define __BAR1_ICE_SZ \
	__select_val_for_correct_version(\
			__BSTEP_BAR1_ICE_SZ, __ASTEP_BAR1_ICE_SZ)


#define IDC_BAR1_COUNTERS_ADDRESS_START 0xFFFF0000
#define IDC_BAR1_COUNTERS_NOTI_ADDR 0xFFFF0800
#define BAR1_ICE_SPACE __BAR1_ICE_SZ
#define BAR1_ICE_PERMISSION 3 /*rw-*/

#define ICE_OFFSET(i) (0x100000 + (i * 0x40000)) /* 1 MB + 0.25MB per ICE */
#define ICE_BAR1_OFFSET(ice_id) (ice_id * BAR1_ICE_SPACE)
#define IDC_ICE_ACCESS_WINDOW_OFFSET __BAR1_IDC_ICE_ACCESS_WINDOW

#endif /* _DEVICE_INTERFACE_INTERNAL_H_ */
