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

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "gstvpe.h"

#define GST_VPE_BUFFER_POOL_LOCK(pool)     g_mutex_lock (&((pool)->lock))
#define GST_VPE_BUFFER_POOL_UNLOCK(pool)   g_mutex_unlock (&((pool)->lock))

enum
{
  BUF_UNALLOCATED,
  BUF_FREE,
  BUF_ALLOCATED,
  BUF_WITH_DRIVER,
};

static void gst_vpe_buffer_pool_finalize (GObject * obj);
static GstFlowReturn gst_vpe_buffer_pool_alloc_buffer (GstBufferPool * bufpool,
    GstBuffer ** buf, GstBufferPoolAcquireParams * params);
static void gst_vpe_buffer_pool_release_buffer (GstBufferPool * pool,
    GstBuffer * buffer);

#define gst_vpe_buffer_pool_parent_class parent_class
G_DEFINE_TYPE (GstVpeBufferPool, gst_vpe_buffer_pool, GST_TYPE_BUFFER_POOL);

static void
gst_vpe_buffer_pool_init (GstVpeBufferPool * self)
{
}

static void
gst_vpe_buffer_pool_class_init (GstVpeBufferPoolClass * klass)
{
  GObjectClass *mo_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *bclass = GST_BUFFER_POOL_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  bclass->alloc_buffer = gst_vpe_buffer_pool_alloc_buffer;
  bclass->release_buffer = gst_vpe_buffer_pool_release_buffer;

  mo_class->finalize = gst_vpe_buffer_pool_finalize;
}

static void
gst_vpe_buffer_pool_finalize (GObject * obj)
{
  GstVpeBufferPool *pool = GST_VPE_BUFFER_POOL (obj);

  VPE_DEBUG ("gst_vpe_buffer_pool_finalize (%s) done",
      pool->output_port ? "output" : "input");
  g_free (pool->buf_tracking);
  g_mutex_clear (&pool->lock);

  G_OBJECT_CLASS (parent_class)->finalize (G_OBJECT (pool));
}

GstVpeBufferPool *
gst_vpe_buffer_pool_new (gboolean output_port, guint max_buffer_count,
    guint min_buffer_count, guint32 v4l2_type, GstCaps * caps,
    GstVpeBufferAllocFunction buffer_alloc_function,
    void *buffer_alloc_function_ctx)
{
  GstVpeBufferPool *pool;
  GstStructure *conf;
  int size;
  GstVideoInfo info;

  g_return_val_if_fail (caps != NULL, NULL);

  pool = (GstVpeBufferPool *) g_object_new (GST_TYPE_VPE_BUFFER_POOL, NULL);
  g_return_val_if_fail (pool != NULL, NULL);

  pool->output_port = output_port;
  pool->shutting_down = FALSE;
  pool->streaming = FALSE;
  pool->v4l2_type = v4l2_type;
  g_mutex_init (&pool->lock);
  pool->buffer_count = max_buffer_count;
  pool->min_buffer_count = min_buffer_count;
  pool->max_buffer_count = max_buffer_count;
  pool->buf_tracking =
      (struct GstVpeBufferPoolBufTracking *) g_malloc0 (max_buffer_count *
      sizeof (struct GstVpeBufferPoolBufTracking));
  pool->buffer_alloc_function = buffer_alloc_function;
  pool->buffer_alloc_function_ctx = buffer_alloc_function_ctx;

  /* get the present config of the buffer pool */
  conf = gst_buffer_pool_get_config (GST_BUFFER_POOL (pool));
  if (conf == NULL) {
    VPE_ERROR ("NULL config obtained after get_config on the pool");
    goto fail;
  }

  if (!gst_video_info_from_caps (&info, caps)) {
    VPE_ERROR ("gst_video_info_from_caps failed");
    goto fail;
  }

  /* set the config params : caps, size of the buffers, min number of buffers,
     max number of buffers (0 for unlimited) */
  gst_buffer_pool_config_set_params (conf, caps, GST_VIDEO_INFO_SIZE (&info), 0,
      0);
  if (conf == NULL) {
    VPE_ERROR ("NULL config after set_params");
    goto fail;
  }

  /* set config of the pool */
  gst_buffer_pool_set_config (GST_BUFFER_POOL (pool), conf);

  gst_buffer_pool_set_active (GST_BUFFER_POOL (pool), TRUE);

  return pool;
fail:
  gst_object_unref (G_OBJECT (pool));
  return FALSE;
}

