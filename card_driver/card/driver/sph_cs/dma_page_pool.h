/********************************************
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 ********************************************/

#ifndef _SPHDRV_DMA_PAGE_POOL_H
#define _SPHDRV_DMA_PAGE_POOL_H

#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include "ipc_protocol.h"

struct dma_page_pool;
typedef struct dma_page_pool *pool_handle; /**< handle to a pool */ /* SPH_IGNORE_STYLE_CHECK */

typedef uint8_t page_handle; /**< handle to page from a pool */

/**
 * Call back function to call to send list of free responce pages to device
 */
typedef int (*send_free_pages_cb)(void *ctx, dma_addr_t list, unsigned int list_size);

struct dma_pool_stat {		 /**< pool statistics */
	unsigned int free_pages; /**< number of free pages in a pool */
	unsigned int sent_pages; /**< number of pages sent to device */
	unsigned int full_pages; /**< number of pages filled */

	/**< number of unused pages, since last deallocation */
	unsigned int unused_page_count;
	unsigned int cb_failures; /**< number of times callback function failed */
};

/**
 * @brief Create dma pages pool
 *
 * This function provides pool handle. If this function fails, it frees
 * all already allocated resouses and exits with error. So inconsistent
 * state is eliminated. In case of failure page_pool_destroy() function
 * should not be called.
 *
 * @param[in]   dev       SpringHill device
 * @param[in]   max_size  Maximum number of pages, pool should handle
 * @param[out]  pool      Handle to newly created pool
 * @return error number on failure.
 */
int dma_page_pool_create(struct device *dev, unsigned int max_size, pool_handle *pool);

/**
 * @brief Create debugfs entries for the pool
 */
void dma_page_pool_init_debugfs(pool_handle    pool,
				struct dentry *parent,
				const char    *dirname);

/**
 * @brief Setup response dma pages pool
 *
 * If you don't call this function, the pool will not act as response pool.
 * This function configure the pool to be response pool as well.
 * If this function fails, you must not use the pool as response pool.
 * If this function is called another time, callback function will be updated
 * If cb parameter is NULL, the pool will stop act as response pool.
 * The callback to send free pages will be called initially during the execution of this
 * function call, it will be called again at a later time when the amount of free host response
 * pages goes below some threshold, in this case the callback will be called from the context
 * of the specified workqueue.
 *
 * @param[in]  pool  handle to the pool
 * @param[in]  cb    callback function to use to send free response pages list
 * @param[in]  ctx   context dependent pointer
 * @param[in]  send_wq qorkqueue used for scheduling the send callback.
 * @return error number on failure.
 */
int dma_page_pool_response_setup(pool_handle pool,
				 send_free_pages_cb cb,
				 void *ctx,
				 struct workqueue_struct *send_wq);

/*
 * @brief Returns all response pages back to the free pool
 *
 * This function should be called after a card reset.
 */
void dma_page_pool_reset_response_pages(pool_handle pool);

/**
 * @brief Destroys the pool previously created
 *
 * This function releases all the resourses allocated for the pool
 *
 * @param[in]  pool  handle to the pool
 * @return nothing.
 */
void dma_page_pool_destroy(pool_handle pool);

/**
 * @brief Allocates free page to send data
 *
 * This function allocates a free page from a pool to send data to device
 * and returns pointer to fill the page with the data.
 * The page is marked as full.
 * If the pool has no pre-allocated free page, the function will fail
 * and returns -EXFULL
 *
 * @param[in]   pool      handle to the pool
 * @param[out]  page      handle to the free page
 * @param[out]  ptr       pointer to fill the data in the page
 * @param[out]  dma_addr  DMA address of the page for device
 * @return error on failure.
 */
int dma_page_pool_get_free_page_nowait(pool_handle pool, page_handle *page, void **ptr, dma_addr_t *dma_addr);

/**
 * @brief Allocates free page to send data
 *
 * This function allocates a free page from a pool to send data to device
 * and returns pointer to fill the page with the data.
 * The page is marked as full.
 * If the pool is fully loaded and a free page is not available, the function will wait
 * until a free page becomes available.
 *
 * @param[in]   pool      handle to the pool
 * @param[out]  page      handle to the free page
 * @param[out]  ptr       pointer to fill the data in the page
 * @param[out]  dma_addr  DMA address of the page for device
 * @return error on failure.
 */
int dma_page_pool_get_free_page(pool_handle pool, page_handle *page, void **ptr, dma_addr_t *dma_addr);

/*
 * @breif Calling poll_wait with the internal waitq object for waiting for a free page
 */
void dma_page_pool_free_page_poll_wait(pool_handle pool,
				       struct file *f,
				       struct poll_table_struct *pt);

/**
 * @brief Mark the response page as full and returns handle to the page
 *
 * Call this function, when filled response page arrived from device.
 *
 * @param[in]  pool  handle to the pool
 * @param[in]  page  handle to the page, arrived from device
 * @return error on failure.
 */
int dma_page_pool_set_response_page_full(pool_handle pool, page_handle page);

/**
 * @brief Sends pointer to the data, which arrived from the device
 *
 * This function returns pointer to access the data received from device
 *
 * @param[in]   pool  handle to the pool
 * @param[in]   page  handle to the page, to read the data from
 * @param[out]  p     pointer to read only data stored in the page
 * @return error on failure.
 */
int dma_page_pool_get_page_pointer(pool_handle pool, page_handle page, const void **p);

/**
 * @brief Sends dma address of a dma page
 *
 * This function returns the dma address of a dma page handle
 *
 * @param[in]   pool  handle to the pool
 * @param[in]   page  handle to the page
 * @param[out]  addr  pointer to store the returned dma address
 * @return error on failure.
 */
int dma_page_pool_get_page_addr(pool_handle pool, page_handle page, dma_addr_t *addr);

/**
 * @brief Mark the page as free to be reused
 *
 * This function should be called when all the needed data
 * was read and the page is free to be reused.
 * After calling this function all the data in the page can be overwritten,
 * or even the page memory can be freed.
 * And there will no be any way to access it or restore it.
 *
 * @param[in]  pool  handle to the pool
 * @param[in]  page  handle to the page to free
 * @return error on failure.
 */
int dma_page_pool_set_page_free(pool_handle pool, page_handle page);

/**
 * @brief Get pool status counters
 *
 * This function fills the stat with relevant counters. So statistic
 * report for futher analysis can be done.
 *
 * @param[in]   pool  handle to the pool
 * @param[out]  stat  struct with relevant counters
 * @return error on failure.
 */
int dma_page_pool_get_stats(pool_handle pool, struct dma_pool_stat *stat);

/**
 * @brief Get pool status counters
 *
 * This function frees all the pages, which has beed unused
 * since last call to it.
 *
 * @param[in]  pool  handle to the pool
 * @return nothing.
 */
void dma_page_pool_deallocate_unused_pages(pool_handle pool);

#ifdef ULT
/**
 * @brief Get response list kernel pointer for ULT only
 *
 * This function is for ULT only! It returns the kernel pointer to the page
 * with free pages for response, sent to device. Call it from ULT callback
 * function to access the content of the page.
 *
 * @param[in]   pool  handle to the pool
 * @param[out]  list  kernel pointer to response list
 * @return error on failure.
 */
int dma_page_pool_get_resp_list_pointer(pool_handle pool, const struct response_list_entry **list);
#endif

#endif
