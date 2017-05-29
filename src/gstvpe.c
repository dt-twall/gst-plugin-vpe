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
#include <libdce.h>
#include <sched.h>
#include <math.h>

#ifndef MIN
#define MIN(a,b)     (((a) < (b)) ? (a) : (b))
#endif


static void gst_vpe_class_init (GstVpeClass * klass);
static void gst_vpe_init (GstVpe * self, gpointer klass);
static void gst_vpe_base_init (gpointer gclass);
static GstElementClass *parent_class = NULL;

static gboolean gst_vpe_set_output_caps (GstVpe * self);

GType
gst_vpe_get_type (void)
{
  static GType vpe_type = 0;

  if (!vpe_type) {
    static const GTypeInfo vpe_info = {
      sizeof (GstVpeClass),
      (GBaseInitFunc) gst_vpe_base_init,
      NULL,
      (GClassInitFunc) gst_vpe_class_init,
      NULL,
      NULL,
      sizeof (GstVpe),
      0,
      (GInstanceInitFunc) gst_vpe_init,
    };

    vpe_type = g_type_register_static (GST_TYPE_ELEMENT,
        "GstVpe", &vpe_info, 0);
  }
  return vpe_type;
}


static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("NV12")
        ";" GST_VIDEO_CAPS_MAKE ("YUYV")
        ";" GST_VIDEO_CAPS_MAKE ("YUY2")));

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("NV12")
        ";" GST_VIDEO_CAPS_MAKE ("YUYV")
        ";" GST_VIDEO_CAPS_MAKE ("YUY2")));

enum
{
  PROP_0,
  PROP_NUM_INPUT_BUFFERS,
  PROP_NUM_OUTPUT_BUFFERS,
  PROP_DEVICE
};


#define MAX_NUM_OUTBUFS   16
#define MAX_NUM_INBUFS    128
#define DEFAULT_NUM_OUTBUFS   6
#define DEFAULT_NUM_INBUFS    12
#define DEFAULT_DEVICE        "/dev/v4l/by-path/platform-489d0000.vpe-video-index0"

static gboolean
gst_vpe_parse_input_caps (GstVpe * self, GstCaps * input_caps)
{
  gboolean match;
  GstStructure *s;
  gint w, h;
  guint32 fourcc = 0;
  const gchar *fmt = NULL;

  GST_DEBUG_OBJECT (self, "Input caps: %s", gst_caps_to_string (input_caps));

  if (self->input_caps) {
    match = gst_caps_is_strictly_equal (self->input_caps, input_caps);
    GST_DEBUG_OBJECT (self,
        "Already set caps comapred with the new caps, returned %s",
        (match == TRUE) ? "TRUE" : "FALSE");
    if (match == TRUE)
      return TRUE;
  }

  s = gst_caps_get_structure (input_caps, 0);

  fmt = gst_structure_get_string (s, "format");
  if (!fmt) {
    return FALSE;
  }
  fourcc = GST_STR_FOURCC (fmt);
  /* For interlaced streams, ducati decoder sets caps without the interlaced 
   * at first, and then changes it to set it as true or false, so if interlaced
   * is false, we cannot assume that the stream is pass-through
   */
  self->interlaced = FALSE;
  gst_structure_get_boolean (s, "interlaced", &self->interlaced);

  gst_structure_get_int (s, "max-ref-frames", &self->input_max_ref_frames);

  if (!(gst_structure_get_int (s, "width", &w) &&
          gst_structure_get_int (s, "height", &h))) {
    return FALSE;
  }

  if (self->input_width != 0 &&
      (self->input_width != w || self->input_height != h)) {
    GST_DEBUG_OBJECT (self,
        "dynamic changes in height and width are not supported");
    return FALSE;
  }
  self->input_height = h;
  self->input_width = w;
  self->input_fourcc = fourcc;

  /* Keep a copy of input caps */
  if (self->input_caps)
    gst_caps_unref (self->input_caps);
  self->input_caps = gst_caps_copy (input_caps);

  return TRUE;
}