void
gst_vpe_buffer_pool_set_min_buffer_count (GstVpeBufferPool * pool,
    guint min_buffer_count)
{
  GST_VPE_BUFFER_POOL_LOCK (pool);
  pool->min_buffer_count = min_buffer_count;
  GST_VPE_BUFFER_POOL_UNLOCK (pool);
}

/* Put a buffer back into the pool 
 * Called either from the application or from buffer finalize handler
 */
gboolean
gst_vpe_buffer_pool_put (GstVpeBufferPool * pool, GstBuffer * buffer)
{
  GstVpeBufferPool *p;
  gboolean ret = TRUE;
  GstMetaVpeBuffer *buf = gst_buffer_get_vpe_buffer_meta (buffer);
  GST_VPE_BUFFER_POOL_LOCK (pool);

  VPE_DEBUG ("Entered for %s Q, buf=%p", pool->output_port ? "output" : "input",
      buffer);

  if (pool->shutting_down) {
    GST_VPE_BUFFER_POOL_UNLOCK (pool);
    gst_buffer_unref (buffer);
    return TRUE;
  } else {
    if (pool->output_port && pool->streaming) {
      int r;
      /* QUEUE this buffer into the driver */
      r = ioctl (pool->video_fd, VIDIOC_QBUF, &buf->v4l2_buf);
      if (r < 0) {
        VPE_ERROR ("vpebufferpool: output QBUF failed: %s, index = %d",
            strerror (errno), buf->v4l2_buf.index);
        ret = FALSE;
      } else {
        VPE_DEBUG ("vpebufferpool: output QBUF succeeded: index = %d",
            buf->v4l2_buf.index);
        pool->buf_tracking[buf->v4l2_buf.index].state = BUF_WITH_DRIVER;
        pool->buf_tracking[buf->v4l2_buf.index].buf = GST_BUFFER (buffer);
        pool->buf_tracking[buf->v4l2_buf.index].q_cnt = 1;
      }
    } else {
      VPE_DEBUG ("vpebufferpool: buf marked free: index = %d, q_cnt = %d",
          buf->v4l2_buf.index, pool->buf_tracking[buf->v4l2_buf.index].q_cnt);
      pool->buf_tracking[buf->v4l2_buf.index].state = BUF_FREE;
      pool->buf_tracking[buf->v4l2_buf.index].buf = GST_BUFFER (buffer);
      pool->buf_tracking[buf->v4l2_buf.index].q_cnt = 1;
    }
  }
  GST_VPE_BUFFER_POOL_UNLOCK (pool);
  return ret;
}

static void
gst_vpe_buffer_pool_free_index_list_init (GstVpeBufferPool * pool, int n)
{
  int i;
  for (i = 0; i < n; i++) {
    pool->index_map[i] = i - 1;
  }
  pool->free_head = n - 1;
}

static int
gst_vpe_buffer_pool_pop_free_index (GstVpeBufferPool * pool, int buf_index)
{
  int i;
  i = pool->free_head;
  if (0 <= i) {
    pool->free_head = pool->index_map[i];
    pool->index_map[i] = buf_index;
  }
  return i;
}

static int
gst_vpe_buffer_pool_push_free_index (GstVpeBufferPool * pool, int v4l2_index)
{
  int i;
  i = pool->index_map[v4l2_index];
  pool->index_map[v4l2_index] = pool->free_head;
  pool->free_head = v4l2_index;
  return i;
}

/* Called to queue the buffer into the driver, if output_port flag is
 * not set
 */
