/*
 * Copyright 2014, 2015 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include "util/u_surface.h"
#include "util/u_memory.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/os_time.h"
#include "frontend/sw_winsys.h"
#include "frontend/xlibsw_api.h"
#include "util/os_mman.h"

#include "virgl_server_winsys.h"
#include "virgl_server_public.h"

/* Gets a pointer to the virgl_hw_res containing the pointed to cache entry. */
#define cache_entry_container_res(ptr) \
    (struct virgl_hw_res*)((char*)ptr - offsetof(struct virgl_hw_res, cache_entry))

static void *virgl_server_resource_map(struct virgl_winsys *vws,
                                       struct virgl_hw_res *res);
static void virgl_server_resource_unmap(struct virgl_winsys *vws,
                                        struct virgl_hw_res *res);
									   
struct virgl_displaytarget {
   struct sw_displaytarget *sw_dt;
   bool no_readback;
   Drawable drawable;
};										   

static struct virgl_displaytarget *
virgl_displaytarget_create(struct virgl_server_winsys *vsws,
                           uint32_t bind,
                           enum pipe_format format,
                           uint32_t width, uint32_t height,
                           uint32_t alignment,
                           const void *map_front_private,
                           uint32_t *stride)
{
   struct virgl_displaytarget *dt = CALLOC_STRUCT(virgl_displaytarget);
   
   const char *no_readback_value = getenv("VIRGL_NO_READBACK");
   
   dt->no_readback = !no_readback_value || !strcmp(no_readback_value, "true") || !strcmp(no_readback_value, "1");
   dt->sw_dt = vsws->sws->displaytarget_create(vsws->sws, bind, format,
                                               width, height, alignment, map_front_private,
                                               stride);

   return dt;
}

static void virgl_displaytarget_destroy(struct virgl_server_winsys *vsws, 
                                        struct virgl_displaytarget *dt)
{
   vsws->sws->displaytarget_destroy(vsws->sws, dt->sw_dt);
   FREE(dt);
}

static inline boolean can_cache_resource_with_bind(uint32_t bind)
{
   return bind == VIRGL_BIND_CONSTANT_BUFFER ||
          bind == VIRGL_BIND_INDEX_BUFFER ||
          bind == VIRGL_BIND_VERTEX_BUFFER ||
          bind == VIRGL_BIND_CUSTOM ||
          bind == VIRGL_BIND_STAGING;
}

static uint32_t virgl_server_get_transfer_size(struct virgl_hw_res *res,
                                               const struct pipe_box *box,
                                               uint32_t stride, uint32_t layer_stride,
                                               uint32_t level, uint32_t *valid_stride_p)
{
   uint32_t valid_stride, valid_layer_stride;

   valid_stride = util_format_get_stride(res->format, box->width);
   if (stride) {
      if (box->height > 1)
         valid_stride = stride;
   }

   valid_layer_stride = util_format_get_2d_size(res->format, valid_stride,
                                                box->height);
   if (layer_stride) {
      if (box->depth > 1)
         valid_layer_stride = layer_stride;
   }

   *valid_stride_p = valid_stride;
   return valid_layer_stride * box->depth;
}

static int
virgl_server_transfer_put(struct virgl_winsys *vws,
                          struct virgl_hw_res *res,
                          const struct pipe_box *box,
                          uint32_t stride, uint32_t layer_stride,
                          uint32_t buf_offset, uint32_t level)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   uint32_t size;
   uint32_t valid_stride;

   size = virgl_server_get_transfer_size(res, box, stride, layer_stride, level,
                                         &valid_stride);

   virgl_server_send_transfer_put(vsws, res->res_handle,
                                  level, stride, layer_stride,
                                  box, size, buf_offset);
   return 0;
}