static gboolean
gst_vpe_set_output_caps (GstVpe * self)
{
  GstCaps *outcaps;
  GstStructure *s, *out_s;
  gint fps_n, fps_d;
  gint par_width, par_height;
  const gchar *fmt = NULL;

  if (!self->input_caps)
    return FALSE;

  if (self->fixed_caps)
    return TRUE;

  s = gst_caps_get_structure (self->input_caps, 0);

  outcaps = gst_pad_get_allowed_caps (self->srcpad);
  if (outcaps && !(self->output_caps
          && gst_caps_is_strictly_equal (outcaps, self->output_caps))) {
    GST_DEBUG_OBJECT (self, "Downstream allowed caps: %s",
        gst_caps_to_string (outcaps));
    out_s = gst_caps_get_structure (outcaps, 0);
    fmt = gst_structure_get_string (out_s, "format");

    if (out_s &&
        gst_structure_get_int (out_s, "width", &self->output_width) &&
        gst_structure_get_int (out_s, "height", &self->output_height) && fmt) {
      self->output_fourcc = GST_STR_FOURCC (fmt);
      GST_DEBUG_OBJECT (self, "Using downstream caps, fixed_caps = TRUE");
      self->fixed_caps = TRUE;
      self->output_framerate_d = self->output_framerate_n = 0;
      if (gst_structure_get_fraction (out_s, "framerate", &fps_n, &fps_d)) {
        self->output_framerate_d = fps_d;
        self->output_framerate_n = fps_n;
      }
    }
  }

  if (!self->fixed_caps) {
    if (self->input_crop.c.width && self->interlaced) {
      /* Ducati decoder had the habit of setting height as half frame hight for
       * interlaced streams */
      self->output_height =
          (self->interlaced) ? self->input_crop.c.height *
          2 : self->input_crop.c.height;
      self->output_width = self->input_crop.c.width;
    } else {
      self->output_height = self->input_height;
      self->output_width = self->input_width;
    }
    self->output_fourcc = GST_MAKE_FOURCC ('N', 'V', '1', '2');
  }

  self->passthrough = !(self->interlaced ||
      self->output_width != self->input_width ||
      self->output_height != self->input_height ||
      self->output_fourcc != self->input_fourcc);

  GST_DEBUG_OBJECT (self, "Passthrough = %s",
      self->passthrough ? "TRUE" : "FALSE");

  gst_caps_unref (outcaps);

  outcaps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING,
      gst_video_format_to_string
      (gst_video_format_from_fourcc (self->output_fourcc)), NULL);

  out_s = gst_caps_get_structure (outcaps, 0);

  gst_structure_set (out_s,
      "width", G_TYPE_INT, self->output_width,
      "height", G_TYPE_INT, self->output_height, NULL);

  if (gst_structure_get_fraction (s, "pixel-aspect-ratio",
          &par_width, &par_height))
    gst_structure_set (out_s, "pixel-aspect-ratio", GST_TYPE_FRACTION,
        par_width, par_height, NULL);

  self->input_framerate_d = self->input_framerate_n = 0;
  if (gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d)) {
    self->input_framerate_d = fps_d;
    self->input_framerate_n = fps_n;
  }
  if (self->output_framerate_d == 0) {
    self->output_framerate_d = self->input_framerate_d;
    self->output_framerate_n = self->input_framerate_n;
  }
  if (self->output_framerate_d != 0) {
    gst_structure_set (out_s, "framerate", GST_TYPE_FRACTION,
        self->output_framerate_n, self->output_framerate_d, NULL);
  }
  if (self->output_caps)
    gst_caps_unref (self->output_caps);
  self->output_caps = outcaps;

  self->output_repeat_rate = 1;
  if (self->output_framerate_n != 0 && self->output_framerate_d != 0
      && self->input_framerate_n != 0 && self->input_framerate_d != 0) {
    self->output_repeat_rate =
        (gint) round (((double) self->output_framerate_n *
            (double) self->input_framerate_d) /
        ((double) self->input_framerate_n * (double) self->output_framerate_d));
  }
  GST_DEBUG_OBJECT (self,
      "framerate conversion: from %d/%d to %d/%d, repeat_factor: %d\n",
      self->input_framerate_n, self->input_framerate_d,
      self->output_framerate_n, self->output_framerate_d,
      self->output_repeat_rate);

  return TRUE;
}