GstBuffer *
gst_vpe_buffer_pool_dequeue (GstVpeBufferPool * pool)
{
  int ret, i;
  struct v4l2_buffer buf;
  struct v4l2_plane planes[2];
  GstBuffer *dqbuf = NULL;

  VPE_LOG ("Entered for %s Q", pool->output_port ? "output" : "input");

  GST_VPE_BUFFER_POOL_LOCK (pool);
  for (i = 0; i < pool->buffer_count; i++) {
    if (pool->buf_tracking[i].state == BUF_WITH_DRIVER) {
      break;
    }
  }
  if (i < pool->buffer_count) {
    GstMetaVpeBuffer *vpebuf =
        gst_buffer_get_vpe_buffer_meta (pool->buf_tracking[i].buf);
    memset (&planes, 0, sizeof planes);
    buf = vpebuf->v4l2_buf;
    buf.m.planes = planes;

    // VPE_DEBUG("try de-queueing buffers from the driver");
    ret = ioctl (pool->video_fd, VIDIOC_DQBUF, &buf);
    if (ret < 0) {
      if (errno == EAGAIN)
        VPE_LOG ("No buffers to DQBUF from %s Q, try again",
            pool->output_port ? "output" : "input");
      else
        VPE_ERROR ("vpebufferpool: DQBUF for %s failed: %s, index = %d",
            pool->output_port ? "output" : "input",
            strerror (errno), buf.index);
      GST_VPE_BUFFER_POOL_UNLOCK (pool);
      return NULL;
    }
    if (!pool->output_port) {
      VPE_LOG ("DQBUF input for v4l2_index: %d", buf.index);
      buf.index = gst_vpe_buffer_pool_push_free_index (pool, buf.index);
    }
    if (pool->buf_tracking[buf.index].state != BUF_WITH_DRIVER) {
      VPE_WARNING ("Dequeued buffer that was not queued, index: %d", buf.index);
      dqbuf = NULL;
    } else {
      dqbuf = pool->buf_tracking[buf.index].buf;
      VPE_LOG
          ("DQBUF for %s succeeded, index: %d, type: %d, field: %d, q_cnt: %d",
          pool->output_port ? "output" : "input", buf.index, buf.type,
          buf.field, pool->buf_tracking[buf.index].q_cnt);
    }
    if (0 < pool->buf_tracking[buf.index].q_cnt)
      pool->buf_tracking[buf.index].q_cnt--;
    if (0 == pool->buf_tracking[buf.index].q_cnt)
      pool->buf_tracking[buf.index].state = BUF_ALLOCATED;
    GST_VPE_BUFFER_POOL_UNLOCK (pool);

    if (dqbuf) {
      if (buf.timestamp.tv_sec == (time_t) - 1) {
        GST_BUFFER_PTS (dqbuf) = GST_CLOCK_TIME_NONE;
      } else {
        /* Assign a timestamp, propogated by the driver */
        GST_BUFFER_PTS (dqbuf) = GST_TIMEVAL_TO_TIME (buf.timestamp);
      }
    }
  } else {
    VPE_LOG ("No buffers to dequeue from %s Q",
        pool->output_port ? "output" : "input");
    GST_VPE_BUFFER_POOL_UNLOCK (pool);
  }
  return dqbuf;
}

/* Called to queue the buffer into the driver, if output_port flag is
 * not set
 */