static int
virgl_server_transfer_get_internal(struct virgl_winsys *vws,
                                   struct virgl_hw_res *res,
                                   const struct pipe_box *box,
                                   uint32_t stride, uint32_t layer_stride,
                                   uint32_t buf_offset, uint32_t level,
                                   bool flush_front_buffer)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   uint32_t size;
   void *ptr;
   uint32_t valid_stride;

   size = virgl_server_get_transfer_size(res, box, stride, layer_stride, level,
                                         &valid_stride);
   virgl_server_send_transfer_get(vsws, res->res_handle,
                                  level, stride, layer_stride,
                                  box, size, buf_offset);

   virgl_server_send_resource_busy_wait(vsws, res->res_handle, VCMD_BUSY_WAIT_FLAG_WAIT);

   if (flush_front_buffer) {
      if (box->depth > 1 || box->z > 1)
         return -1;

      void *dt_map;
      uint32_t shm_stride;

      /*
       * The display target is aligned to 64 bytes, while the shared resource
       * between the client/server is not.
       */
      shm_stride = util_format_get_stride(res->format, res->width);
      ptr = virgl_server_resource_map(vws, res);
      dt_map = vsws->sws->displaytarget_map(vsws->sws, res->dt->sw_dt, 0);

      util_copy_rect(dt_map, res->format, res->stride, box->x, box->y,
                     box->width, box->height, ptr, shm_stride, box->x,
                     box->y);

      virgl_server_resource_unmap(vws, res);
      vsws->sws->displaytarget_unmap(vsws->sws, res->dt->sw_dt);
   }

   return 0;
}

static int
virgl_server_transfer_get(struct virgl_winsys *vws,
                          struct virgl_hw_res *res,
                          const struct pipe_box *box,
                          uint32_t stride, uint32_t layer_stride,
                          uint32_t buf_offset, uint32_t level)
{
   return virgl_server_transfer_get_internal(vws, res, box, stride,
                                             layer_stride, buf_offset,
                                             level, false);
}

static void virgl_hw_res_destroy(struct virgl_server_winsys *vsws,
                                 struct virgl_hw_res *res)
{
   virgl_server_send_resource_destroy(vsws, res->res_handle);
   if (res->dt)
      virgl_displaytarget_destroy(vsws, res->dt);
   
   if (res->ptr)
      os_munmap(res->ptr, res->size);

   FREE(res);
}

static boolean virgl_server_resource_is_busy(struct virgl_winsys *vws,
                                             struct virgl_hw_res *res)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);

   int ret;
   ret = virgl_server_send_resource_busy_wait(vsws, res->res_handle, 0);

   if (ret < 0)
      return false;

   return ret == 1 ? true : false;
}

static void virgl_server_resource_reference(struct virgl_winsys *vws,
                                            struct virgl_hw_res **dres,
                                            struct virgl_hw_res *sres)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(&(*dres)->reference, &sres->reference)) {
      if (!can_cache_resource_with_bind(old->bind)) {
         virgl_hw_res_destroy(vsws, old);
      } else {
         mtx_lock(&vsws->mutex);
         virgl_resource_cache_add(&vsws->cache, &old->cache_entry);
         mtx_unlock(&vsws->mutex);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
virgl_server_winsys_resource_create(struct virgl_winsys *vws,
                                    enum pipe_texture_target target,
									const void *map_front_private,
                                    uint32_t format,
                                    uint32_t bind,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t depth,
                                    uint32_t array_size,
                                    uint32_t last_level,
                                    uint32_t nr_samples,
                                    uint32_t size)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   struct virgl_hw_res *res;
   static int handle = 1;
   int fd = -1;
   struct virgl_resource_params params = { .size = size,
                                           .bind = bind,
                                           .format = format,
                                           .flags = 0,
                                           .nr_samples = nr_samples,
                                           .width = width,
                                           .height = height,
                                           .depth = depth,
                                           .array_size = array_size,
                                           .last_level = last_level,
                                           .target = target };   

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;

   if (bind & (VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT)) {
      res->dt = virgl_displaytarget_create(vsws, bind, format,
                                           width, height, 64, map_front_private,
                                           &res->stride);

   }

   res->bind = bind;
   res->format = format;
   res->height = height;
   res->width = width;
   res->size = size;
   virgl_server_send_resource_create(vsws, handle, target, pipe_to_virgl_format(format), bind,
                                     width, height, depth, array_size,
                                     last_level, nr_samples, size, &fd);

   if (res->size == 0) {
      res->ptr = NULL;
      goto out;
   }

   if (fd < 0) {
      FREE(res);
      return NULL;
   }

   res->ptr = os_mmap(NULL, res->size, PROT_WRITE | PROT_READ, MAP_SHARED,
                      fd, 0);

   if (res->ptr == MAP_FAILED) {
      close(fd);
      FREE(res);
      return NULL;
   }

   close(fd);

out:
   virgl_resource_cache_entry_init(&res->cache_entry, params);
   res->res_handle = handle++;
   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->num_cs_references, 0);
   return res;
}