static gboolean
gst_vpe_init_output_buffers (GstVpe * self)
{
  int i;
  GstBuffer *buf;
  if (!self->output_caps) {
    GST_DEBUG_OBJECT (self,
        "Output caps should be set before init output buffer");
    return FALSE;
  }
  self->output_pool =
      gst_vpe_buffer_pool_new (TRUE, self->num_output_buffers,
      self->num_output_buffers, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      self->output_caps, NULL, NULL);
  if (!self->output_pool) {
    return FALSE;
  }

  self->output_pool->format = &self->output_format;

  for (i = 0; i < self->num_output_buffers; i++) {
    buf = gst_vpe_buffer_new (self->output_pool, self->dev,
        self->output_fourcc,
        self->output_width, self->output_height,
        i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (!buf) {
      return FALSE;
    }

    gst_vpe_buffer_pool_put (self->output_pool, buf);
    /* gst_vpe_buffer_pool_put keeps a reference of the buffer,
     * so, unref ours 
     gst_buffer_unref (GST_BUFFER (buf)); */
  }
  return TRUE;
}

static int
gst_vpe_fourcc_to_pixelformat (guint32 fourcc)
{
  switch (fourcc) {
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', '2'):
    case GST_MAKE_FOURCC ('Y', 'U', 'Y', 'V'):
      return V4L2_PIX_FMT_YUYV;
    case GST_MAKE_FOURCC ('N', 'V', '1', '2'):
      return V4L2_PIX_FMT_NV12;
  }
  return -1;
}

static gboolean
gst_vpe_output_set_fmt (GstVpe * self)
{
  struct v4l2_format fmt;
  int ret;
  // V4L2 Stuff
  bzero (&fmt, sizeof (fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width = self->output_width;
  fmt.fmt.pix_mp.height = self->output_height;
  fmt.fmt.pix_mp.pixelformat =
      gst_vpe_fourcc_to_pixelformat (self->output_fourcc);
  fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
  fmt.fmt.pix_mp.num_planes = 1;
  GST_DEBUG_OBJECT (self, "vpe: output S_FMT image: %dx%d",
      fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
  ret = ioctl (self->video_fd, VIDIOC_S_FMT, &fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed");
    return FALSE;
  } else {
    GST_DEBUG_OBJECT (self, "sizeimage[0] = %d, sizeimage[1] = %d",
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
        fmt.fmt.pix_mp.plane_fmt[1].sizeimage);
    /* Save a copy of the current format for this pool */
    self->output_format = fmt;
  }
  return TRUE;
}

static GstBuffer *
gst_vpe_alloc_inputbuffer (void *ctx, int index)
{
  GstVpe *self = (GstVpe *) ctx;

  return gst_vpe_buffer_new (self->input_pool, self->dev,
      self->input_fourcc,
      self->input_width, self->input_height, index,
      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
}

static gboolean
gst_vpe_init_input_buffers (GstVpe * self, gint min_num_input_buffers)
{
  int i;
  GstBuffer *buf;
  if (!self->input_caps) {
    GST_DEBUG_OBJECT (self,
        "Input caps should be set before init input buffer");
    return FALSE;
  }
  self->input_pool =
      gst_vpe_buffer_pool_new (FALSE, MAX_NUM_INBUFS, min_num_input_buffers,
      V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, self->input_caps,
      gst_vpe_alloc_inputbuffer, self);
  if (!self->input_pool) {
    return FALSE;
  }

  self->input_pool->format = &self->input_format;
  return TRUE;
}

static gboolean
gst_vpe_input_set_fmt (GstVpe * self)
{
  struct v4l2_format fmt;
  int ret;
  struct v4l2_selection sel = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
    .target = V4L2_SEL_TGT_CROP_DEFAULT,
  };
  struct v4l2_rect r;

  // V4L2 Stuff
  bzero (&fmt, sizeof (fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.width = self->input_width;
  fmt.fmt.pix_mp.pixelformat =
      gst_vpe_fourcc_to_pixelformat (self->input_fourcc);
  if (self->interlaced) {
    fmt.fmt.pix_mp.height = (self->input_height);
    fmt.fmt.pix_mp.field = V4L2_FIELD_SEQ_TB;
  } else {
    fmt.fmt.pix_mp.height = self->input_height;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
  }
  fmt.fmt.pix_mp.num_planes = 1;

  GST_DEBUG_OBJECT (self,
      "input S_FMT field: %d, image: %dx%d, numbufs: %d",
      fmt.fmt.pix_mp.field, fmt.fmt.pix_mp.width,
      fmt.fmt.pix_mp.height, self->num_input_buffers);
  ret = ioctl (self->video_fd, VIDIOC_S_FMT, &fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed");
    return FALSE;
  } else {
    GST_DEBUG_OBJECT (self, "sizeimage[0] = %d, sizeimage[1] = %d",
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
        fmt.fmt.pix_mp.plane_fmt[1].sizeimage);
    /* Save a copy of the current format for this pool */
    self->input_format = fmt;
  }

  if (self->input_crop.c.width != 0) {
    GST_DEBUG_OBJECT (self,
        "Crop values: top: %d, left: %d, width: %d, height: %d",
        self->input_crop.c.top, self->input_crop.c.left,
        self->input_crop.c.width, self->input_crop.c.height);

    ret = ioctl (self->video_fd, VIDIOC_G_SELECTION, &sel);
    if (ret < 0) {
      GST_ERROR_OBJECT (self, "VIDIOC_G_SELECTION for crop failed");
      return FALSE;
    }
    sel.target = V4L2_SEL_TGT_CROP;
    r.width = self->input_crop.c.width;
    r.height = self->input_crop.c.height;
    r.left = self->input_crop.c.left;
    r.top = self->input_crop.c.top;
    sel.r = r;
    ret = ioctl (self->video_fd, VIDIOC_S_SELECTION, &sel);

    if (ret < 0) {
      GST_ERROR_OBJECT (self, "VIDIOC_S_SELECTION for crop failed");
      return FALSE;
    }
  }
  return TRUE;
}


static void
gst_vpe_dequeue_loop (gpointer data)
{
  GstVpe *self = (GstVpe *) data;
  GstBuffer *buf, *b;
  gint q_cnt;
  while (1) {
    buf = NULL;
    GST_OBJECT_LOCK (self);
    if (self->video_fd >= 0 && self->output_pool)
      (void)
          gst_buffer_pool_acquire_buffer (GST_BUFFER_POOL (self->output_pool),
          &buf, NULL);
    if (buf) {
      self->output_q_processing--;
      g_assert (self->output_q_processing >= 0);
    }
    GST_OBJECT_UNLOCK (self);
    if (buf) {
      for (q_cnt = 1; q_cnt < self->output_repeat_rate; q_cnt++) {
        b = gst_vpe_buffer_ref (self->output_pool, buf);
        if (b)
          gst_pad_push (self->srcpad, GST_BUFFER (b));
      }
      GST_DEBUG_OBJECT (self, "push: %" GST_TIME_FORMAT " (ptr %p)",
          GST_TIME_ARGS (GST_BUFFER_PTS (buf)), buf);
      gst_pad_push (self->srcpad, GST_BUFFER (buf));
    } else
      break;
  }
  GST_OBJECT_LOCK (self);
  if (self->video_fd >= 0 && self->input_pool) {
    while (NULL != (buf = gst_vpe_buffer_pool_dequeue (self->input_pool))) {
      self->input_q_depth--;
      g_assert (self->input_q_depth >= 0);
      gst_buffer_unref (buf);
    }
    while (((MAX_INPUT_Q_DEPTH - self->input_q_depth) >= 1)
        && (NULL != (buf = (GstBuffer *) g_queue_pop_head (&self->input_q)))
        && (TRUE == gst_vpe_buffer_pool_queue (self->input_pool, buf, &q_cnt))) {
      self->input_q_depth += q_cnt;
      if (self->interlaced) {
        self->output_q_processing += q_cnt * 2;
      } else {
        self->output_q_processing += q_cnt;
      }
    }
  }
  GST_OBJECT_UNLOCK (self);
  usleep (10000);
}

static void
gst_vpe_print_driver_capabilities (GstVpe * self)
{
  struct v4l2_capability cap;
  if (0 == ioctl (self->video_fd, VIDIOC_QUERYCAP, &cap)) {
    GST_DEBUG_OBJECT (self, "driver:      '%s'", cap.driver);
    GST_DEBUG_OBJECT (self, "card:        '%s'", cap.card);
    GST_DEBUG_OBJECT (self, "bus_info:    '%s'", cap.bus_info);
    GST_DEBUG_OBJECT (self, "version:     %08x", cap.version);
    GST_DEBUG_OBJECT (self, "capabilites: %08x", cap.capabilities);
  } else {
    GST_WARNING_OBJECT (self, "Cannot get V4L2 driver capabilites!");
  }
}

static gboolean
gst_vpe_create (GstVpe * self)
{
  if (self->dev == NULL) {
    self->dev = dce_init ();
    if (self->dev == NULL) {
      GST_ERROR_OBJECT (self, "dce_init() failed");
      return FALSE;
    }
    GST_DEBUG_OBJECT (self, "dce_init() done");
  }
  return TRUE;
}

static gboolean
gst_vpe_init_input_bufs (GstVpe * self, GstCaps * input_caps)
{
  gint min_num_input_buffers;

  if (!gst_vpe_create (self)) {
    return FALSE;
  }

  if (input_caps && !gst_vpe_parse_input_caps (self, input_caps)) {
    GST_ERROR_OBJECT (self, "Could not parse/set caps");
    return FALSE;
  }
  if (self->num_input_buffers) {
    min_num_input_buffers = self->num_input_buffers;
  } else if (self->input_max_ref_frames) {
    min_num_input_buffers = MAX (6, self->input_max_ref_frames);
  } else {
    min_num_input_buffers = DEFAULT_NUM_INBUFS;
  }
  if (min_num_input_buffers > MAX_NUM_INBUFS)
    min_num_input_buffers = MAX_NUM_INBUFS;
  GST_DEBUG_OBJECT (self, "Using min input buffers: %d", min_num_input_buffers);
  GST_DEBUG_OBJECT (self, "parse/set caps done");
  if (self->input_pool == NULL) {
    if (!gst_vpe_init_input_buffers (self, min_num_input_buffers)) {
      GST_ERROR_OBJECT (self, "gst_vpe_init_input_buffers failed");
      return FALSE;
    }
    GST_DEBUG_OBJECT (self, "gst_vpe_init_input_buffers done");
  } else {
    gst_vpe_buffer_pool_set_min_buffer_count (self->input_pool,
        min_num_input_buffers);
  }
  return TRUE;
}

static void
gst_vpe_set_streaming (GstVpe * self, gboolean streaming)
{
  gboolean ret;
  GstBuffer *buf;
  if (streaming) {
    if (self->video_fd < 0) {
      GST_DEBUG_OBJECT (self, "Calling open(%s)", self->device);
      self->video_fd = open (self->device, O_RDWR | O_NONBLOCK);
      if (self->video_fd < 0) {
        GST_ERROR_OBJECT (self, "Cant open %s", self->device);
        return;
      }
      GST_DEBUG_OBJECT (self, "Opened %s", self->device);
      gst_vpe_print_driver_capabilities (self);

      /* Call V4L2 S_FMT for input and output */
      gst_vpe_input_set_fmt (self);
      gst_vpe_output_set_fmt (self);

      if (!gst_vpe_init_input_bufs (self, NULL)) {
        GST_ERROR_OBJECT (self, "gst_vpe_init_input_bufs failed");
      }
      if (self->input_pool)
        gst_vpe_buffer_pool_set_streaming (self->input_pool,
            self->video_fd, streaming, self->interlaced);
      self->output_q_processing = 0;

      if (!self->output_pool) {
        if (!gst_vpe_init_output_buffers (self)) {
          GST_ERROR_OBJECT (self, "gst_vpe_init_output_buffers failed");
        }
        GST_DEBUG_OBJECT (self, "gst_vpe_init_output_buffers done");
      }
      if (self->output_pool)
        gst_vpe_buffer_pool_set_streaming (self->output_pool,
            self->video_fd, streaming, FALSE);
      self->input_q_depth = 0;
    } else {
      GST_DEBUG_OBJECT (self, "streaming already on");
    }
  } else {
    if (self->video_fd >= 0) {
      while (NULL != (buf = (GstBuffer *) g_queue_pop_head (&self->input_q))) {
        gst_buffer_unref (buf);
      }
      if (self->input_pool)
        gst_vpe_buffer_pool_set_streaming (self->input_pool,
            self->video_fd, streaming, self->interlaced);
      if (self->output_pool)
        gst_vpe_buffer_pool_set_streaming (self->output_pool,
            self->video_fd, streaming, FALSE);
      close (self->video_fd);
      self->video_fd = -1;
    } else {
      GST_DEBUG_OBJECT (self, "streaming already off");
    }
  }
}

static gboolean
gst_vpe_start (GstVpe * self, GstCaps * input_caps)
{
  if (!gst_vpe_init_input_bufs (self, input_caps)) {
    GST_ERROR_OBJECT (self, "gst_vpe_init_input_bufs failed");
    return FALSE;
  }

  if (!self->output_pool) {
    if (!gst_vpe_set_output_caps (self)) {
      GST_ERROR_OBJECT (self, "gst_vpe_set_output_caps failed");
      return FALSE;
    }
  }
  self->state = GST_VPE_ST_ACTIVE;
  return TRUE;
}

static void
gst_vpe_destroy (GstVpe * self)
{
  gst_vpe_set_streaming (self, FALSE);
  if (self->input_caps)
    gst_caps_unref (self->input_caps);
  self->input_caps = NULL;
  if (self->output_caps)
    gst_caps_unref (self->output_caps);
  self->output_caps = NULL;
  self->fixed_caps = FALSE;
  if (self->input_pool) {
    gst_vpe_buffer_pool_destroy (self->input_pool);
    GST_DEBUG_OBJECT (self, "gst_vpe_buffer_pool_destroy(input) done");
  }
  self->input_pool = NULL;
  if (self->output_pool) {
    gst_vpe_buffer_pool_destroy (self->output_pool);
    GST_DEBUG_OBJECT (self, "gst_vpe_buffer_pool_destroy(output) done");
  }
  self->output_pool = NULL;
  if (self->video_fd >= 0)
    close (self->video_fd);
  self->video_fd = -1;
  if (self->dev)
    dce_deinit (self->dev);
  GST_DEBUG_OBJECT (self, "dce_deinit done");
  gst_segment_init (&self->segment, GST_FORMAT_UNDEFINED);
  self->dev = NULL;
  self->input_width = 0;
  self->input_height = 0;
  self->input_max_ref_frames = 0;
  self->output_width = 0;
  self->output_height = 0;
  self->input_crop.c.top = 0;
  self->input_crop.c.left = 0;
  self->input_crop.c.width = 0;
  self->input_crop.c.height = 0;
  self->output_framerate_d = 0;
  self->output_repeat_rate = 1;
  if (self->device)
    g_free (self->device);
  self->device = NULL;
}


static gboolean
gst_vpe_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  if (mode == GST_PAD_MODE_PUSH) {
    gboolean result = TRUE;
    GstVpe *self;
    self = GST_VPE (parent);
    GST_DEBUG_OBJECT (self, "gst_vpe_activate_mode (active = %d)", active);
    if (!active) {
      result = gst_pad_stop_task (self->srcpad);
      GST_DEBUG_OBJECT (self, "task gst_vpe_dequeue_loop stopped");
    } else {
      result =
          gst_pad_start_task (self->srcpad, gst_vpe_dequeue_loop, self, NULL);
      GST_DEBUG_OBJECT (self, "gst_pad_start_task returned %d", result);
    }
    return result;
  }
  return FALSE;
}

static gboolean
gst_vpe_sink_setcaps (GstPad * pad, GstCaps * caps)
{
  gboolean ret = TRUE;
  GstStructure *s;
  GstVpe *self = GST_VPE (gst_pad_get_parent (pad));
  if (caps) {
    GST_OBJECT_LOCK (self);
    if (TRUE == (ret = gst_vpe_parse_input_caps (self, caps))) {
      ret = gst_vpe_set_output_caps (self);
    }
    GST_OBJECT_UNLOCK (self);

    if (TRUE == ret) {
      gst_pad_set_caps (self->srcpad, self->output_caps);
    }

    GST_INFO_OBJECT (self, "set caps done %d", ret);
  }
  gst_object_unref (self);
  return ret;
}

static GstCaps *
gst_vpe_getcaps (GstPad * pad)
{
  GstCaps *caps = NULL;
  caps = gst_pad_get_current_caps (pad);
  if (caps == NULL) {
    GstCaps *fil = gst_pad_get_pad_template_caps (pad);
    return fil;
  } else {
    return caps;
  }
}

static gboolean
gst_vpe_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstVpe *self = GST_VPE (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *caps;
      caps = gst_vpe_getcaps (pad);
      gst_query_set_caps_result (query, caps);
      return TRUE;
      break;
    }
    case GST_QUERY_ALLOCATION:
    {
      GstCaps *caps;
      gst_query_parse_allocation (query, &caps, NULL);

      if (caps == NULL)
        return FALSE;

      GST_OBJECT_LOCK (self);
      if (G_UNLIKELY (self->state == GST_VPE_ST_DEINIT)) {
        GST_OBJECT_UNLOCK (self);
        GST_WARNING_OBJECT (self, "Plugin is shutting down, returning FALSE");
        return FALSE;
      }

      if (!gst_vpe_init_input_bufs (self, caps)) {
        GST_OBJECT_UNLOCK (self);
        return FALSE;
      }

      gst_query_add_allocation_pool (query,
          GST_BUFFER_POOL (self->input_pool), 1, 0, self->num_input_buffers);

      gst_query_add_allocation_param (query, gst_drm_allocator_get (), NULL);

      GST_OBJECT_UNLOCK (self);
      gst_caps_unref (caps);
      return TRUE;
      break;
    }
    case GST_QUERY_LATENCY:
      /* TODO: */
      break;
    default:
      break;
  }
  return gst_pad_query_default (pad, parent, query);
}

static GstFlowReturn
gst_vpe_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstVpe *self = GST_VPE (parent);
  gint q_cnt;
  GstVPEBufferPriv *vpe_buf;

  GST_DEBUG_OBJECT (self, "chain: %" GST_TIME_FORMAT " ( ptr %p)",
      GST_TIME_ARGS (GST_BUFFER_PTS (buf)), buf);

  GST_OBJECT_LOCK (self);
  if (G_UNLIKELY (self->state != GST_VPE_ST_ACTIVE &&
          self->state != GST_VPE_ST_STREAMING)) {
    if (self->state == GST_VPE_ST_DEINIT) {
      GST_OBJECT_UNLOCK (self);
      GST_WARNING_OBJECT (self,
          "Plugin is shutting down, freeing buffer: %p", buf);
      gst_buffer_unref (buf);
      return GST_FLOW_OK;
    } else {
      if (self->input_crop.c.width == 0) {
        GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta (buf);
        if (crop) {
          self->input_crop.c.left = crop->x;
          self->input_crop.c.top = crop->y;
          self->input_crop.c.width = crop->width;
          self->input_crop.c.height = crop->height;
        }
      }
      if (gst_vpe_start (self, gst_pad_get_current_caps (pad))) {
        GST_OBJECT_UNLOCK (self);
        /* Set output caps, this should be done outside the lock */
        gst_pad_set_caps (self->srcpad, self->output_caps);
        GST_OBJECT_LOCK (self);
      } else {
        GST_OBJECT_UNLOCK (self);
        return GST_FLOW_ERROR;
      }
    }
  }

  if (self->passthrough) {
    GST_OBJECT_UNLOCK (self);
    GST_DEBUG_OBJECT (self, "Passthrough for VPE");
    return gst_pad_push (self->srcpad, buf);
  }

  vpe_buf = gst_buffer_get_vpe_buffer_priv (self->input_pool, buf);
  if (!vpe_buf) {
    GST_DEBUG_OBJECT (self, "Importing buffer not allocated by self %p", buf);
    GstBuffer *in = gst_vpe_buffer_pool_import (self->input_pool, buf);
    if (in) {
      vpe_buf = gst_buffer_get_vpe_buffer_priv (self->input_pool, buf);
    }
  }

  if (vpe_buf) {
    if (G_UNLIKELY (self->state != GST_VPE_ST_STREAMING)) {
      gst_vpe_set_streaming (self, TRUE);
      self->state = GST_VPE_ST_STREAMING;
    }
    if ((MAX_INPUT_Q_DEPTH - self->input_q_depth) >= 1) {
      GST_DEBUG_OBJECT (self, "Push the buffer into the V4L2 driver %d",
          self->input_q_depth);
      if (TRUE != gst_vpe_buffer_pool_queue (self->input_pool, buf, &q_cnt)) {
        GST_OBJECT_UNLOCK (self);
        return GST_FLOW_ERROR;
      }
      self->input_q_depth += q_cnt;
      if (self->interlaced) {
        self->output_q_processing += q_cnt * 2;
      } else {
        self->output_q_processing += q_cnt;
      }
    } else {
      g_queue_push_tail (&self->input_q, (gpointer) buf);
    }
  }
  GST_OBJECT_UNLOCK (self);
  /* Allow dequeue thread to run */
  sched_yield ();
  return GST_FLOW_OK;
}

static gboolean
gst_vpe_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstVpe *self = GST_VPE (parent);
  gboolean ret = TRUE;
  GST_DEBUG_OBJECT (self, "begin: event=%s", GST_EVENT_TYPE_NAME (event));
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      gst_event_parse_caps (event, &caps);
      gst_event_unref (event);
      return gst_vpe_sink_setcaps (pad, caps);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      gst_event_copy_segment (event, &self->segment);

      if (self->segment.format == GST_FORMAT_TIME &&
          self->segment.rate < (gdouble) 0.0) {
        GST_OBJECT_LOCK (self);
        /* In case of reverse playback, more input buffers
           are required */
        if (!gst_vpe_init_input_bufs (self, NULL)) {
          GST_ERROR_OBJECT (self, "gst_vpe_init_input_bufs failed");
        }
        GST_OBJECT_UNLOCK (self);
      }
    }
      break;

    case GST_EVENT_EOS:
      while (1) {
        GST_OBJECT_LOCK (self);
        if (TRUE != g_queue_is_empty (&self->input_q)) {
          GST_DEBUG_OBJECT (self,
              "Buffers to be pushed into the V4L2 driver %d",
              self->input_q_depth);
        } else if (0 != self->output_q_processing) {
          GST_DEBUG_OBJECT (self,
              "Buffers to be processed by the V4L2 driver %d",
              self->output_q_processing);
        } else {
          GST_OBJECT_UNLOCK (self);
          GST_DEBUG_OBJECT (self, "VPE ready for EOS");
          break;
        }
        GST_OBJECT_UNLOCK (self);
        usleep (10000);
      }
      break;
    case GST_EVENT_FLUSH_STOP:
      GST_OBJECT_LOCK (self);
      self->state = GST_VPE_ST_INIT;
      GST_OBJECT_UNLOCK (self);
      break;
    case GST_EVENT_FLUSH_START:
      GST_OBJECT_LOCK (self);
      gst_vpe_set_streaming (self, FALSE);
      self->state = GST_VPE_ST_DEINIT;
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      break;
  }

  ret = gst_pad_push_event (self->srcpad, event);
  GST_DEBUG_OBJECT (self, "end ret=%d", ret);
  return ret;
}