gboolean
gst_vpe_buffer_pool_queue (GstVpeBufferPool * pool, GstBuffer * buff,
    gint * q_cnt)
{
  int ret = 0;
  GstMetaVpeBuffer *buf = gst_buffer_get_vpe_buffer_meta (buff);

  *q_cnt = 0;
  GST_VPE_BUFFER_POOL_LOCK (pool);
  if (pool->streaming) {
    VPE_DEBUG ("Queueing buffer, fd: %d", buf->v4l2_buf.m.planes[0].m.fd);
    if (GST_CLOCK_TIME_IS_VALID (GST_BUFFER_PTS (buff)))
      GST_TIME_TO_TIMEVAL (GST_BUFFER_PTS (buff), buf->v4l2_buf.timestamp);
    else
      buf->v4l2_buf.timestamp.tv_sec = (time_t) - 1;

    buf->v4l2_buf.bytesused = 0;
    buf->v4l2_planes[0].bytesused = 0;
    buf->v4l2_planes[1].bytesused = 0;

    do {
      if (pool->interlaced) {
        struct v4l2_buffer buffer;
        struct v4l2_plane buf_planes[2];
        gint field_offset;

        buffer = buf->v4l2_buf;
        buffer.m.planes = buf_planes;
        buf_planes[0] = buf->v4l2_planes[0];
        buf_planes[1] = buf->v4l2_planes[1];
        field_offset = buf_planes[1].data_offset >> 1;

        if (GST_BUFFER_FLAG_IS_SET (GST_BUFFER (buff),
                GST_VIDEO_BUFFER_FLAG_TFF)) {
          if (pool->last_field_pushed == 0)
            pool->last_field_pushed = V4L2_FIELD_BOTTOM;
          if (pool->last_field_pushed == V4L2_FIELD_TOP) {
            VPE_WARNING
                ("Top field was last pushed to the driver, same one turned up again");
            ret = 0;
            break;
          }
          pool->last_field_pushed = buffer.field = V4L2_FIELD_TOP;
          buffer.index =
              gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
          /* QUEUE this buffer into the driver */
          VPE_DEBUG ("Queueing V4L2_FIELD_TOP first  index=%d", buffer.index);
          ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
          if (ret < 0) {
            VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
                strerror (errno), buffer.index);
            break;
          }
          (*q_cnt)++;

          buffer = buf->v4l2_buf;
          buffer.timestamp.tv_sec = (time_t) - 1;
          buffer.m.planes = buf_planes;
          buf_planes[0] = buf->v4l2_planes[0];
          buf_planes[1] = buf->v4l2_planes[1];
          pool->last_field_pushed = buffer.field = V4L2_FIELD_BOTTOM;
          buf_planes[0].data_offset += field_offset;
          buf_planes[1].data_offset += (field_offset >> 1);
          buffer.index =
              gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
          /* QUEUE this buffer into the driver */
          VPE_DEBUG ("Queueing V4L2_FIELD_BOTTOM index=%d", buffer.index);
          ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
          if (ret < 0) {
            VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
                strerror (errno), buffer.index);
            break;
          }
          (*q_cnt)++;
          gst_buffer_ref (GST_BUFFER (buff));

          if (GST_BUFFER_FLAG_IS_SET (GST_BUFFER (buff),
                  GST_VIDEO_BUFFER_FLAG_RFF)) {
            buffer = buf->v4l2_buf;
            buffer.timestamp.tv_sec = (time_t) - 1;
            buffer.m.planes = buf_planes;
            buf_planes[0] = buf->v4l2_planes[0];
            buf_planes[1] = buf->v4l2_planes[1];
            pool->last_field_pushed = buffer.field = V4L2_FIELD_TOP;
            buffer.index =
                gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
            /* QUEUE this buffer into the driver */
            VPE_DEBUG ("Queueing V4L2_FIELD_TOP (repeating) index=%d",
                buffer.index);
            ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
            if (ret < 0) {
              VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
                  strerror (errno), buffer.index);
              break;
            }
            (*q_cnt)++;
            gst_buffer_ref (GST_BUFFER (buff));
          }
        } else {
          if (pool->last_field_pushed == 0)
            pool->last_field_pushed = V4L2_FIELD_TOP;
          if (pool->last_field_pushed == V4L2_FIELD_BOTTOM) {
            VPE_WARNING
                ("Bottom field was last pushed to the driver, same one turned up again");
            ret = 0;
            break;
          }
          pool->last_field_pushed = buffer.field = V4L2_FIELD_BOTTOM;
          buf_planes[0].data_offset += field_offset;
          buf_planes[1].data_offset += (field_offset >> 1);
          buffer.index =
              gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
          VPE_DEBUG ("Queueing V4L2_FIELD_BOTTOM first  index=%d",
              buffer.index);
          /* QUEUE this buffer into the driver */
          ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
          if (ret < 0) {
            VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
                strerror (errno), buffer.index);
            break;
          }
          (*q_cnt)++;

          buffer = buf->v4l2_buf;
          buffer.timestamp.tv_sec = (time_t) - 1;
          buffer.m.planes = buf_planes;
          buf_planes[0] = buf->v4l2_planes[0];
          buf_planes[1] = buf->v4l2_planes[1];
          pool->last_field_pushed = buffer.field = V4L2_FIELD_TOP;
          buffer.index =
              gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
          /* QUEUE this buffer into the driver */
          VPE_DEBUG ("Queueing V4L2_FIELD_TOP  index=%d", buffer.index);
          ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
          if (ret < 0) {
            VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
                strerror (errno), buffer.index);
            break;
          }
          (*q_cnt)++;
          gst_buffer_ref (GST_BUFFER (buff));

          if (GST_BUFFER_FLAG_IS_SET (GST_BUFFER (buff),
                  GST_VIDEO_BUFFER_FLAG_RFF)) {
            buffer = buf->v4l2_buf;
            buffer.timestamp.tv_sec = (time_t) - 1;
            buffer.m.planes = buf_planes;
            buf_planes[0] = buf->v4l2_planes[0];
            buf_planes[1] = buf->v4l2_planes[1];
            pool->last_field_pushed = buffer.field = V4L2_FIELD_BOTTOM;
            buf_planes[0].data_offset += field_offset;
            buf_planes[1].data_offset += (field_offset >> 1);
            buffer.index =
                gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
            /* QUEUE this buffer into the driver */
            VPE_DEBUG ("Queueing V4L2_FIELD_BOTTOM (repeating) index=%d",
                buffer.index);
            ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
            if (ret < 0) {
              VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
                  strerror (errno), buffer.index);
              break;
            }
            (*q_cnt)++;
            gst_buffer_ref (GST_BUFFER (buff));
          }
        }
      } else {                  // pool->interlaced
        struct v4l2_buffer buffer;
        struct v4l2_plane buf_planes[2];

        buffer = buf->v4l2_buf;
        buf_planes[0] = buf->v4l2_planes[0];
        buf_planes[1] = buf->v4l2_planes[1];
        buffer.m.planes = buf_planes;
        buffer.field = V4L2_FIELD_ANY;
        buffer.index =
            gst_vpe_buffer_pool_pop_free_index (pool, buf->v4l2_buf.index);
        VPE_DEBUG ("Queueing V4L2_FIELD_ANY index=%d", buffer.index);
        /* QUEUE this buffer into the driver */
        ret = ioctl (pool->video_fd, VIDIOC_QBUF, &buffer);
        if (ret < 0) {
          VPE_ERROR ("vpebufferpool: QBUF failed: %s, index = %d",
              strerror (errno), buffer.index);
          break;
        }
        (*q_cnt)++;
      }
    } while (0);
  }
  if ((*q_cnt)) {
    pool->buf_tracking[buf->v4l2_buf.index].state = BUF_WITH_DRIVER;
  }
  pool->buf_tracking[buf->v4l2_buf.index].q_cnt = (*q_cnt);
  VPE_LOG ("Q_CNT after QBUF index = %d, q_cnt: %d",
      buf->v4l2_buf.index, (*q_cnt));
  GST_VPE_BUFFER_POOL_UNLOCK (pool);

  if (0 == (*q_cnt))
    gst_buffer_unref (GST_BUFFER (buff));

  return (ret == 0) ? TRUE : FALSE;
}

