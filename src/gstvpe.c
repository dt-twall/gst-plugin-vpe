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

#ifndef MIN
#define MIN(a,b)     (((a) < (b)) ? (a) : (b))
#endif


GST_BOILERPLATE (GstVpe, gst_vpe, GstElement, GST_TYPE_ELEMENT);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_YUV ("NV12")));

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_YUV ("NV12")));

enum
{
  PROP_0,
  PROP_NUM_INPUT_BUFFERS,
  PROP_NUM_OUTPUT_BUFFERS
};


#define MAX_NUM_OUTBUFS   16
#define MAX_NUM_INBUFS    24
#define DEFAULT_NUM_OUTBUFS   8
#define DEFAULT_NUM_INBUFS    24

static gboolean
gst_vpe_parse_input_caps (GstVpe * self, GstCaps * input_caps)
{
  gboolean match;
  GstStructure *s;
  gint w, h;

  if (self->input_caps) {
    match = gst_caps_is_strictly_equal (self->input_caps, input_caps);
    GST_DEBUG_OBJECT (self,
        "Already set caps comapred with the new caps, returned %s",
        (match == TRUE) ? "TRUE" : "FALSE");
    if (match == TRUE)
      return TRUE;
  }

  s = gst_caps_get_structure (input_caps, 0);

  /* For interlaced streams, ducati decoder sets caps without the interlaced 
   * at first, and then changes it to set it as true or false, so if interlaced
   * is false, we cannot assume that the stream is pass-through
   */
  self->interlaced = FALSE;
  gst_structure_get_boolean (s, "interlaced", &self->interlaced);

  /* assuming NV12 input and output */
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

  /* Keep a copy of input caps */
  self->input_caps = gst_caps_copy (input_caps);

  return TRUE;
}

static gboolean
gst_vpe_set_output_caps (GstVpe * self)
{
  GstCaps *outcaps;
  GstStructure *s, *out_s;
  gboolean fixed_caps;
  gint fps_n, fps_d;
  gint par_width, par_height;

  if (!self->input_caps)
    return FALSE;

  s = gst_caps_get_structure (self->input_caps, 0);

  fixed_caps = FALSE;

  outcaps = gst_pad_get_allowed_caps (self->srcpad);
  if (outcaps) {
    GST_DEBUG_OBJECT (self, "Downstream allowed caps: %s",
        gst_caps_to_string (outcaps));
    out_s = gst_caps_get_structure (outcaps, 0);
    if (out_s &&
        gst_structure_get_int (out_s, "width", &self->output_width) &&
        gst_structure_get_int (out_s, "height", &self->output_height)) {
      fixed_caps = TRUE;
    }
  }

  self->passthrough = !(self->interlaced || fixed_caps);

  GST_DEBUG_OBJECT (self, "Passthrough = %s",
      self->passthrough ? "TRUE" : "FALSE");

  if (!fixed_caps) {
    if (self->input_crop.c.width && !self->passthrough) {
      /* Ducati decoder had the habit of setting height as half frame hight for
       * interlaced streams */
      self->output_height = (self->interlaced) ? self->input_crop.c.height * 2 :
          self->input_crop.c.height;
      self->output_width = self->input_crop.c.width;
    } else {
      self->output_height = self->input_height;
      self->output_width = self->input_width;
    }
  }

  gst_caps_unref (outcaps);

  outcaps = gst_caps_new_simple ("video/x-raw-yuv",
      "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('N', 'V', '1', '2'), NULL);

  out_s = gst_caps_get_structure (outcaps, 0);

  gst_structure_set (out_s,
      "width", G_TYPE_INT, self->output_width,
      "height", G_TYPE_INT, self->output_height, NULL);

  if (gst_structure_get_fraction (s, "pixel-aspect-ratio",
          &par_width, &par_height))
    gst_structure_set (out_s, "pixel-aspect-ratio", GST_TYPE_FRACTION,
        par_width, par_height, NULL);

  if (gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d))
    gst_structure_set (out_s, "framerate", GST_TYPE_FRACTION,
        fps_n, fps_d, NULL);

  if (!gst_pad_set_caps (self->srcpad, outcaps))
    return FALSE;

  self->output_caps = outcaps;

  if (self->passthrough && self->input_crop.c.width != 0) {
    /* If passthrough, then the original crop event should be sent downstream */
    gst_pad_push_event (self->srcpad,
        gst_event_new_crop (self->input_crop.c.top, self->input_crop.c.left,
            self->input_crop.c.width, self->input_crop.c.height));
  }
  return TRUE;
}

