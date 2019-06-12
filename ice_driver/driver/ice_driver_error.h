/*
 * NNP-I Linux Driver
 * Copyright (c) 2017-2019, Intel Corporation.
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


#ifndef _ICE_DRIVER_ERROR_H_
#define _ICE_DRIVER_ERROR_H_


/** Proprietary error code base value for ice kernel driver */
#define ICEDRV_BASE_KERROR_CODE 1100

/**
 * @enum ice_kerror_type
 * @brief exhaustive proprietary error codes for ice kernel ice driver
 *
 * it lists non-conflicting error codes to be used within the KMD to expedite
 * debug. Error codes does not conflict with standard Linux error codes and
 * user space library. Each IOCTL will either throw a standard Linux error like
 * -ENOMEM for allocation failure or the driver proprietary error.
 *  Proprietary error is thrown when error occurs due to violation of driver
 *  based rules.
 */
enum ice_kerror_type {
	/** Handle used to access an IOCTL is invalid */
	ICEDRV_KERROR_INVALID_DRV_HANDLE = ICEDRV_BASE_KERROR_CODE,
	/** Context handle is invalid */
	ICEDRV_KERROR_CTX_INVAL_ID, /* [1101] */
	/** Context or its dependent resource are in use, cannot be deleted */
	ICEDRV_KERROR_CTX_BUSY, /* [1102] */
	/** Context or dependent resources don't exist anymore */
	ICEDRV_KERROR_CTX_NODEV, /* [1103] */
	/** Driver is out of space to create a new page table entry */
	ICEDRV_KERROR_IOVA_NOMEM, /* [1104] */
	/** surface memory to be IO mapped to ICE violates alignment rules */
	ICEDRV_KERROR_IOVA_PAGE_ALIGNMENT, /* [1105] */
	/** Invalid number of pages to be mapped */
	ICEDRV_KERROR_IOVA_INVALID_PAGE_COUNT, /* [1106] */
	/** surface memory violates ICE cache alignment rules */
	ICEDRV_KERROR_SURF_DEV_CACHE_ALIGNMENT, /* [1107] */
	/** surface memory referenced for an operation is invalid */
	ICEDRV_KERROR_CB_INVAL_BUFFER_ID, /* [1108] */
	/** Page table programming resulted in duplicate entry */
	ICEDRV_KERROR_PT_DUPLICATE_ENTRY, /* [1109] */
	/** LLC policy request violates page table's LLC rules */
	ICEDRV_KERROR_PT_INVAL_LLC_POLICY, /* [1110] */
	/** Requested FW not found */
	ICEDRV_KERROR_FW_NOENT, /* [1111] */
	/** Embedded CB generated is invalid */
	ICEDRV_KERROR_FW_INVAL_ECB, /* [1112] */
	/** Embedded CB's vmap operation failed */
	ICEDRV_KERROR_FW_ECB_MAPPING, /* [1113] */
	/** Requested FW is not allowed to be mapped */
	ICEDRV_KERROR_FW_PERM, /* [1114] */
	/** Requested FW type does not match any of the supported types */
	ICEDRV_KERROR_FW_INVAL_TYPE, /* [1115] */
	/** Resource request violates actual HW capabilities */
	ICEDRV_KERROR_NTW_INVAL_RESOURCE_REQ, /* [1116] */
	/** Network ID/Handle  is invalid */
	ICEDRV_KERROR_NTW_INVAL_ID, /* [1117] */
	/** Requested Network does not exist */
	ICEDRV_KERROR_NTW_NODEV, /* [1118] */
	/** Network already scheduled to ICE */
	ICEDRV_KERROR_NTW_EALREADY, /* [1119] */
	/** Job group dependencies within the network may cause dead lock */
	ICEDRV_KERROR_NTW_DEADLK, /* [1120] */
	/** Requested operation on the network is not allowed */
	ICEDRV_KERROR_NTW_PERM, /* [1121] */
	/** Relevant counter information not available within the network */
	ICEDRV_KERROR_NTW_CNTR_NXIO, /* [1122] */
	/** Requested ICE is more than available ICE */
	ICEDRV_KERROR_NTW_ICE_MAX, /* [1123] */
	/** ICE Power UP sequence failed */
	ICEDRV_KERROR_ICE_DOWN, /* [1124] */
	/** no free pool available */
	ICEDRV_KERROR_ICEDC_POOL_BUSY, /* [1125] */
	/** Device reset required */
	ICEDRV_KERROR_CARD_RESET_NEEDED, /* [1126] */
	/** Invalid Patch Point Type */
	ICEDRV_KERROR_PP_TYPE_EINVAL, /* [1127] */
	/** Invalid Patch Point Count */
	ICEDRV_KERROR_PP_COUNT_EINVAL, /* [1128] */
	/** Hardware resource (ICE/COUNTER) not available */
	ICEDRV_KERROR_RESOURCE_BUSY, /* [1129] */
	/** Invalid max shared distance argument */
	ICEDRV_KERROR_INVAL_MAX_SHARED_DISTANCE, /* [1130] */
	/** Inference already scheduled to ICE */
	ICEDRV_KERROR_INF_EALREADY, /* [1131] */
	/** Infer buffer index is invalid */
	ICEDRV_KERROR_INF_INDEX_INVAL_ID, /* [1132] */
	/** Inference ID/Handle  is invalid */
	ICEDRV_KERROR_INF_INVAL_ID /* [1133] */
};

#endif /* _ICE_DRIVER_ERROR_H_ */