static gboolean
gst_vpe_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstVpe *self = GST_VPE (parent);
  gboolean ret = TRUE;
  GST_DEBUG_OBJECT (self, "begin: event=%s", GST_EVENT_TYPE_NAME (event));
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
      // TODO or not!!
      ret = gst_pad_push_event (self->sinkpad, event);
      break;
    default:
      ret = gst_pad_push_event (self->sinkpad, event);
      break;
  }

  GST_DEBUG_OBJECT (self, "end");
  return ret;
}

static GstStateChangeReturn
gst_vpe_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstVpe *self = GST_VPE (element);
  gboolean supported;
  GST_DEBUG_OBJECT (self, "begin: changing state %s -> %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT
          (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));
  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_OBJECT_LOCK (self);
      self->state = GST_VPE_ST_INIT;
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  GST_DEBUG_OBJECT (self, "parent state change returned: %d", ret);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto leave;
  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_OBJECT_LOCK (self);
      gst_vpe_set_streaming (self, FALSE);
      self->state = GST_VPE_ST_DEINIT;
      gst_vpe_destroy (self);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      break;
  }

leave:
  GST_DEBUG_OBJECT (self, "end");
  return ret;
}

/* GObject vmethod implementations */
static void
gst_vpe_get_property (GObject * obj,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVpe *self = GST_VPE (obj);
  switch (prop_id) {
    case PROP_NUM_INPUT_BUFFERS:
      g_value_set_int (value, self->num_input_buffers);
      break;
    case PROP_NUM_OUTPUT_BUFFERS:
      g_value_set_int (value, self->num_output_buffers);
      break;
    case PROP_DEVICE:
      g_value_set_string (value, self->device);
      break;
    default:
    {
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
      break;
    }
  }
}