/* Called to get a free buffer from the pool
 */
static GstBuffer *
gst_vpe_buffer_pool_get (GstVpeBufferPool * pool)
{
  int r = -1, i, dbufs = 0;
  GstBuffer *ret = NULL;

  VPE_DEBUG ("Entered gst_vpe_buffer_pool_get");
  GST_VPE_BUFFER_POOL_LOCK (pool);
  if (!pool->shutting_down) {
    for (i = 0; i < pool->buffer_count; i++) {
      if (pool->buf_tracking[i].state == BUF_FREE) {
        ret = pool->buf_tracking[i].buf;
        pool->buf_tracking[i].state = BUF_ALLOCATED;
        pool->buf_tracking[i].q_cnt = 0;
        break;
      }
      if (pool->buf_tracking[i].state == BUF_WITH_DRIVER)
        dbufs++;
      if (r == -1 && pool->buf_tracking[i].state == BUF_UNALLOCATED)
        r = i;
    }
    if (NULL == ret && pool->buffer_alloc_function && r != -1) {
      if (!pool->streaming || dbufs < 4) {
        VPE_WARNING ("Allocating a new input buffer index: %d/%d, %d",
            r, pool->buffer_count, dbufs);
        ret = pool->buffer_alloc_function (pool->buffer_alloc_function_ctx, r);
        if (ret) {
          pool->buf_tracking[r].buf = ret;
          pool->buf_tracking[r].state = BUF_ALLOCATED;
          pool->buf_tracking[r].q_cnt = 0;
          VPE_DEBUG ("New buffer allocated, index: %d", r);
          i = r;
        }
      }
    }
  }
  GST_VPE_BUFFER_POOL_UNLOCK (pool);
  return ret;
}