static void *virgl_server_resource_map(struct virgl_winsys *vws,
                                       struct virgl_hw_res *res)
{ 
   res->mapped = res->ptr;
   return res->mapped;
}

static void virgl_server_resource_unmap(struct virgl_winsys *vws,
                                        struct virgl_hw_res *res)
{
   if (res->mapped)
      res->mapped = NULL;
}

static void virgl_server_resource_wait(struct virgl_winsys *vws,
                                       struct virgl_hw_res *res)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);

   virgl_server_send_resource_busy_wait(vsws, res->res_handle, VCMD_BUSY_WAIT_FLAG_WAIT);
}

static struct virgl_hw_res *
virgl_server_winsys_resource_cache_create(struct virgl_winsys *vws,
                                          enum pipe_texture_target target,
										  const void *map_front_private,
                                          uint32_t format,
                                          uint32_t bind,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t depth,
                                          uint32_t array_size,
                                          uint32_t last_level,
                                          uint32_t nr_samples,
										  uint32_t flags,
                                          uint32_t size)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   struct virgl_hw_res *res;
   struct virgl_resource_cache_entry *entry;
   struct virgl_resource_params params = { .size = size,
                                           .bind = bind,
                                           .format = format,
                                           .flags = 0,
                                           .nr_samples = nr_samples,
                                           .width = width,
                                           .height = height,
                                           .depth = depth,
                                           .array_size = array_size,
                                           .last_level = last_level,
                                           .target = target };   

   if (!can_cache_resource_with_bind(bind))
      goto alloc;

   mtx_lock(&vsws->mutex);

   entry = virgl_resource_cache_remove_compatible(&vsws->cache, params);
   if (entry) {
      res = cache_entry_container_res(entry);
      mtx_unlock(&vsws->mutex);
      pipe_reference_init(&res->reference, 1);
      return res;
   }

   mtx_unlock(&vsws->mutex);

alloc:
   res = virgl_server_winsys_resource_create(vws, target, map_front_private,
                                             format, bind, width, height, depth, 
											 array_size, last_level, nr_samples, 
											 size);
   return res;
}

static boolean virgl_server_lookup_res(struct virgl_server_cmd_buf *cbuf,
                                       struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_handle_added)-1);
   int i;

   if (cbuf->is_handle_added[hash]) {
      i = cbuf->reloc_indices_hashlist[hash];
      if (cbuf->res_bo[i] == res)
         return true;

      for (i = 0; i < cbuf->cres; i++) {
         if (cbuf->res_bo[i] == res) {
            cbuf->reloc_indices_hashlist[hash] = i;
            return true;
         }
      }
   }
   return false;
}

static void virgl_server_release_all_res(struct virgl_server_winsys *vsws,
                                         struct virgl_server_cmd_buf *cbuf)
{
   int i;

   for (i = 0; i < cbuf->cres; i++) {
      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      virgl_server_resource_reference(&vsws->base, &cbuf->res_bo[i], NULL);
   }
   cbuf->cres = 0;
}

static void virgl_server_add_res(struct virgl_server_winsys *vsws,
                                 struct virgl_server_cmd_buf *cbuf,
                                 struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_handle_added)-1);

   if (cbuf->cres >= cbuf->nres) {
      unsigned new_nres = cbuf->nres + 256;
      struct virgl_hw_res **new_re_bo = REALLOC(cbuf->res_bo,
                                                cbuf->nres * sizeof(struct virgl_hw_buf*),
                                                new_nres * sizeof(struct virgl_hw_buf*));
      if (!new_re_bo)
         return;

      cbuf->res_bo = new_re_bo;
      cbuf->nres = new_nres;
   }

   cbuf->res_bo[cbuf->cres] = NULL;
   virgl_server_resource_reference(&vsws->base, &cbuf->res_bo[cbuf->cres], res);
   cbuf->is_handle_added[hash] = true;

   cbuf->reloc_indices_hashlist[hash] = cbuf->cres;
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static struct virgl_cmd_buf *virgl_server_cmd_buf_create(struct virgl_winsys *vws,
                                                         uint32_t size)
{
   struct virgl_server_cmd_buf *cbuf;

   cbuf = CALLOC_STRUCT(virgl_server_cmd_buf);
   if (!cbuf)
      return NULL;

   cbuf->nres = 512;
   cbuf->res_bo = CALLOC(cbuf->nres, sizeof(struct virgl_hw_buf*));
   if (!cbuf->res_bo) {
      FREE(cbuf);
      return NULL;
   }

   cbuf->buf = CALLOC(size, sizeof(uint32_t));
   if (!cbuf->buf) {
      FREE(cbuf->res_bo);
      FREE(cbuf);
      return NULL;
   }

   cbuf->ws = vws;
   cbuf->base.buf = cbuf->buf;
   return &cbuf->base;
}