static gboolean
gst_vpe_init_output_buffers (GstVpe * self)
{
  int i;
  GstVpeBuffer *buf;
  struct v4l2_format fmt;
  int ret;

  self->output_pool = gst_vpe_buffer_pool_new (self->video_fd,
      TRUE, self->num_output_buffers, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
  if (!self->output_pool) {
    return FALSE;
  }

  for (i = 0; i < self->num_output_buffers; i++) {
    buf = gst_vpe_buffer_new (self->dev,
        GST_MAKE_FOURCC ('N', 'V', '1', '2'),
        self->output_width, self->output_height,
        i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (!buf) {
      return FALSE;
    }
    gst_buffer_set_caps (GST_BUFFER (buf), self->output_caps);

    gst_vpe_buffer_pool_put (self->output_pool, buf);

    /* gst_vpe_buffer_pool_put keeps a reference of the buffer,
     * so, unref ours */
    gst_buffer_unref (GST_BUFFER (buf));
  }

  // V4L2 Stuff
  bzero (&fmt, sizeof (fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width = self->output_width;
  fmt.fmt.pix_mp.height = self->output_height;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;

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
  }
  return TRUE;
}

static gboolean
gst_vpe_init_input_buffers (GstVpe * self)
{
  int i;
  GstVpeBuffer *buf;
  struct v4l2_format fmt;
  int ret;

  self->input_pool = gst_vpe_buffer_pool_new (self->video_fd,
      FALSE, self->num_input_buffers, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  if (!self->input_pool) {
    return FALSE;
  }

  for (i = 0; i < self->num_input_buffers; i++) {
    buf = gst_vpe_buffer_new (self->dev,
        GST_MAKE_FOURCC ('N', 'V', '1', '2'),
        self->input_width, self->input_height, i,
        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (!buf) {
      return FALSE;
    }

    if (!gst_vpe_buffer_pool_put (self->input_pool, buf))
      return FALSE;
    /* gst_vpe_buffer_pool_put function keeps a reference, so give up ours */
    gst_buffer_unref (GST_BUFFER (buf));
  }

  // V4L2 Stuff
  bzero (&fmt, sizeof (fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.width = self->input_width;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
#ifdef GST_VPE_USE_FIELD_ALTERNATE
  fmt.fmt.pix_mp.height = (self->input_height >> 1);
  fmt.fmt.pix_mp.field = V4L2_FIELD_ALTERNATE;
#else
  fmt.fmt.pix_mp.height = self->input_height;
  fmt.fmt.pix_mp.field = V4L2_FIELD_SEQ_TB;
  // HACK: bottom-field first is not yet supported 
#endif

  GST_DEBUG_OBJECT (self, "vpe: input S_FMT image: %dx%d, numbufs: %d",
      fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, self->num_input_buffers);

  ret = ioctl (self->video_fd, VIDIOC_S_FMT, &fmt);
  if (ret < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_S_FMT failed");
    return FALSE;
  } else {
    GST_DEBUG_OBJECT (self, "sizeimage[0] = %d, sizeimage[1] = %d",
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
        fmt.fmt.pix_mp.plane_fmt[1].sizeimage);
  }

  if (self->input_crop.c.width != 0) {
    ret = ioctl (self->video_fd, VIDIOC_S_CROP, &self->input_crop);
    if (ret < 0) {
      GST_ERROR_OBJECT (self, "VIDIOC_S_CROP failed");
      return FALSE;
    }
  }
  return TRUE;
}

static void
gst_vpe_try_dequeue (GstVpeBufferPool * pool)
{
  GstVpeBuffer *buf;

  while (1) {
    buf = gst_vpe_buffer_pool_dequeue (pool);
    if (buf) {
      gst_buffer_unref (GST_BUFFER (buf));
    } else
      break;
  }
}

static void
gst_vpe_output_loop (gpointer data)
{
  GstVpe *self = (GstVpe *) data;
  GstVpeBuffer *buf = NULL;

  if (self->output_pool)
    buf = gst_vpe_buffer_pool_dequeue (self->output_pool);
  if (buf) {
    GST_DEBUG_OBJECT (self, "push: %" GST_TIME_FORMAT " (%d bytes, ptr %p)",
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)), GST_BUFFER_SIZE (buf), buf);
    gst_pad_push (self->srcpad, GST_BUFFER (buf));
  } else {
    /* Try dequeueing some buffers while we are here */
    if (self->input_pool)
      gst_vpe_try_dequeue (self->input_pool);
    usleep (10000);
  }
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
  GST_DEBUG_OBJECT (self, "Calling open(/dev/video0)");
  if (self->video_fd < 0) {
    self->video_fd = open ("/dev/video0", O_RDWR | O_NONBLOCK);
    if (self->video_fd < 0) {
      GST_ERROR_OBJECT (self, "Cant open /dev/video0");
      return FALSE;
    }
    GST_DEBUG_OBJECT (self, "Opened /dev/video0");
    gst_vpe_print_driver_capabilities (self);
  }
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
gst_vpe_init_input (GstVpe * self, GstCaps * input_caps)
{

  if (!gst_vpe_create (self)) {
    return FALSE;
  }

  if (!gst_vpe_parse_input_caps (self, input_caps)) {
    GST_ERROR_OBJECT (self, "Could not parse/set caps");
    return FALSE;
  }
  GST_DEBUG_OBJECT (self, "parse/set caps done");

  if (self->input_pool == NULL) {
    if (!gst_vpe_init_input_buffers (self)) {
      GST_ERROR_OBJECT (self, "gst_vpe_init_input_buffers failed");
      return FALSE;
    }
    GST_DEBUG_OBJECT (self, "gst_vpe_init_input_buffers done");
  }
  return TRUE;
}

static void
gst_vpe_set_flushing (GstVpe * self, gboolean flushing)
{
  if (self->input_pool)
    gst_vpe_buffer_pool_set_flushing (self->input_pool, flushing);
}

static void
gst_vpe_set_streaming (GstVpe * self, gboolean streaming)
{
  gboolean ret;
  if (self->input_pool)
    gst_vpe_buffer_pool_set_streaming (self->input_pool, streaming);
  if (self->output_pool) {
    if (!streaming) {
      ret = gst_pad_stop_task (self->srcpad);
      GST_DEBUG_OBJECT (self, "gst_pad_stop_task returned %d", ret);
    }
    gst_vpe_buffer_pool_set_streaming (self->output_pool, streaming);
    if (streaming) {
      ret = gst_pad_start_task (self->srcpad, gst_vpe_output_loop, self);
      GST_DEBUG_OBJECT (self, "gst_pad_start_task returned %d", ret);
    }
  }
}

static gboolean
gst_vpe_start (GstVpe * self, GstCaps * input_caps)
{
  if (!gst_vpe_init_input (self, input_caps)) {
    GST_ERROR_OBJECT (self, "gst_vpe_init_input failed");
    return FALSE;
  }

  if (!gst_vpe_set_output_caps (self)) {
    GST_ERROR_OBJECT (self, "gst_vpe_set_output_caps failed");
    return FALSE;
  }

  if (!gst_vpe_init_output_buffers (self)) {
    GST_ERROR_OBJECT (self, "gst_vpe_init_output_buffers failed");
    return FALSE;
  }
  GST_DEBUG_OBJECT (self, "gst_vpe_init_output_buffers done");

  gst_vpe_set_streaming (self, TRUE);
  GST_DEBUG_OBJECT (self, "task gst_vpe_output_loop started");

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
  self->dev = NULL;

  self->input_width = 0;
  self->input_height = 0;

  self->output_width = 0;
  self->output_height = 0;

  self->input_crop.c.top = 0;
  self->input_crop.c.left = 0;
  self->input_crop.c.width = 0;
  self->input_crop.c.height = 0;
}

static gboolean
gst_vpe_activate_push (GstPad * pad, gboolean active)
{
  gboolean result = TRUE;
  GstVpe *self;

  self = GST_VPE (GST_OBJECT_PARENT (pad));
  GST_DEBUG_OBJECT (self, "gst_vpe_activate_push (active = %d)", active);

  if (!active) {
    result = gst_pad_stop_task (self->srcpad);
    GST_DEBUG_OBJECT (self, "task gst_vpe_output_loop stopped");
  }
  return result;
}

static gboolean
gst_vpe_sink_setcaps (GstPad * pad, GstCaps * caps)
{
  gboolean ret = TRUE;
  GstStructure *s;
  GstVpe *self = GST_VPE (gst_pad_get_parent (pad));
  if (caps) {
    ret = gst_vpe_parse_input_caps (self, caps);
    GST_INFO_OBJECT (self, "set caps done %d", ret);
  }
  gst_object_unref (self);
  return ret;
}


static GstCaps *
gst_vpe_src_getcaps (GstPad * pad)
{
  GstCaps *caps = NULL;

  caps = GST_PAD_CAPS (pad);
  if (caps == NULL) {
    return gst_caps_copy (gst_pad_get_pad_template_caps (pad));
  } else {
    return gst_caps_copy (caps);
  }
}

static gboolean
gst_vpe_query (GstVpe * self, GstPad * pad,
    GstQuery * query, gboolean * forward)
{
  gboolean res = TRUE;

  *forward = TRUE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_BUFFERS:
      /* TODO: */
      break;
    case GST_QUERY_LATENCY:
      /* TODO: */
      break;
    default:
      break;
  }
  return res;
}

static gboolean
gst_vpe_src_query (GstPad * pad, GstQuery * query)
{
  gboolean res = TRUE, forward = TRUE;
  GstVpe *self = GST_VPE (GST_OBJECT_PARENT (pad));
  GstVpeClass *klass = GST_VPE_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "query: %" GST_PTR_FORMAT, query);
  res = gst_vpe_query (self, pad, query, &forward);
  if (res && forward)
    res = gst_pad_query_default (pad, query);

  return res;
}

static GstFlowReturn
gst_vpe_bufferalloc (GstPad * pad, guint64 offset, guint size,
    GstCaps * caps, GstBuffer ** buf)
{
  GstVpe *self;
  self = GST_VPE (GST_OBJECT_PARENT (pad));

  if (G_UNLIKELY (self->state == GST_VPE_ST_DEINIT))
    return GST_FLOW_ERROR;

  if (G_UNLIKELY (NULL == self->input_pool)) {
    if (!gst_vpe_init_input (self, caps)) {
      return GST_FLOW_ERROR;
    }
  }
  *buf = GST_BUFFER (gst_vpe_buffer_pool_get (self->input_pool));
  if (*buf) {
    gst_buffer_set_caps (*buf, caps);
    return GST_FLOW_OK;
  }
  return GST_FLOW_ERROR;
}


static GstFlowReturn
gst_vpe_chain (GstPad * pad, GstBuffer * buf)
{
  GstVpe *self = GST_VPE (GST_OBJECT_PARENT (pad));
  GstFlowReturn ret = GST_FLOW_OK;

  if (G_UNLIKELY (self->state == GST_VPE_ST_DEINIT)) {
    return GST_FLOW_ERROR;
  }

  if (G_UNLIKELY (self->state != GST_VPE_ST_ACTIVE)) {
    if (!gst_vpe_start (self, GST_BUFFER_CAPS (buf))) {
      return GST_FLOW_ERROR;
    }
  }

  GST_DEBUG_OBJECT (self, "chain: %" GST_TIME_FORMAT " (%d bytes, ptr %p)",
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)), GST_BUFFER_SIZE (buf), buf);

  if (self->passthrough) {
    GST_DEBUG_OBJECT (self, "Passthrough for VPE, passthrough");
    return gst_pad_push (self->srcpad, buf);
  }

  /* Push the buffer into the V4L2 driver */
  if (GST_IS_VPE_BUFFER (buf)) {
    GstVpeBuffer *vpe_buf = GST_VPE_BUFFER (buf);

    if (!gst_vpe_buffer_pool_queue (self->input_pool, vpe_buf)) {
      return GST_FLOW_ERROR;
    }
  } else {
    GST_WARNING_OBJECT (self,
        "This plugin does not support buffers not allocated by self %p", buf);
    gst_buffer_unref (buf);
    return GST_FLOW_OK;
  }
  return GST_FLOW_OK;
}

static gboolean
gst_vpe_event (GstPad * pad, GstEvent * event)
{
  GstVpe *self = GST_VPE (GST_OBJECT_PARENT (pad));
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (self, "begin: event=%s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NEWSEGMENT:
      break;
    case GST_EVENT_CROP:
    {
      gint left, top, width, height;

      gst_event_parse_crop (event, &top, &left, &width, &height);

      if (width == -1)
        width = self->input_width - left;
      if (height == -1)
        height = self->input_height - top;

      self->input_crop.c.top = top;
      self->input_crop.c.left = left;
      self->input_crop.c.width = width;
      self->input_crop.c.height = height;

      if (self->state != GST_VPE_ST_ACTIVE && self->input_pool != NULL) {
        /* Set the crop value to the driver */
        ioctl (self->video_fd, VIDIOC_S_CROP, &self->input_crop);
      }
      return TRUE;
    }
    case GST_EVENT_EOS:
      break;
    case GST_EVENT_FLUSH_STOP:
      gst_vpe_set_flushing (self, TRUE);
      break;
    case GST_EVENT_FLUSH_START:
      gst_vpe_set_flushing (self, FALSE);
      break;
    default:
      break;
  }

  if (ret)
    ret = gst_pad_push_event (self->srcpad, event);
  GST_DEBUG_OBJECT (self, "end ret=%d", ret);

  return ret;
}

static gboolean
gst_vpe_src_event (GstPad * pad, GstEvent * event)
{
  GstVpe *self = GST_VPE (GST_OBJECT_PARENT (pad));
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
    case GST_STATE_CHANGE_NULL_TO_READY:
      self->state = GST_VPE_ST_INIT;
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
      gst_vpe_set_flushing (self, TRUE);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      self->state = GST_VPE_ST_DEINIT;
      gst_vpe_destroy (self);
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

  gst_vpe_destroy (self);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_vpe_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple (element_class,
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

  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_vpe_get_property);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_vpe_set_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_vpe_finalize);
  gstelement_class->change_state = GST_DEBUG_FUNCPTR (gst_vpe_change_state);

  g_object_class_install_property (gobject_class, PROP_NUM_INPUT_BUFFERS,
      g_param_spec_int ("num-input-buffers",
          "Number of input buffers that are allocated and used by this plugin.",
          "The number if input buffers allocated should be specified based on "
          "the upstream element's requirement. For example, if gst-ducati-plugin "
          "is the upstream element, this value should be based on max-reorder-frames "
          "property of that element.",
          3, MAX_NUM_INBUFS, DEFAULT_NUM_INBUFS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_OUTPUT_BUFFERS,
      g_param_spec_int ("num-output-buffers",
          "Number of output buffers that are allocated and used by this plugin.",
          "The number if output buffers allocated should be specified based on "
          "the downstream element's requirement. It is generally set to the minimum "
          "value acceptable to the downstream element to reduce memory usage.",
          3, MAX_NUM_OUTBUFS, DEFAULT_NUM_OUTBUFS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_vpe_init (GstVpe * self, GstVpeClass * klass)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  self->sinkpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template
      (gstelement_class, "sink"), "sink");
  gst_pad_set_setcaps_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_vpe_sink_setcaps));
  gst_pad_set_chain_function (self->sinkpad, GST_DEBUG_FUNCPTR (gst_vpe_chain));
  gst_pad_set_event_function (self->sinkpad, GST_DEBUG_FUNCPTR (gst_vpe_event));
  gst_pad_set_bufferalloc_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_vpe_bufferalloc));

  self->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_pad_set_event_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_vpe_src_event));
  gst_pad_set_query_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_vpe_src_query));
  gst_pad_set_getcaps_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_vpe_src_getcaps));
  gst_pad_set_activatepush_function (self->srcpad, gst_vpe_activate_push);

  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->input_width = 0;
  self->input_height = 0;

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

  self->num_input_buffers = DEFAULT_NUM_INBUFS;
  self->num_output_buffers = DEFAULT_NUM_OUTBUFS;
}

