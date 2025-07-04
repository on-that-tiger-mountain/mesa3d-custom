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

#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>

#include <util/u_process.h>
#include <util/format/u_format.h>

#include "virgl_server_winsys.h"
#include "virgl_server_public.h"

static int virgl_block_write(int fd, void *buf, int size)
{	
   char *ptr = buf;
   int left;
   int ret;
   left = size;

   do {
      ret = write(fd, ptr, left);
      if (ret < 0)
         return -errno;

      left -= ret;
      ptr += ret;
   } while (left);

   return size;
}

static int virgl_block_read(int fd, void *buf, int size)
{
   char *ptr = buf;
   int left;
   int ret;

   left = size;
   do {
      ret = read(fd, ptr, left);
      if (ret <= 0)
         return ret == -1 ? -errno : 0;

      left -= ret;
      ptr += ret;
   } while (left);

   return size;
}

static int virgl_server_recv_fd(int socket_fd)
{
   struct cmsghdr *cmsgh;
   struct msghdr msgh = { 0 };
   char buf[CMSG_SPACE(sizeof(int))], c;
   struct iovec iovec;

   iovec.iov_base = &c;
   iovec.iov_len = sizeof(char);

   msgh.msg_name = NULL;
   msgh.msg_namelen = 0;
   msgh.msg_iov = &iovec;
   msgh.msg_iovlen = 1;
   msgh.msg_control = buf;
   msgh.msg_controllen = sizeof(buf);
   msgh.msg_flags = 0;

   int size = recvmsg(socket_fd, &msgh, 0);
   if (size < 0)
      return -1;

   cmsgh = CMSG_FIRSTHDR(&msgh);
   if (!cmsgh)
      return -1;

   if (cmsgh->cmsg_level != SOL_SOCKET)
      return -1;

   if (cmsgh->cmsg_type != SCM_RIGHTS)
      return -1;

   return *((int *) CMSG_DATA(cmsgh));
}

static int virgl_server_send_create_renderer(struct virgl_server_winsys *vws)
{
   uint32_t send_buf[2];
   send_buf[0] = 0;
   send_buf[1] = VCMD_CREATE_RENDERER;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   return 0;
}

int virgl_server_connect(struct virgl_server_winsys *vws)
{
   struct sockaddr_un un;
   int fd, ret;

   fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
  
   const char *path = getenv("VIRGL_SERVER_PATH");
   if (!path) 
      path = VIRGL_DEFAULT_SERVER_PATH;  

   memset(&un, 0, sizeof(un));
   un.sun_family = AF_LOCAL;
   snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);

   do {
      ret = 0;
      if (connect(fd, (struct sockaddr *)&un, sizeof(un)) < 0)
         ret = -errno;
   } while (ret == -EINTR);

   vws->sock_fd = fd;
   
   virgl_server_send_create_renderer(vws);
   return 0;
}

int virgl_server_send_get_caps(struct virgl_server_winsys *vws,
                               struct virgl_drm_caps *caps)
{
   uint32_t send_buf[2];
   uint32_t resp_buf[2];
   uint32_t caps_size = sizeof(struct virgl_caps_v2);
   int ret;
   send_buf[0] = 0;
   send_buf[1] = VCMD_GET_CAPS;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));

   ret = virgl_block_read(vws->sock_fd, resp_buf, sizeof(resp_buf));
   if (ret <= 0)
      return 0;

   uint32_t resp_size = resp_buf[0] - 1;
   
   if (resp_size > caps_size)
	  resp_size = caps_size;

   virgl_block_read(vws->sock_fd, &caps->caps, resp_size);
   return 0;
}

