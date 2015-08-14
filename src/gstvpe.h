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
#include <gst/video/gstvideometa.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include <gst/gst.h>

G_BEGIN_DECLS GST_DEBUG_CATEGORY_EXTERN (gst_vpe_debug);
#define GST_CAT_DEFAULT gst_vpe_debug

/* align x to next highest multiple of 2^n */
#define ALIGN2(x,n)   (((x) + ((1 << (n)) - 1)) & ~((1 << (n)) - 1))

/* Max V4L2 buffer indexes that could be requested */
#define MAX_REQBUF_CNT      32

/* Maximum number of buffers that could be pushed into input Q.
   Due to a bug in the V4L2 driver, omap DRM buffers are 'pinned' at
   queueing time instead of when it is actually needed (dma-time).
   Restricting this to a low value helps to save tiler memory.
   Make sure that this value is less that MAX_REQBUF_CNT/2.5
*/
#define MAX_INPUT_Q_DEPTH   12

#define GST_TYPE_VPE               (gst_vpe_get_type())
#define GST_VPE(obj)               (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VPE, GstVpe))
#define GST_VPE_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VPE, GstVpeClass))
#define GST_IS_VPE(obj)            (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VPE))
#define GST_IS_VPE_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VPE))
#define GST_VPE_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_VPE, GstVpeClass))

typedef struct _GstVpe GstVpe;
typedef struct _GstVpeClass GstVpeClass;


GType gst_vpe_buffer_pool_get_type (void);
#define GST_TYPE_VPE_BUFFER_POOL       (gst_vpe_buffer_pool_get_type())
#define GST_IS_VPE_BUFFER_POOL(obj)    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VPE_BUFFER_POOL))
#define GST_VPE_BUFFER_POOL(obj)       (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_VPE_BUFFER_POOL, GstVpeBufferPool))
#define GST_VPE_BUFFER_POOL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VPE_BUFFER_POOL, GstVpeBufferPoolClass))

typedef struct _GstVpeBufferPool GstVpeBufferPool;
typedef struct _GstVpeBufferPoolClass GstVpeBufferPoolClass;
typedef GstBuffer *(*GstVpeBufferAllocFunction) (void *ctx, int index);

struct _GstVpeBufferPool
{
  GstBufferPool parent;

  gboolean output_port;         /* if true, unusued buffers are automatically re-QBUF'd */
  GMutex lock;
  gboolean shutting_down, streaming;    /* States */
  gboolean interlaced;          /* Whether input is interlaced */
  gint video_fd;                /* a dup(2) of the v4l2object's video_fd */
  guint32 v4l2_type;
  guint buffer_count, min_buffer_count, max_buffer_count;
  guint32 last_field_pushed;    /* Was the last field sent to the dirver top of bottom */
  GstVpeBufferAllocFunction buffer_alloc_function;
  void *buffer_alloc_function_ctx;
  struct GstVpeBufferPoolBufTracking
  {
    GstBuffer *buf;             /* Buffers that are part of this pool */
    gint state;                 /* state of the buffer, FREE, ALLOCATED, WITH_DRIVER */
    gint q_cnt;                 /* Number of times this buffer is queued into the driver */
  } *buf_tracking;
  gint free_head;               /* Head pointer to a free index */
  guint8 index_map[MAX_REQBUF_CNT];
};

struct _GstVpeBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

typedef struct
{
  GstMeta meta;

  int size;
  struct omap_bo *bo;
  struct v4l2_buffer v4l2_buf;
  struct v4l2_plane v4l2_planes[2];
} GstMetaVpeBuffer;


GstBuffer *gst_vpe_buffer_new (struct omap_device *dev,
    guint32 fourcc, gint width, gint height, int index, guint32 v4l2_type);

GstMetaVpeBuffer *gst_buffer_add_vpe_buffer_meta (GstBuffer * buf,
    struct omap_device *dev, guint32 fourcc, gint width, gint height, int index,
    guint32 v4l2_type);

GstMetaVpeBuffer *gst_buffer_get_vpe_buffer_meta (GstBuffer * buf);

GstVpeBufferPool *gst_vpe_buffer_pool_new (gboolean output_port,
    guint max_buffer_count, guint min_buffer_count, guint32 v4l2_type,
    GstCaps * caps, GstVpeBufferAllocFunction buffer_alloc_function,
    void *buffer_alloc_function_ctx);

void gst_vpe_buffer_pool_set_min_buffer_count (GstVpeBufferPool * pool,
    guint min_buffer_count);

gboolean gst_vpe_buffer_pool_put (GstVpeBufferPool * pool, GstBuffer * buf);

gboolean gst_vpe_buffer_pool_queue (GstVpeBufferPool * pool, GstBuffer * buf,
    gint * q_cnt);

GstBuffer *gst_vpe_buffer_pool_dequeue (GstVpeBufferPool * pool);

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
  gint input_max_ref_frames;
  guint32 input_fourcc;
  gint output_height, output_width;
  guint32 output_fourcc;
  struct v4l2_crop input_crop;
  gboolean interlaced;
  gboolean fixed_caps;
  gboolean passthrough;
  GstSegment segment;
  enum
  { GST_VPE_ST_INIT, GST_VPE_ST_ACTIVE, GST_VPE_ST_STREAMING,
    GST_VPE_ST_DEINIT
  } state;

  gint video_fd;
  struct omap_device *dev;
  gchar *device;
  gint input_q_depth;
  GQueue input_q;
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
