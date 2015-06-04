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


static GType gst_meta_vpe_buffer_api_get_type (void);
#define GST_META_VPE_BUFFER_API_TYPE (gst_meta_vpe_buffer_api_get_type())

static const GstMetaInfo *gst_meta_vpe_buffer_get_info (void);
#define GST_META_VPE_BUFFER_INFO (gst_meta_vpe_buffer_get_info())

#define GST_META_VPE_BUFFER_GET(buf) ((GstMetaVpeBuffer *)gst_buffer_get_meta(buf,GST_META_VPE_BUFFER_API_TYPE))
#define GST_META_VPE_BUFFER_ADD(buf) ((GstMetaVpeBuffer *)gst_buffer_add_meta(buf,GST_META_VPE_BUFFER_INFO,NULL))


static gboolean
vpebuffer_init_func (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GST_DEBUG ("init called on buffer %p, meta %p", buffer, meta);
  /* nothing to init really, the init function is mostly for allocating
   * additional memory or doing special setup as part of adding the metadata to
   * the buffer*/
  return TRUE;
}

static void
vpebuffer_free_func (GstMeta * vpemeta, GstBuffer * buffer)
{
  GstMetaVpeBuffer *meta = (GstMetaVpeBuffer *) vpemeta;

  VPE_DEBUG ("Free VPE buffer, index: %d, type: %d fd: %d",
      meta->v4l2_buf.index, meta->v4l2_buf.type, meta->v4l2_planes[0].m.fd);

  /* No pool to put back to buffer into, so delete it completely */
  if (meta->bo) {
    /* Close the dmabuff fd */
    close (meta->v4l2_planes[0].m.fd);

    /* Free the DRM buffer */
    omap_bo_del (meta->bo);
  }
}

static gboolean
vpebuffer_transform_func (GstBuffer * transbuf, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  return FALSE;
}

static GType
gst_meta_vpe_buffer_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] = { "vpebuffer", NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstMetaVpeBufferAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static const GstMetaInfo *
gst_meta_vpe_buffer_get_info (void)
{
  static const GstMetaInfo *meta_vpe_buffer_info = NULL;

  if (g_once_init_enter (&meta_vpe_buffer_info)) {
    const GstMetaInfo *mi = gst_meta_register (GST_META_VPE_BUFFER_API_TYPE,
        "GstMetaVpeBuffer",
        sizeof (GstMetaVpeBuffer),
        vpebuffer_init_func, vpebuffer_free_func, vpebuffer_transform_func);
    g_once_init_leave (&meta_vpe_buffer_info, mi);
  }
  return meta_vpe_buffer_info;
}

GstMetaVpeBuffer *
gst_buffer_add_vpe_buffer_meta (GstBuffer * buf, struct omap_device * dev,
    guint32 fourcc, gint width, gint height, int index, guint32 v4l2_type)
{
  GstMetaVpeBuffer *vpebuf;

  vpebuf = GST_META_VPE_BUFFER_ADD (buf);
  if (!vpebuf)
    goto fail;

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
      vpebuf->bo = omap_bo_new (dev, vpebuf->size, OMAP_BO_WC);
      vpebuf->v4l2_buf.length = 1;
      vpebuf->v4l2_buf.m.planes[0].m.fd = omap_bo_dmabuf (vpebuf->bo);
      break;
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', '2'):
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', 'V'):
      vpebuf->size = width * height * 2;
      vpebuf->bo = omap_bo_new (dev, vpebuf->size, OMAP_BO_WC);
      vpebuf->v4l2_buf.length = 1;
      vpebuf->v4l2_buf.m.planes[0].m.fd = omap_bo_dmabuf (vpebuf->bo);
      break;
    case GST_MAKE_FOURCC ('N', 'V', '1', '2'):
      vpebuf->size = (width * height * 3) / 2;
      vpebuf->bo = omap_bo_new (dev, vpebuf->size, OMAP_BO_WC);
      vpebuf->v4l2_buf.length = 2;
      vpebuf->v4l2_buf.m.planes[1].m.fd =
          vpebuf->v4l2_buf.m.planes[0].m.fd = omap_bo_dmabuf (vpebuf->bo);
      vpebuf->v4l2_buf.m.planes[1].data_offset = width * height;
      break;
    default:
      VPE_ERROR ("invalid format: 0x%08x", fourcc);
      goto fail;
  }
  return vpebuf;
fail:
  gst_buffer_unref (buf);
  return NULL;
}

GstMetaVpeBuffer *
gst_buffer_get_vpe_buffer_meta (GstBuffer * buf)
{
  GstMetaVpeBuffer *vpebuf;
  vpebuf = GST_META_VPE_BUFFER_GET (buf);
  return vpebuf;
}


GstBuffer *
gst_vpe_buffer_new (struct omap_device * dev,
    guint32 fourcc, gint width, gint height, int index, guint32 v4l2_type)
{
  GstMetaVpeBuffer *vpemeta;
  GstMetaDmaBuf *dmabuf;
  GstVideoCropMeta *crop;
  int size;
  GstBuffer *buf;

  buf = gst_buffer_new ();
  if (!buf)
    return NULL;

  vpemeta = gst_buffer_add_vpe_buffer_meta (buf, dev,
      fourcc, width, height, index, v4l2_type);
  if (!vpemeta) {
    VPE_ERROR ("Failed to add vpe metadata");
    gst_buffer_unref (buf);
    return NULL;
  }

  gst_buffer_append_memory (buf,
      gst_memory_new_wrapped (GST_MEMORY_FLAG_NO_SHARE,
          omap_bo_map (vpemeta->bo), vpemeta->size, 0, vpemeta->size, NULL,
          NULL));

  /* attach dmabuf handle to buffer so that elements from other
   * plugins can access for zero copy hw accel:
   */
  dmabuf =
      gst_buffer_add_dma_buf_meta (GST_BUFFER (buf),
      omap_bo_dmabuf (vpemeta->bo));
  if (!dmabuf) {
    VPE_DEBUG ("Failed to attach dmabuf to buffer");
    gst_buffer_unref (buf);
    return NULL;
  }

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
