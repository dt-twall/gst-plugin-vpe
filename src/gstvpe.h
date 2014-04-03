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

#ifndef __GST_VPE_H__
#define __GST_VPE_H__

#include <stdint.h>
#include <string.h>

#include <stdint.h>
#include <stddef.h>
#include <libdce.h>
#include <omap_drm.h>
#include <omap_drmif.h>
#include <gst/video/video.h>
#include <gst/video/video-crop.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include <gst/gst.h>

G_BEGIN_DECLS GST_DEBUG_CATEGORY_EXTERN (gst_vpe_debug);
#define GST_CAT_DEFAULT gst_vpe_debug

/* align x to next highest multiple of 2^n */
#define ALIGN2(x,n)   (((x) + ((1 << (n)) - 1)) & ~((1 << (n)) - 1))

/** Use field alternate mode instead SEQ_TB */
#define GST_VPE_USE_FIELD_ALTERNATE    1

#define GST_TYPE_VPE               (gst_vpe_get_type())
#define GST_VPE(obj)               (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VPE, GstVpe))
#define GST_VPE_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VPE, GstVpeClass))
#define GST_IS_VPE(obj)            (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VPE))
#define GST_IS_VPE_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VPE))
#define GST_VPE_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_VPE, GstVpeClass))

typedef struct _GstVpe GstVpe;
typedef struct _GstVpeClass GstVpeClass;

GType gst_vpe_buffer_get_type (void);
#define GST_TYPE_VPE_BUFFER         (gst_vpe_buffer_get_type())
#define GST_IS_VPE_BUFFER(obj)      (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VPE_BUFFER))
#define GST_VPE_BUFFER(obj)         (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_VPE_BUFFER, GstVpeBuffer))
#define GST_VPE_BUFFER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VPE_BUFFER, GstVpeBufferClass))

GType gst_vpe_buffer_pool_get_type (void);
#define GST_TYPE_VPE_BUFFER_POOL       (gst_vpe_buffer_pool_get_type())
#define GST_IS_VPE_BUFFER_POOL(obj)    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VPE_BUFFER_POOL))
#define GST_VPE_BUFFER_POOL(obj)       (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_VPE_BUFFER_POOL, GstVpeBufferPool))
#define GST_VPE_BUFFER_POOL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VPE_BUFFER_POOL, GstVpeBufferPoolClass))

typedef struct _GstVpeBufferPool GstVpeBufferPool;
typedef struct _GstVpeBuffer GstVpeBuffer;
typedef struct _GstVpeBufferPoolClass GstVpeBufferPoolClass;
typedef struct _GstVpeBufferClass GstVpeBufferClass;

struct _GstVpeBufferPool
{
  GstMiniObject parent;

  gboolean output_port;         /* if true, unusued buffers are automatically re-QBUF'd */
  GMutex lock;
  gboolean shutting_down, streaming;    /* States */
  gboolean interlaced;          /* Whether input is interlaced */
  gint video_fd;                /* a dup(2) of the v4l2object's video_fd */
  guint32 v4l2_type;
  guint buffer_count;
  guint32 last_field_pushed;    /* Was the last field sent to the dirver top of bottom */
  struct GstVpeBufferPoolBufTracking
  {
    GstVpeBuffer *buf;          /* Buffers that are part of this pool */
    gint state;                 /* state of the buffer, FREE, ALLOCATED, WITH_DRIVER */
    gint q_cnt;                 /* Number of times this buffer is queued into the driver */
  } *buf_tracking;
};

struct _GstVpeBufferPoolClass
{
  GstMiniObjectClass parent_class;
};

struct _GstVpeBuffer
{
  GstBuffer buffer;
  struct omap_bo *bo;
  struct v4l2_buffer v4l2_buf;
  struct v4l2_plane v4l2_planes[2];
  GstVpeBufferPool *pool;
};

struct _GstVpeBufferClass
{
  GstBufferClass parent_class;
};

GstVpeBuffer *gst_vpe_buffer_new (struct omap_device *dev,
    gint fourcc, gint width, gint height, int index, guint32 v4l2_type);

GstVpeBufferPool *gst_vpe_buffer_pool_new (gboolean output_port,
    guint buffer_count, guint32 v4l2_type);

gboolean gst_vpe_buffer_pool_put (GstVpeBufferPool * pool, GstVpeBuffer * buf);

gboolean gst_vpe_buffer_pool_queue (GstVpeBufferPool * pool,
    GstVpeBuffer * buf);

GstVpeBuffer *gst_vpe_buffer_pool_dequeue (GstVpeBufferPool * pool);

GstVpeBuffer *gst_vpe_buffer_pool_get (GstVpeBufferPool * pool);

void gst_vpe_buffer_pool_destroy (GstVpeBufferPool * pool);

gboolean gst_vpe_buffer_pool_set_streaming (GstVpeBufferPool * pool,
    int video_fd, gboolean streaming, gboolean interlaced);

struct _GstVpe
{
  GstElement parent;

  GstPad *sinkpad, *srcpad;

  GstCaps *input_caps, *output_caps;

  GstVpeBufferPool *input_pool, *output_pool;
  gint num_input_buffers, num_output_buffers;
  gint input_height, input_width;
  gint output_height, output_width;
  struct v4l2_crop input_crop;
  gboolean interlaced;
  gboolean passthrough;
  enum
  { GST_VPE_ST_INIT, GST_VPE_ST_ACTIVE, GST_VPE_ST_DEINIT } state;

  gint video_fd;
  struct omap_device *dev;
};

struct _GstVpeClass
{
  GstElementClass parent_class;
};

GType gst_vpe_get_type (void);

#define VPE_LOG(x...)      GST_CAT_LOG(gst_vpe_debug, x)
#define VPE_DEBUG(x...)    GST_CAT_DEBUG(gst_vpe_debug, x)
#define VPE_INFO(x...)     GST_CAT_INFO(gst_vpe_debug, x)
#define VPE_ERROR(x...)    GST_CAT_ERROR(gst_vpe_debug, x)
#define VPE_WARNING(x...)  GST_CAT_WARNING(gst_vpe_debug, x)

G_END_DECLS
#endif /* __GST_VPE_H__ */