static void virgl_server_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_server_cmd_buf *cbuf = virgl_server_cmd_buf(_cbuf);

   virgl_server_release_all_res(virgl_server_winsys(cbuf->ws), cbuf);
   FREE(cbuf->res_bo);
   FREE(cbuf->buf);
   FREE(cbuf);
}

static struct pipe_fence_handle *
virgl_server_fence_create(struct virgl_winsys *vws)
{
   struct virgl_hw_res *res;

   /* Resources for fences should not be from the cache, since we are basing
    * the fence status on the resource creation busy status.
    */
   res = virgl_server_winsys_resource_create(vws,
                                             PIPE_BUFFER,
											 NULL,
                                             PIPE_FORMAT_R8_UNORM,
                                             VIRGL_BIND_CUSTOM,
                                             8, 1, 1, 0, 0, 0, 8);

   return (struct pipe_fence_handle *)res;
}

static int virgl_server_winsys_submit_cmd(struct virgl_winsys *vws,
                                          struct virgl_cmd_buf *_cbuf,
                                          struct pipe_fence_handle **fence)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   struct virgl_server_cmd_buf *cbuf = virgl_server_cmd_buf(_cbuf);
   int ret;

   if (cbuf->base.cdw == 0)
      return 0;

   ret = virgl_server_submit_cmd(vsws, cbuf);
   if (fence && ret == 0)
      *fence = virgl_server_fence_create(vws);

   virgl_server_release_all_res(vsws, cbuf);
   memset(cbuf->is_handle_added, 0, sizeof(cbuf->is_handle_added));
   cbuf->base.cdw = 0;
   return ret;
}

static void virgl_server_emit_res(struct virgl_winsys *vws,
                                  struct virgl_cmd_buf *_cbuf,
                                  struct virgl_hw_res *res, boolean write_buf)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   struct virgl_server_cmd_buf *cbuf = virgl_server_cmd_buf(_cbuf);
   boolean already_in_list = virgl_server_lookup_res(cbuf, res);

   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!already_in_list)
      virgl_server_add_res(vsws, cbuf, res);
}

static boolean virgl_server_res_is_ref(struct virgl_winsys *vws,
                                      struct virgl_cmd_buf *_cbuf,
                                      struct virgl_hw_res *res)
{
   if (!p_atomic_read(&res->num_cs_references))
      return false;

   return true;
}

static int virgl_server_get_caps(struct virgl_winsys *vws,
                                 struct virgl_drm_caps *caps)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);

   virgl_ws_fill_new_caps_defaults(caps);
   return virgl_server_send_get_caps(vsws, caps);
}

static struct pipe_fence_handle *
virgl_cs_create_fence(struct virgl_winsys *vws, int fd)
{
   return virgl_server_fence_create(vws);
}

static bool virgl_fence_wait(struct virgl_winsys *vws,
                             struct pipe_fence_handle *fence,
                             uint64_t timeout)
{
   struct virgl_hw_res *res = virgl_hw_res(fence);

   if (timeout == 0)
      return !virgl_server_resource_is_busy(vws, res);

   if (timeout != OS_TIMEOUT_INFINITE) {
      int64_t start_time = os_time_get();
      timeout /= 1000;
      while (virgl_server_resource_is_busy(vws, res)) {
         if (os_time_get() - start_time >= timeout)
            return false;
         os_time_sleep(10);
      }
      return true;
   }
   virgl_server_resource_wait(vws, res);
   return true;
}

static void virgl_fence_reference(struct virgl_winsys *vws,
                                  struct pipe_fence_handle **dst,
                                  struct pipe_fence_handle *src)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   virgl_server_resource_reference(&vsws->base, (struct virgl_hw_res **)dst,
                                   virgl_hw_res(src));
}