int virgl_server_send_resource_create(struct virgl_server_winsys *vws,
                                      uint32_t handle,
                                      enum pipe_texture_target target,
                                      uint32_t format,
                                      uint32_t bind,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t depth,
                                      uint32_t array_size,
                                      uint32_t last_level,
                                      uint32_t nr_samples,
                                      uint32_t size,
                                      int *out_fd)
{
   uint32_t send_buf[13];
   send_buf[0] = 11;
   send_buf[1] = VCMD_RESOURCE_CREATE;
   send_buf[2] = handle;
   send_buf[3] = target;
   send_buf[4] = format;
   send_buf[5] = bind;
   send_buf[6] = width;
   send_buf[7] = height;
   send_buf[8] = depth;
   send_buf[9] = array_size;
   send_buf[10] = last_level;
   send_buf[11] = nr_samples;
   send_buf[12] = size;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   
   if (size == 0)
      return 0;

   *out_fd = virgl_server_recv_fd(vws->sock_fd);
   if (*out_fd < 0)
      return -1;

   return 0;
}

int virgl_server_send_resource_destroy(struct virgl_server_winsys *vws,
                                       uint32_t handle)
{
   uint32_t send_buf[3];
   send_buf[0] = 1;
   send_buf[1] = VCMD_RESOURCE_DESTROY;
   send_buf[2] = handle;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   return 0;
}

int virgl_server_send_transfer_get(struct virgl_server_winsys *vws,
                                   uint32_t handle,
                                   uint32_t level, uint32_t stride,
                                   uint32_t layer_stride,
                                   const struct pipe_box *box,
                                   uint32_t data_size,
                                   uint32_t offset)
{
   uint32_t send_buf[12];
   send_buf[0] = 10;
   send_buf[1] = VCMD_TRANSFER_GET;
   send_buf[2] = handle;
   send_buf[3] = level;
   send_buf[4] = box->x;
   send_buf[5] = box->y;
   send_buf[6] = box->z;
   send_buf[7] = box->width;
   send_buf[8] = box->height;
   send_buf[9] = box->depth;
   send_buf[10] = data_size;
   send_buf[11] = offset;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   return 0;
}

int virgl_server_send_transfer_put(struct virgl_server_winsys *vws,
                                   uint32_t handle,
                                   uint32_t level, uint32_t stride,
                                   uint32_t layer_stride,
                                   const struct pipe_box *box,
                                   uint32_t data_size,
                                   uint32_t offset)
{
   uint32_t send_buf[12];
   send_buf[0] = 10 + ((data_size + 3) / 4);
   send_buf[1] = VCMD_TRANSFER_PUT;
   send_buf[2] = handle;
   send_buf[3] = level;
   send_buf[4] = box->x;
   send_buf[5] = box->y;
   send_buf[6] = box->z;
   send_buf[7] = box->width;
   send_buf[8] = box->height;
   send_buf[9] = box->depth;
   send_buf[10] = data_size;
   send_buf[11] = offset;
   
   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   return 0;
}

int virgl_server_submit_cmd(struct virgl_server_winsys *vws,
                            struct virgl_server_cmd_buf *cbuf)
{
   uint32_t send_buf[2];
   send_buf[0] = cbuf->base.cdw;
   send_buf[1] = VCMD_SUBMIT_CMD;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   virgl_block_write(vws->sock_fd, cbuf->buf, cbuf->base.cdw * 4);
   return 0;
}

int virgl_server_send_resource_busy_wait(struct virgl_server_winsys *vws, int handle,
                           int flags)
{
   uint32_t send_buf[4];
   uint32_t recv_buf[3];
   send_buf[0] = 2;
   send_buf[1] = VCMD_RESOURCE_BUSY_WAIT;
   send_buf[2] = handle;
   send_buf[3] = flags;

   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   virgl_block_read(vws->sock_fd, recv_buf, sizeof(recv_buf));
   return recv_buf[2];
}

int virgl_server_send_flush_frontbuffer(struct virgl_server_winsys *vws,
									    uint32_t handle,
									    uint32_t drawable)
{
   uint32_t send_buf[4];
   send_buf[0] = 2;
   send_buf[1] = VCMD_FLUSH_FRONTBUFFER;
   send_buf[2] = handle;
   send_buf[3] = drawable;
   
   virgl_block_write(vws->sock_fd, &send_buf, sizeof(send_buf));
   return 0;
}
