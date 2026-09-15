/*
 * Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#ifndef DMA_COMMON_H_
#define DMA_COMMON_H_

#include <doca_error.h>

#define BUFSZ 16
#define PAGE_SIZE 1024 * 4
#define WORKQ_DEPTH 32

struct dma_state {
  struct doca_dev *dev;
  struct doca_mmap *mmap;
  struct doca_buf_inventory *buf_inv;
  struct doca_ctx *ctx;
  struct doca_dma *dma_ctx;
  struct doca_pe *pe;
};

typedef doca_error_t (*jobs_check)(struct doca_devinfo *);

doca_error_t
open_doca_device_with_pci(const char *pci_addr, jobs_check func, struct doca_dev **retval);

doca_error_t
dpu_init_core_objects(struct dma_state *state, uint32_t max_buf_chunks);

doca_error_t
host_init_core_objects(struct dma_state *state);

doca_error_t
destroy_core_objects(struct dma_state *state);

void
host_destroy_core_objects(struct dma_state *state);

void
dma_cleanup(struct dma_state *state, struct doca_dma *dma_ctx);

doca_error_t
dma_jobs_is_supported(struct doca_devinfo *devinfo);

#endif