static void
gst_vpe_set_property (GObject * obj,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVpe *self = GST_VPE (obj);
  switch (prop_id) {
    case PROP_NUM_INPUT_BUFFERS:
      self->num_input_buffers = g_value_get_int (value);
      break;
    case PROP_NUM_OUTPUT_BUFFERS:
      self->num_output_buffers = g_value_get_int (value);
      break;
    case PROP_DEVICE:
      g_free (self->device);
      self->device = g_value_dup_string (value);
      break;
    default:
    {
      G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
      break;
    }
  }
}

static void
gst_vpe_finalize (GObject * obj)
{
  GstVpe *self = GST_VPE (obj);
  GST_OBJECT_LOCK (self);
  gst_vpe_destroy (self);
  GST_OBJECT_UNLOCK (self);
  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_vpe_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);
  gst_element_class_set_static_metadata (element_class,
      "vpe",
      "Filter/Converter/Video",
      "Video processing adapter", "Harinarayan Bhatta <harinarayan@ti.com>");
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
}

static void
gst_vpe_class_init (GstVpeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  parent_class = g_type_class_peek_parent (klass);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_vpe_get_property);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_vpe_set_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_vpe_finalize);
  gstelement_class->change_state = GST_DEBUG_FUNCPTR (gst_vpe_change_state);
  g_object_class_install_property (gobject_class,
      PROP_NUM_INPUT_BUFFERS,
      g_param_spec_int ("num-input-buffers",
          "Number of input buffers that are allocated and used by this plugin.",
          "The number if input buffers allocated should be specified based on "
          "the upstream element's requirement. For example, if gst-ducati-plugin "
          "is the upstream element, this value should be based on max-reorder-frames "
          "property of that element. 0 => decide automatically",
          0, MAX_NUM_INBUFS,
          DEFAULT_NUM_INBUFS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_NUM_OUTPUT_BUFFERS,
      g_param_spec_int ("num-output-buffers",
          "Number of output buffers that are allocated and used by this plugin.",
          "The number if output buffers allocated should be specified based on "
          "the downstream element's requirement. It is generally set to the minimum "
          "value acceptable to the downstream element to reduce memory usage.",
          3, MAX_NUM_OUTBUFS,
          DEFAULT_NUM_OUTBUFS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "Device", "Device location",
          DEFAULT_DEVICE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_vpe_init (GstVpe * self, gpointer klass)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  self->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_chain_function (self->sinkpad, GST_DEBUG_FUNCPTR (gst_vpe_chain));
  gst_pad_set_event_function (self->sinkpad, GST_DEBUG_FUNCPTR (gst_vpe_event));
  self->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_pad_set_event_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_vpe_src_event));
  gst_pad_set_query_function (self->srcpad, GST_DEBUG_FUNCPTR (gst_vpe_query));
  gst_pad_set_query_function (self->sinkpad, GST_DEBUG_FUNCPTR (gst_vpe_query));
  gst_pad_set_activatemode_function (self->srcpad, gst_vpe_activate_mode);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
  self->input_width = 0;
  self->input_height = 0;
  self->input_max_ref_frames = 0;
  self->input_crop.c.top = 0;
  self->input_crop.c.left = 0;
  self->input_crop.c.width = 0;
  self->input_crop.c.height = 0;
  self->input_crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  self->interlaced = FALSE;
  self->state = GST_VPE_ST_INIT;
  self->passthrough = TRUE;
  self->input_pool = NULL;
  self->output_pool = NULL;
  self->dev = NULL;
  self->video_fd = -1;
  self->input_caps = NULL;
  self->output_caps = NULL;
  self->fixed_caps = FALSE;
  self->num_input_buffers = DEFAULT_NUM_INBUFS;
  self->num_output_buffers = DEFAULT_NUM_OUTBUFS;
  self->output_framerate_d = 0;
  self->output_repeat_rate = 1;
  self->device = g_strdup (DEFAULT_DEVICE);
  g_queue_init (&self->input_q);
  self->input_q_depth = 0;
  self->output_q_processing = 0;
  gst_segment_init (&self->segment, GST_FORMAT_UNDEFINED);
}

