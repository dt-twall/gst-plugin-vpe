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
#include <gst/dmabuf/dmabuf.h>

static GstBufferClass *parent_class;

static void gst_vpe_buffer_finalize (GObject * obj);

G_DEFINE_TYPE (GstVpeBuffer, gst_vpe_buffer, GST_TYPE_BUFFER);


static void
gst_vpe_buffer_init (GstVpeBuffer * self)
{
}

static void
gst_vpe_buffer_class_init (GstVpeBufferClass * klass)
{
  parent_class = g_type_class_peek_parent (klass);

  /* Override the mini-object's finalize routine so we can do cleanup when
   * a GstVpeBufferClass is unref'd.
   */
  klass->parent_class.mini_object_class.finalize =
      (GstMiniObjectFinalizeFunction) gst_vpe_buffer_finalize;
}

static void
gst_vpe_buffer_finalize (GObject * obj)
{
  GstVpeBuffer *buf = GST_VPE_BUFFER (obj);

  if (buf->pool) {
    /* Put the buffer back into the pool */
    if (gst_vpe_buffer_pool_put (buf->pool, buf)) {
      VPE_DEBUG ("Recycled VPE buffer, index: %d, type: %d",
          buf->v4l2_buf.index, buf->v4l2_buf.type);
      return;
    }
  }
  VPE_DEBUG ("Free VPE buffer, index: %d, type: %d",
      buf->v4l2_buf.index, buf->v4l2_buf.type);

  /* No pool to put back to buffer into, so delete it completely */
  if (buf->bo) {
    /* Close the dmabuff fd */
    close (buf->v4l2_planes[0].m.fd);

    /* Free the DRM buffer */
    omap_bo_del (buf->bo);
  }
  GST_BUFFER_CLASS (parent_class)->mini_object_class.
      finalize (GST_MINI_OBJECT (buf));
}

GstVpeBuffer *
gst_vpe_buffer_new (struct omap_device *dev,
    gint fourcc, gint width, gint height, int index, guint32 v4l2_type)
{
  GstVpeBuffer *buf;
  GstDmaBuf *dmabuf;
  int size;

  buf = (GstVpeBuffer *) gst_mini_object_new (GST_TYPE_VPE_BUFFER);

  g_return_val_if_fail (buf != NULL, NULL);

  buf->pool = NULL;
  buf->bo = NULL;
  memset (&buf->v4l2_buf, 0, sizeof (buf->v4l2_buf));
  memset (&buf->v4l2_planes, 0, sizeof (buf->v4l2_planes));

  buf->v4l2_buf.type = v4l2_type;
  buf->v4l2_buf.index = index;
  buf->v4l2_buf.m.planes = buf->v4l2_planes;
  buf->v4l2_buf.memory = V4L2_MEMORY_DMABUF;

  switch (fourcc) {
    case GST_MAKE_FOURCC ('A', 'R', '2', '4'):
      size = width * height * 4;
      buf->bo = omap_bo_new (dev, size, OMAP_BO_WC);
      buf->v4l2_buf.length = 1;
      buf->v4l2_buf.m.planes[0].m.fd = omap_bo_dmabuf (buf->bo);
      break;
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', 'V'):
      size = width * height * 2;
      buf->bo = omap_bo_new (dev, size, OMAP_BO_WC);
      buf->v4l2_buf.length = 1;
      buf->v4l2_buf.m.planes[0].m.fd = omap_bo_dmabuf (buf->bo);
      break;
    case GST_MAKE_FOURCC ('N', 'V', '1', '2'):
      size = (width * height * 3) / 2;
      buf->bo = omap_bo_new (dev, size, OMAP_BO_WC);
      buf->v4l2_buf.length = 2;
      buf->v4l2_buf.m.planes[1].m.fd =
          buf->v4l2_buf.m.planes[0].m.fd = omap_bo_dmabuf (buf->bo);
      buf->v4l2_buf.m.planes[1].data_offset = width * height;
      break;
    default:
      VPE_ERROR ("invalid format: 0x%08x", fourcc);
      goto fail;
  }

  GST_BUFFER_DATA (GST_BUFFER (buf)) = omap_bo_map (buf->bo);
  GST_BUFFER_SIZE (GST_BUFFER (buf)) = size;

  /* attach dmabuf handle to buffer so that elements from other
   * plugins can access for zero copy hw accel:
   */
  // XXX buffer doesn't take ownership of the GstDmaBuf...
  dmabuf = gst_dma_buf_new (omap_bo_dmabuf (buf->bo));
  gst_buffer_set_dma_buf (GST_BUFFER (buf), dmabuf);
  gst_dma_buf_unref (dmabuf);

  VPE_DEBUG ("Allocated a new VPE buffer, index: %d, type: %d",
      index, v4l2_type);

  return buf;
fail:
  gst_buffer_unref (GST_BUFFER (buf));
  return NULL;
}