GST_DEBUG_CATEGORY (gst_vpe_debug);

#include "gstvpebins.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_vpe_debug, "vpe", 0, "vpe");

  return (gst_element_register (plugin, "vpe", GST_RANK_NONE, GST_TYPE_VPE) &&
      gst_element_register (plugin, "ducatih264decvpe", GST_RANK_PRIMARY + 1,
          gst_vpe_ducatih264dec_get_type ()) &&
      gst_element_register (plugin, "ducatimpeg2decvpe", GST_RANK_PRIMARY + 1,
          gst_vpe_ducatimpeg2dec_get_type ()) &&
      gst_element_register (plugin, "ducatimpeg4decvpe", GST_RANK_PRIMARY + 1,
          gst_vpe_ducatimpeg4dec_get_type ()) &&
      gst_element_register (plugin, "ducatijpegdecvpe", GST_RANK_PRIMARY + 2,
          gst_vpe_ducatijpegdec_get_type ()) &&
      gst_element_register (plugin, "ducativc1decvpe", GST_RANK_PRIMARY + 2,
          gst_vpe_ducativc1dec_get_type ()));
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "vpeplugin"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, "vpeplugin",
    "Hardware accelerated video porst-processing using TI VPE (V4L2-M2M) driver on DRA7x SoC",
    plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")