GST_DEBUG_CATEGORY (gst_vpe_debug);
#include "gstvpebins.h"
static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_vpe_debug, "vpe", 0, "vpe");
  return (gst_element_register (plugin, "vpe", GST_RANK_NONE, GST_TYPE_VPE))
      && gst_element_register (plugin, "ducatih264decvpe",
      GST_RANK_PRIMARY + 2, gst_vpe_ducatih264dec_get_type ())
      && gst_element_register (plugin, "ducatimpeg2decvpe",
      GST_RANK_PRIMARY + 2, gst_vpe_ducatimpeg2dec_get_type ())
      && gst_element_register (plugin, "ducatimpeg4decvpe",
      GST_RANK_PRIMARY + 2, gst_vpe_ducatimpeg4dec_get_type ())
      && gst_element_register (plugin, "ducatijpegdecvpe",
      GST_RANK_PRIMARY + 2, gst_vpe_ducatijpegdec_get_type ())
      && gst_element_register (plugin, "ducativc1decvpe",
      GST_RANK_PRIMARY + 2, gst_vpe_ducativc1dec_get_type ());
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "vpeplugin"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, vpeplugin,
    "Hardware accelerated video porst-processing using TI VPE (V4L2-M2M) driver on DRA7x SoC",
    plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")