/* This function makes moves to shutting down state where it waits 
 * for all buffers to be freed before freeing itself.
 *
 * All existing free and driver buffers will be freed.
 */
void
gst_vpe_buffer_pool_destroy (GstVpeBufferPool * pool)
{
  int i, q_cnt;
  GstBuffer *buf;

  GST_VPE_BUFFER_POOL_LOCK (pool);
  gst_buffer_pool_set_active (GST_BUFFER_POOL (pool), FALSE);
  pool->shutting_down = TRUE;

  for (i = 0; i < pool->buffer_count; i++) {
    buf = pool->buf_tracking[i].buf;
    VPE_DEBUG ("Freeing %s buffers: %d, q_cnt: %d, state: %d",
        pool->output_port ? "output" : "input", i,
        pool->buf_tracking[i].q_cnt, pool->buf_tracking[i].state);
    pool->buf_tracking[i].state = BUF_ALLOCATED;
    q_cnt = pool->buf_tracking[i].q_cnt;
    pool->buf_tracking[i].q_cnt = 0;
    GST_VPE_BUFFER_POOL_UNLOCK (pool);
    while (q_cnt--)
      gst_buffer_unref (GST_BUFFER (buf));
    GST_VPE_BUFFER_POOL_LOCK (pool);
  }
  GST_VPE_BUFFER_POOL_UNLOCK (pool);
  gst_object_unref (pool);
}

static gboolean
stream_on (int fd, int type)
{
  int ret = -1;
  ret = ioctl (fd, VIDIOC_STREAMON, &type);
  if (0 > ret) {
    VPE_ERROR ("VIDIOC_STREAMON type=%d failed", type);
    return FALSE;
  }
  return TRUE;
}

static gboolean
stream_off (int fd, int type)
{
  int ret = -1;
  ret = ioctl (fd, VIDIOC_STREAMOFF, &type);
  if (0 > ret) {
    VPE_ERROR ("VIDIOC_STREAMOFF type=%d failed", type);
    return FALSE;
  }
  return TRUE;
}

