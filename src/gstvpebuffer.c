/*
 * GStreamer
 * Copyright (c) 2014, Texas Instruments Incorporated
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstvpe.h"

#include <unistd.h>

GstVPEBufferPriv *
gst_buffer_get_vpe_buffer_priv (GstVpeBufferPool * pool, GstBuffer * buf)
{
  int fd_copy;
  GstMemory *mem;
  GstVPEBufferPriv *vpemeta;

  mem = gst_buffer_peek_memory (buf, 0);
  fd_copy = gst_fd_memory_get_fd (mem);

  vpemeta = g_hash_table_lookup (pool->vpebufferpriv, (gpointer) fd_copy);
  return vpemeta;
}

GstBuffer *
gst_vpe_buffer_new (GstVpeBufferPool * pool, struct omap_device * dev,
    guint32 fourcc, gint width, gint height, int index, guint32 v4l2_type)
{
  GstVPEBufferPriv *vpemeta;
  GstVideoCropMeta *crop;
  int size = 0;
  GstBuffer *buf;
  GstAllocator *allocator = gst_drm_allocator_get ();

  buf = gst_buffer_new ();
  if (!buf)
    return NULL;

  switch (fourcc) {
    case GST_MAKE_FOURCC ('A', 'R', '2', '4'):
      size = width * height * 4;
      break;
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', '2'):
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', 'V'):
      size = width * height * 2;
      break;
    case GST_MAKE_FOURCC ('N', 'V', '1', '2'):
      size = (width * height * 3) / 2;
      break;
  }

  gst_buffer_append_memory (buf, gst_allocator_alloc (allocator, size, NULL));

  vpemeta = gst_vpe_buffer_priv (pool, dev,
      fourcc, width, height, index, v4l2_type, buf);

  if (!vpemeta) {
    VPE_ERROR ("Failed to add vpe metadata");
    gst_buffer_unref (buf);
    return NULL;
  }


  /* attach dmabuf handle to buffer so that elements from other
   * plugins can access for zero copy hw accel:
   */

  crop = gst_buffer_add_video_crop_meta (buf);
  if (!crop) {
    VPE_DEBUG ("Failed to add crop meta to buffer");
  } else {
    crop->x = 0;
    crop->y = 0;
    crop->height = height;
    crop->width = width;
  }

  VPE_DEBUG ("Allocated a new VPE buffer, %dx%d, index: %d, type: %d",
      width, height, index, v4l2_type);

  return buf;
}

GstBuffer *
gst_vpe_buffer_ref (GstVpeBufferPool * pool, GstBuffer * in)
{
  GstVPEBufferPriv *vpemeta;
  GstVideoCropMeta *crop, *incrop;
  int size;
  GstBuffer *buf;
  GstAllocator *allocator = gst_drm_allocator_get ();
  int fd_copy;
  GstMemory *mem;

  buf = gst_buffer_new ();
  if (!buf)
    return NULL;

  mem = gst_buffer_peek_memory (in, 0);
  fd_copy = gst_fd_memory_get_fd (mem);

  vpemeta = g_hash_table_lookup (pool->vpebufferpriv, (gpointer) fd_copy);
  if (!vpemeta) {
    VPE_ERROR ("Failed to get vpe metadata");
    gst_buffer_unref (buf);
    return NULL;
  }

  gst_buffer_append_memory (buf, gst_buffer_get_memory (buf, 0));

  /* attach dmabuf handle to buffer so that elements from other
   * plugins can access for zero copy hw accel:
   */

  incrop = gst_buffer_get_video_crop_meta (in);
  if (incrop) {
    crop = gst_buffer_add_video_crop_meta (buf);
    if (!crop) {
      VPE_DEBUG ("Failed to add crop meta to buffer");
    } else {
      crop->x = incrop->x;
      crop->y = incrop->y;
      crop->height = incrop->height;
      crop->width = incrop->width;
    }
  }

  return buf;
}

GstBuffer *
gst_vpe_buffer_import (GstVpeBufferPool * pool, struct omap_device * dev,
    guint32 fourcc, gint width, gint height, int index, guint32 v4l2_type,
    GstBuffer * buf)
{

  GstVPEBufferPriv *vpemeta;
  VPE_DEBUG ("Importing buffer");
  vpemeta = gst_vpe_buffer_priv (pool, dev,
      fourcc, width, height, index, v4l2_type, buf);
  if (!vpemeta) {
    VPE_ERROR ("Failed to add vpe metadata");
    gst_buffer_unref (buf);
    return NULL;
  }
  return buf;
}

GstVPEBufferPriv *
gst_vpe_buffer_priv (GstVpeBufferPool * pool, struct omap_device * dev,
    guint32 fourcc, gint width, gint height, int index, guint32 v4l2_type,
    GstBuffer * buf)
{

  GstVPEBufferPriv *vpebuf = g_malloc0 (sizeof (GstVPEBufferPriv));

  int fd_copy;
  GstMemory *mem;

  if (!vpebuf)
    goto fail;

  mem = gst_buffer_peek_memory (buf, 0);
  fd_copy = gst_fd_memory_get_fd (mem);


  vpebuf->size = 0;
  vpebuf->bo = NULL;
  memset (&vpebuf->v4l2_buf, 0, sizeof (vpebuf->v4l2_buf));
  memset (&vpebuf->v4l2_planes, 0, sizeof (vpebuf->v4l2_planes));

  vpebuf->v4l2_buf.type = v4l2_type;
  vpebuf->v4l2_buf.index = index;
  vpebuf->v4l2_buf.m.planes = vpebuf->v4l2_planes;
  vpebuf->v4l2_buf.memory = V4L2_MEMORY_DMABUF;

  switch (fourcc) {
    case GST_MAKE_FOURCC ('A', 'R', '2', '4'):
      vpebuf->size = width * height * 4;
      vpebuf->bo = omap_bo_from_dmabuf (dev, fd_copy);
      vpebuf->v4l2_buf.length = 1;
      vpebuf->v4l2_buf.m.planes[0].m.fd = fd_copy;
      break;
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', '2'):
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', 'V'):
      vpebuf->size = width * height * 2;
      vpebuf->bo = omap_bo_from_dmabuf (dev, fd_copy);
      vpebuf->v4l2_buf.length = 1;
      vpebuf->v4l2_buf.m.planes[0].m.fd = fd_copy;
      break;
    case GST_MAKE_FOURCC ('N', 'V', '1', '2'):
      vpebuf->size = (width * height * 3) / 2;
      vpebuf->bo = omap_bo_from_dmabuf (dev, fd_copy);
      vpebuf->v4l2_buf.length = 1;
      vpebuf->v4l2_buf.m.planes[0].m.fd = fd_copy;
      break;
    default:
      VPE_ERROR ("invalid format: 0x%08x", fourcc);
      goto fail;
  }
  g_hash_table_insert (pool->vpebufferpriv, (gpointer) fd_copy, vpebuf);
  return vpebuf;
fail:
  gst_buffer_unref (buf);
  return NULL;

}
