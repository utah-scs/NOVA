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

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "dma_common.h"

DOCA_LOG_REGISTER(DMA_COMMON);

doca_error_t
open_doca_device_with_pci(const char *pci_addr, jobs_check func, struct doca_dev **retval)
{
  struct doca_devinfo **dev_list;
  uint32_t nb_devs;
  char buf[DOCA_DEVINFO_PCI_ADDR_SIZE];
  doca_error_t res;
  size_t i;

  *retval = NULL;

  res = doca_devinfo_create_list(&dev_list, &nb_devs);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Failed to load doca devices list. Doca_error value: %d", res);
    return res;
  }

  /* DOCA 3.3 returns addresses as "Domain:Bus:Device.Function" (e.g., "0000:03:00.0").
   * Accept both full format and short format without domain (e.g., "03:00.0"). */
  size_t pci_addr_len = strlen(pci_addr);

  for (i = 0; i < nb_devs; i++) {
    res = doca_devinfo_get_pci_addr_str(dev_list[i], buf);
    if (res != DOCA_SUCCESS)
      continue;
    size_t buf_len = strlen(buf);
    bool match = (buf_len >= pci_addr_len) &&
                 (strncasecmp(buf + buf_len - pci_addr_len, pci_addr, pci_addr_len) == 0);
    if (match) {
      if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
        continue;

      res = doca_dev_open(dev_list[i], retval);
      if (res == DOCA_SUCCESS) {
        doca_devinfo_destroy_list(dev_list);
        return res;
      }
    }
  }

  DOCA_LOG_ERR("Matching device not found.");
  res = DOCA_ERROR_NOT_FOUND;

  doca_devinfo_destroy_list(dev_list);
  return res;
}


doca_error_t
dpu_init_core_objects(struct dma_state *state, uint32_t max_buf_chunks)
{
  doca_error_t res;

  res = doca_mmap_create(&state->mmap);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to create mmap: %s", doca_error_get_descr(res));
    return res;
  }

  res = doca_mmap_add_dev(state->mmap, state->dev);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to add device to mmap: %s", doca_error_get_descr(res));
    doca_mmap_destroy(state->mmap);
    state->mmap = NULL;
    return res;
  }

  res = doca_buf_inventory_create(max_buf_chunks, &state->buf_inv);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to create buffer inventory: %s", doca_error_get_descr(res));
    return res;
  }

  res = doca_buf_inventory_start(state->buf_inv);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to start buffer inventory: %s", doca_error_get_descr(res));
    return res;
  }

  res = doca_pe_create(&state->pe);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(res));
    return res;
  }

  return DOCA_SUCCESS;
}

doca_error_t
host_init_core_objects(struct dma_state *state)
{
  doca_error_t res;

  res = doca_mmap_create(&state->mmap);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to create mmap: %s", doca_error_get_descr(res));
    return res;
  }

  res = doca_mmap_add_dev(state->mmap, state->dev);
  if (res != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to add device to mmap: %s", doca_error_get_descr(res));
    doca_mmap_destroy(state->mmap);
    state->mmap = NULL;
    return res;
  }

  return DOCA_SUCCESS;
}

doca_error_t
destroy_core_objects(struct dma_state *state)
{
  doca_error_t tmp_result, result = DOCA_SUCCESS;

  if (state->pe != NULL) {
    tmp_result = doca_pe_destroy(state->pe);
    if (tmp_result != DOCA_SUCCESS) {
      DOCA_ERROR_PROPAGATE(result, tmp_result);
      DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(tmp_result));
    }
    state->pe = NULL;
  }

  if (state->buf_inv != NULL) {
    tmp_result = doca_buf_inventory_destroy(state->buf_inv);
    if (tmp_result != DOCA_SUCCESS) {
      DOCA_ERROR_PROPAGATE(result, tmp_result);
      DOCA_LOG_ERR("Failed to destroy buf inventory: %s", doca_error_get_descr(tmp_result));
    }
    state->buf_inv = NULL;
  }

  if (state->mmap != NULL) {
    tmp_result = doca_mmap_destroy(state->mmap);
    if (tmp_result != DOCA_SUCCESS) {
      DOCA_ERROR_PROPAGATE(result, tmp_result);
      DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(tmp_result));
    }
    state->mmap = NULL;
  }

  if (state->dev != NULL) {
    tmp_result = doca_dev_close(state->dev);
    if (tmp_result != DOCA_SUCCESS) {
      DOCA_ERROR_PROPAGATE(result, tmp_result);
      DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(tmp_result));
    }
    state->dev = NULL;
  }

  return result;
}

void
host_destroy_core_objects(struct dma_state *state)
{
  doca_error_t res;

  if (state->mmap != NULL) {
    res = doca_mmap_destroy(state->mmap);
    if (res != DOCA_SUCCESS)
      DOCA_LOG_ERR("Failed to destroy mmap: %s", doca_error_get_descr(res));
    state->mmap = NULL;
  }

  if (state->dev != NULL) {
    res = doca_dev_close(state->dev);
    if (res != DOCA_SUCCESS)
      DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(res));
    state->dev = NULL;
  }
}


void
dma_cleanup(struct dma_state *state, struct doca_dma *dma_ctx)
{
  doca_error_t res;

  if (state->ctx != NULL) {
    doca_ctx_stop(state->ctx);
    state->ctx = NULL;
  }

  if (dma_ctx != NULL) {
    res = doca_dma_destroy(dma_ctx);
    if (res != DOCA_SUCCESS)
      DOCA_LOG_ERR("Failed to destroy dma: %s", doca_error_get_descr(res));
  }

  destroy_core_objects(state);
}

doca_error_t
dma_jobs_is_supported(struct doca_devinfo *devinfo)
{
  return doca_dma_cap_task_memcpy_is_supported(devinfo);
}