gboolean
gst_vpe_buffer_pool_set_streaming (GstVpeBufferPool * pool, int video_fd,
    gboolean streaming, gboolean interlaced)
{
  gboolean ret = FALSE;
  int i, q_cnt, r, index;
  GstBuffer *buf;
  struct v4l2_requestbuffers reqbuf;
  struct v4l2_buffer buffer;
  struct v4l2_plane buf_planes[2];
  int req_buf_count;
  GstMetaVpeBuffer *vbuf;


  GST_VPE_BUFFER_POOL_LOCK (pool);
  if (streaming && !pool->streaming) {
    pool->video_fd = dup (video_fd);
    pool->interlaced = interlaced;
    bzero (&reqbuf, sizeof (reqbuf));
    req_buf_count = pool->buffer_count;
    if (!pool->output_port) {
      req_buf_count = req_buf_count * 3;
      if (req_buf_count > MAX_REQBUF_CNT) {
        req_buf_count = MAX_REQBUF_CNT;
      }
    }
    reqbuf.count = req_buf_count;
    reqbuf.type = pool->v4l2_type;
    reqbuf.memory = V4L2_MEMORY_DMABUF;

    r = ioctl (pool->video_fd, VIDIOC_REQBUFS, &reqbuf);
    if (r < 0) {
      VPE_ERROR ("VIDIOC_REQBUFS (input) failed");
      ret = FALSE;
      goto DONE;
    } else if (reqbuf.count != req_buf_count) {
      VPE_ERROR ("REQBUFS asked: %d, got: %d", req_buf_count, reqbuf.count);
      ret = FALSE;
      goto DONE;
    }

    vbuf = gst_buffer_get_vpe_buffer_meta (pool->buf_tracking[0].buf);
    buffer = vbuf->v4l2_buf;
    buffer.m.planes = buf_planes;
    buf_planes[0] = vbuf->v4l2_planes[0];
    buf_planes[1] = vbuf->v4l2_planes[1];

    for (i = 0; i < req_buf_count; i++) {
      buffer.index = i;
      ret = ioctl (pool->video_fd, VIDIOC_QUERYBUF, &buffer);
      if (ret < 0) {
        VPE_ERROR ("Cant query buffers");
        return FALSE;
      }
      VPE_DEBUG ("query buf %s, index = %d, fd = %d, plane[0], size = %d, "
          "plane[1] size = %d", (pool->output_port) ? "output" : "input",
          vbuf->v4l2_buf.index, vbuf->v4l2_buf.m.planes[0].m.fd,
          buffer.m.planes[0].length, buffer.m.planes[1].length);
    }

    if (!pool->output_port)
      gst_vpe_buffer_pool_free_index_list_init (pool, req_buf_count);

    if (pool->output_port) {
      for (i = 0; i < pool->buffer_count; i++) {
        GstMetaVpeBuffer *vbuf =
            gst_buffer_get_vpe_buffer_meta (pool->buf_tracking[i].buf);
        if (pool->buf_tracking[i].state != BUF_FREE)
          continue;
        /* QUEUE all free buffers into the driver */
        r = ioctl (pool->video_fd, VIDIOC_QBUF, &vbuf->v4l2_buf);
        if (r < 0) {
          VPE_ERROR ("vpebufferpool: op QBUF failed: %s, index = %d",
              strerror (errno), vbuf->v4l2_buf.index);
          ret = FALSE;
          goto DONE;
        }
        VPE_DEBUG ("vpebufferpool: op QBUF succeeded: index = %d",
            vbuf->v4l2_buf.index);
        pool->buf_tracking[i].state = BUF_WITH_DRIVER;
        pool->buf_tracking[i].q_cnt = 1;
      }
    }
    VPE_DEBUG ("Start streaming for type: %d", pool->v4l2_type);
    pool->streaming = streaming;

    ret = stream_on (pool->video_fd, pool->v4l2_type);
  } else if (pool->streaming && !streaming) {
    VPE_DEBUG ("Stop streaming for type: %d", pool->v4l2_type);
    pool->streaming = streaming;
    ret = stream_off (pool->video_fd, pool->v4l2_type);
    close (pool->video_fd);
    /* After stream off, free driver buffers */
    for (i = 0; i < pool->buffer_count; i++) {
      if (pool->buf_tracking[i].state == BUF_WITH_DRIVER) {
        buf = pool->buf_tracking[i].buf;
        if (pool->output_port) {
          pool->buf_tracking[i].state = BUF_FREE;
          g_assert (pool->buf_tracking[i].q_cnt == 1);
        } else {
          pool->buf_tracking[i].state = BUF_ALLOCATED;
          q_cnt = pool->buf_tracking[i].q_cnt;
          pool->buf_tracking[i].q_cnt = 0;
          GST_VPE_BUFFER_POOL_UNLOCK (pool);
          while (q_cnt--)
            gst_buffer_unref (GST_BUFFER (buf));
          GST_VPE_BUFFER_POOL_LOCK (pool);
        }
      }
    }
    pool->last_field_pushed = 0;
  }
DONE:
  GST_VPE_BUFFER_POOL_UNLOCK (pool);
  return ret;
}

static GstFlowReturn
gst_vpe_buffer_pool_alloc_buffer (GstBufferPool * bufpool,
    GstBuffer ** buf, GstBufferPoolAcquireParams * params)
{
  GstVpeBufferPool *pool = GST_VPE_BUFFER_POOL (bufpool);
  if (pool->output_port)
    do {
      /* Try dequeueing some buffers */
      *buf = gst_vpe_buffer_pool_dequeue (pool);
      if (*buf) {
        return GST_FLOW_OK;
      }
    } while (0);
  else
    do {
      *buf = GST_BUFFER (gst_vpe_buffer_pool_get (pool));
      if (*buf) {
        VPE_DEBUG ("Returning %p", *buf);
        return GST_FLOW_OK;
      }
      usleep (10000);
    } while (gst_buffer_pool_is_active (bufpool));
  VPE_DEBUG ("Failed %p", *buf);
  return GST_FLOW_ERROR;
}

static void
gst_vpe_buffer_pool_release_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  if (!gst_vpe_buffer_pool_put (GST_VPE_BUFFER_POOL (pool), buffer)) {
    VPE_DEBUG ("Failed to put the buffer to pool");
  }
}