static void virgl_server_flush_frontbuffer(struct virgl_winsys *vws,
                                           struct virgl_hw_res *res,
                                           unsigned level, unsigned layer,
                                           void *winsys_drawable_handle,
                                           struct pipe_box *sub_box)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);
   struct pipe_box box;
   uint32_t offset = 0;
   if (!res->dt)
      return;
   
   if (res->dt->no_readback) {
      if (!res->dt->drawable) {
         struct xlib_drawable *xlib_drawable = (struct xlib_drawable*)winsys_drawable_handle;
		 res->dt->drawable = xlib_drawable->drawable;
	  }
	  
      virgl_server_send_flush_frontbuffer(vsws, res->res_handle, (uint32_t)res->dt->drawable);
	  virgl_server_send_resource_busy_wait(vsws, res->res_handle, VCMD_BUSY_WAIT_FLAG_WAIT);
      return;
   }
   
   memset(&box, 0, sizeof(box));

   if (sub_box) {
      box = *sub_box;
      offset = box.y / util_format_get_blockheight(res->format) * res->stride +
               box.x / util_format_get_blockwidth(res->format) * util_format_get_blocksize(res->format);
   } else {
      box.z = layer;
      box.width = res->width;
      box.height = res->height;
      box.depth = 1;
   }

   virgl_server_transfer_get_internal(vws, res, &box, res->stride, 0, offset,
                                      level, true);

   vsws->sws->displaytarget_display(vsws->sws, res->dt->sw_dt, winsys_drawable_handle, sub_box);
}

static void
virgl_server_winsys_destroy(struct virgl_winsys *vws)
{
   struct virgl_server_winsys *vsws = virgl_server_winsys(vws);

   virgl_resource_cache_flush(&vsws->cache);

   mtx_destroy(&vsws->mutex);
   FREE(vsws);
}

static bool
virgl_server_resource_cache_entry_is_busy(struct virgl_resource_cache_entry *entry,
                                          void *user_data)
{
   struct virgl_server_winsys *vsws = user_data;
   struct virgl_hw_res *res = cache_entry_container_res(entry);

   return virgl_server_resource_is_busy(&vsws->base, res);
}

static void
virgl_server_resource_cache_entry_release(struct virgl_resource_cache_entry *entry,
                                          void *user_data)
{
   struct virgl_server_winsys *vsws = user_data;
   struct virgl_hw_res *res = cache_entry_container_res(entry);

   virgl_hw_res_destroy(vsws, res);
}

struct virgl_winsys *
virgl_server_winsys_wrap(struct sw_winsys *sws)
{
   static const unsigned CACHE_TIMEOUT_USEC = 1000000;
   struct virgl_server_winsys *vsws;

   vsws = CALLOC_STRUCT(virgl_server_winsys);
   if (!vsws)
      return NULL;

   virgl_server_connect(vsws);
   vsws->sws = sws;

   virgl_resource_cache_init(&vsws->cache, CACHE_TIMEOUT_USEC,
                             virgl_server_resource_cache_entry_is_busy,
                             virgl_server_resource_cache_entry_release,
                             vsws);
   (void) mtx_init(&vsws->mutex, mtx_plain);

   vsws->base.destroy = virgl_server_winsys_destroy;

   vsws->base.transfer_put = virgl_server_transfer_put;
   vsws->base.transfer_get = virgl_server_transfer_get;

   vsws->base.resource_create = virgl_server_winsys_resource_cache_create;
   vsws->base.resource_reference = virgl_server_resource_reference;
   vsws->base.resource_map = virgl_server_resource_map;
   vsws->base.resource_wait = virgl_server_resource_wait;
   vsws->base.resource_is_busy = virgl_server_resource_is_busy;
   vsws->base.cmd_buf_create = virgl_server_cmd_buf_create;
   vsws->base.cmd_buf_destroy = virgl_server_cmd_buf_destroy;
   vsws->base.submit_cmd = virgl_server_winsys_submit_cmd;

   vsws->base.emit_res = virgl_server_emit_res;
   vsws->base.res_is_referenced = virgl_server_res_is_ref;
   vsws->base.get_caps = virgl_server_get_caps;

   vsws->base.cs_create_fence = virgl_cs_create_fence;
   vsws->base.fence_wait = virgl_fence_wait;
   vsws->base.fence_reference = virgl_fence_reference;
   vsws->base.supports_fences = 0;
   vsws->base.supports_encoded_transfers = 1;

   vsws->base.flush_frontbuffer = virgl_server_flush_frontbuffer;

   return &vsws->base;
}
