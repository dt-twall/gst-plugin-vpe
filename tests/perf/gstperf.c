/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2012 Harinarayan Bhatta <<user@hostname.org>>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-perf
 *
 * FIXME:Describe perf here.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m fakesrc ! perf ! fakesink silent=TRUE
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include <string.h>
#include <stdio.h>

#include "gstperf.h"

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );


static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );

GST_BOILERPLATE (GstPerf, gst_perf, GstElement, GST_TYPE_ELEMENT);

static gboolean gst_perf_set_caps (GstPad * pad, GstCaps * caps);
static GstFlowReturn gst_perf_chain (GstPad * pad, GstBuffer * buf);

#define DEFAULT_INTERVAL  1
#define PRINT_ARM_LOAD    TRUE
#define PRINT_FPS         TRUE


/* GObject vmethod implementations */

static void
gst_perf_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple (element_class,
      "perf",
      "Miscellaneous",
      "Print framerate", "Harinarayan Bhatta <harinarayan@ti.com>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
}



/* initialize the perf's class */
static void
gst_perf_class_init (GstPerfClass * klass)
{
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_perf_init (GstPerf * self, GstPerfClass * gclass)
{
  self->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_setcaps_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_perf_set_caps));
  gst_pad_set_chain_function (self->sinkpad,
      GST_DEBUG_FUNCPTR (gst_perf_chain));

  self->srcpad = gst_pad_new_from_static_template (&src_factory, "src");

  gst_pad_set_getcaps_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_pad_proxy_getcaps));
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->fps_update_interval = GST_SECOND * DEFAULT_INTERVAL;
  self->print_arm_load = PRINT_ARM_LOAD;
  self->print_fps = PRINT_FPS;
  self->lastbuf_ts = GST_CLOCK_TIME_NONE;

  /* Init counters */
  self->frames_count = G_GUINT64_CONSTANT (0);
  self->total_size = G_GUINT64_CONSTANT (0);
  self->last_frames_count = G_GUINT64_CONSTANT (0);

  /* init time stamps */
  self->last_ts = self->start_ts = self->interval_ts = GST_CLOCK_TIME_NONE;

}

/* GstElement vmethod implementations */

/* this function handles the link with other elements */
static gboolean
gst_perf_set_caps (GstPad * pad, GstCaps * caps)
{
  GstPerf *self;
  GstPad *otherpad;

  self = GST_PERF (gst_pad_get_parent (pad));
  otherpad = (pad == self->srcpad) ? self->sinkpad : self->srcpad;
  gst_object_unref (self);

  return gst_pad_set_caps (otherpad, caps);
}

static gboolean
display_current_fps (gpointer data)
{
  GstPerf *self = GST_PERF (data);
  guint64 frames_count;
  gdouble rr, average_fps, average_bitrate;
  gchar fps_message[256];
  gdouble time_diff, time_elapsed;
  GstClockTime current_ts = gst_util_get_timestamp ();
  char *name = GST_OBJECT_NAME (self);

  frames_count = self->frames_count;

  time_diff = (gdouble) (current_ts - self->last_ts) / GST_SECOND;
  time_elapsed = (gdouble) (current_ts - self->start_ts) / GST_SECOND;

  rr = (gdouble) (frames_count - self->last_frames_count) / time_diff;

  average_fps = (gdouble) frames_count / time_elapsed;
  average_bitrate = ((gdouble) self->total_size * 8.0) / (time_diff * 1000);

  g_snprintf (fps_message, 255,
      "%" GST_TIME_FORMAT " %s: frames: %" G_GUINT64_FORMAT
      " \tcurrent: %.2f \t average: %.2f \tbitrate: %.2f \tts: %"
      GST_TIME_FORMAT, GST_TIME_ARGS (current_ts), name, frames_count, rr,
      average_fps, average_bitrate, GST_TIME_ARGS (self->lastbuf_ts));
  g_print ("%s", fps_message);

  self->total_size = G_GUINT64_CONSTANT (0);
  self->last_frames_count = frames_count;
  self->last_ts = current_ts;
  self->lastbuf_ts = GST_CLOCK_TIME_NONE;

  return TRUE;
}


static int
print_cpu_load (GstPerf * perf)
{
  int cpuLoadFound = FALSE;
  unsigned long nice, sys, idle, iowait, irq, softirq, steal;
  unsigned long deltaTotal;
  char textBuf[4];
  FILE *fptr;
  int load;

  /* Read the overall system information */
  fptr = fopen ("/proc/stat", "r");

  if (fptr == NULL) {
    return -1;
  }

  perf->prevTotal = perf->total;
  perf->prevuserTime = perf->userTime;

  /* Scan the file line by line */
  while (fscanf (fptr, "%4s %lu %lu %lu %lu %lu %lu %lu %lu", textBuf,
          &perf->userTime, &nice, &sys, &idle, &iowait, &irq, &softirq,
          &steal) != EOF) {
    if (strcmp (textBuf, "cpu") == 0) {
      cpuLoadFound = TRUE;
      break;
    }
  }

  if (fclose (fptr) != 0) {
    return -1;
  }

  if (!cpuLoadFound) {
    return -1;
  }

  perf->total = perf->userTime + nice + sys + idle + iowait + irq + softirq +
      steal;
  perf->userTime += nice + sys + iowait + irq + softirq + steal;
  deltaTotal = perf->total - perf->prevTotal;

  if (deltaTotal) {
    load = 100 * (perf->userTime - perf->prevuserTime) / deltaTotal;
  } else {
    load = 0;
  }

  g_print ("\tarm-load: %d", load);
  return 0;
}

/* chain function
 * this function does the actual processing
 */
static GstFlowReturn
gst_perf_chain (GstPad * pad, GstBuffer * buf)
{
  GstClockTime ts;
  GstPerf *self = GST_PERF (GST_PAD_PARENT (pad));

  self->frames_count++;
  self->total_size += GST_BUFFER_SIZE (buf);

  if (self->lastbuf_ts == GST_CLOCK_TIME_NONE)
    self->lastbuf_ts = GST_BUFFER_TIMESTAMP (buf);

  ts = gst_util_get_timestamp ();
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (self->start_ts))) {
    self->interval_ts = self->last_ts = self->start_ts = ts;
  }

  if (GST_CLOCK_DIFF (self->interval_ts, ts) > self->fps_update_interval) {

    if (self->print_fps)
      display_current_fps (self);

    if (self->print_arm_load)
      print_cpu_load (self);

    g_print ("\n");
    self->interval_ts = ts;
  }
  return gst_pad_push (self->srcpad, buf);
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
rate_init (GstPlugin * perf)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template perf' with your description
   */
  return gst_element_register (perf, "perf", GST_RANK_PRIMARY, GST_TYPE_PERF);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "perf"
#endif

/* gstreamer looks for this structure to register rates
 *
 * exchange the string 'Template perf' with your perf description
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "perf",
    "Performance display",
    rate_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")
