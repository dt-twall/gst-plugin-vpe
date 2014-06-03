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
#endif /*  */

#define _GNU_SOURCE
#include <features.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <gst/gst.h>
static int sigtermed = 0;

#define MAX_PIPELINES 16
static GstElement *p[MAX_PIPELINES];
static void
my_signal_handler (int signum)
{
  if (signum == SIGINT) {
    sigtermed = 1;
  }
}

static void
pad_added_cb (GstElement * element, GstPad * pad, void *data)
{
  GstElement *parse = GST_ELEMENT (data);
  GstPad *sinkpad;
  gchar *theName = GST_PAD_NAME (pad);
  printf ("pad added:%s\n", theName);
  if (strstr (theName, "video") != NULL) {
    sinkpad = gst_element_get_static_pad (parse, "sink");
    gst_pad_link (pad, sinkpad);
    gst_object_unref (sinkpad);
  } else
    printf ("not linking audio pad\n");
}

static const char *sinkname = "kmssink";
static gboolean use_vpe = TRUE;
static gboolean use_scaling = TRUE;
static gint scale_w = 800, scale_h = 480;
static gboolean use_avsync = TRUE;
static GstElement *
create_pipeline (char *arg)
{
  GstElement *src1;
  GstElement *demux;
  GstElement *vdecode, *h264parse, *pipeline, *gstperf, *filter;
  GstElement *vqueue;
  GstElement *queue, *vsink;
  GstCaps *filtercaps;
  const char *decoder_name, *parser_name, *demux_name;
  if (NULL != strcasestr (arg, ".mkv"))
    demux_name = "matroskademux";

  else if (NULL != strcasestr (arg, ".mp4"))
    demux_name = "qtdemux";

  else if (NULL != strcasestr (arg, ".asf"))
    demux_name = "asfdemux";

  else if (NULL != strcasestr (arg, ".wmv"))
    demux_name = "asfdemux";

  else if (NULL != strcasestr (arg, ".ts"))
    demux_name = "mpegtsdemux";

  else
    demux_name = "identity";
  if (NULL != strcasestr (arg, "264")) {
    decoder_name = use_vpe ? "ducatih264decvpe" : "ducatih264dec";
    parser_name = "h264parse";
  }

  else if (NULL != strcasestr (arg, "mpeg2")) {
    decoder_name = use_vpe ? "ducatimpeg2decvpe" : "ducatimpeg2dec";
    parser_name = "mpegvideoparse";
  }

  else if (NULL != strcasestr (arg, "mpeg4")) {
    decoder_name = use_vpe ? "ducatimpeg4decvpe" : "ducatimpeg4dec";
    parser_name = "mpeg4videoparse";
  }

  else {
    decoder_name = use_vpe ? "ducativc1decvpe" : "ducativc1dec";
    parser_name = "identity";
  }
  printf ("Creating pipeline with %s->%s->%s\n", demux_name, parser_name,
      decoder_name);
  pipeline = gst_pipeline_new ("my-pipeline");
  src1 = gst_element_factory_make ("filesrc", "src1");
  if (src1 == NULL)
    printf ("Could not create 'src1' element\r\n");
  demux = gst_element_factory_make (demux_name, "demux");
  if (demux == NULL)
    printf ("Could not create 'demux' element\r\n");
  vdecode = gst_element_factory_make (decoder_name, "vdecode");
  if (vdecode == NULL)
    printf ("Could not create 'omx_h264dec1' element\r\n");
  h264parse = gst_element_factory_make (parser_name, "h264parse");
  if (h264parse == NULL)
    printf ("Could not create 'h264parse' element\r\n");
  vsink = gst_element_factory_make (sinkname, "kmssink");
  if (vsink == NULL)
    printf ("Could not create neither 'omx_videosink' element\r\n");
  gstperf = gst_element_factory_make ("perf", "perf");
  if (gstperf == NULL)
    printf ("Could not create neither 'perf' element\r\n");
  queue = gst_element_factory_make ("queue", "queue");
  if (queue == NULL)
    printf ("Could not create neither 'queue' element\r\n");
  vqueue = gst_element_factory_make ("queue", "vqueue");
  if (vqueue == NULL)
    printf ("Could not create neither 'queue' element\r\n");
  filter =
      gst_element_factory_make (use_scaling ? "capsfilter" : "identity",
      "filter");
  if (filter == NULL)
    printf ("Could not create 'capsfilter' element\r\n");
  filtercaps =
      gst_caps_new_simple ("video/x-raw-yuv", "format", GST_TYPE_FOURCC,
      GST_MAKE_FOURCC ('N', 'V', '1', '2'), "width", G_TYPE_INT, scale_w,
      "height", G_TYPE_INT, scale_h, NULL);
  g_object_set (G_OBJECT (filter), "caps", filtercaps, NULL);
  gst_caps_unref (filtercaps);
  printf ("set locaticn to : %s\n", arg);

  // ================= Add capabilities and properties
  // g_object_set(G_OBJECT (scaler), "num-input-buffers", 24, NULL);
  // g_object_set(G_OBJECT (scaler), "num-output-buffers", 12, NULL);
  // g_object_set(G_OBJECT (vdecode), "max-reorder-frames", 4, NULL);
  g_object_set (G_OBJECT (src1), "location", arg, NULL);
  g_object_set (G_OBJECT (vsink), "sync", use_avsync, NULL);

  // ================= Put pipeline together
  gst_bin_add_many (GST_BIN (pipeline), src1, demux, h264parse, vdecode,
      vsink, gstperf, filter, vqueue, queue, NULL);
  gst_element_link_many (src1, demux, NULL);
  gst_element_link_many (h264parse, queue, vdecode, vqueue, filter, gstperf,
      vsink, NULL);
  if (!g_signal_connect (demux, "pad-added", G_CALLBACK (pad_added_cb),
          (void *) h264parse))
    printf ("Cannot connect pad-added cb\r\n");

  // ================= Run
  printf ("Set Play Mode ...\r\n");
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

#if 0
  {
    GstClock *clk = gst_pipeline_get_clock (GST_PIPELINE (pipeline));
    if (clk) {
      printf ("Pipeline clock is %s\n", GST_OBJECT_NAME (clk));
      gst_object_unref (clk);
    }
  }

#endif /*  */
  return pipeline;
}

gint
main (gint argc, gchar * argv[])
{
  FILE *in = stdin;
  fd_set f;
  char *line = NULL;
  char linebuf[1024];
  char *args[10];
  int n, i, time, j, rate;
  GstEvent *seek_event;
  if (SIG_ERR == signal (SIGINT, my_signal_handler))
    exit (1);

  /* Init GStreamer */
  gst_init (&argc, &argv);
  if (argc > 1 && (0 == strcmp ("-h", argv[1])
          || 0 == strcmp ("--help", argv[1]))) {
    printf ("Usage: %s <options>\n", argv[0]);
    printf
        ("       -s <sinkname>    Specify the video sink name to be used, default: kmssink\n");
    printf ("       -n               Do not use VPE, implies - no scaling\n");
    printf
        ("       -r <width>x<height> Resize the output to widthxheight, no scaling if left blank\n");
    printf ("       -a               Play with no A/V Sync\n");
    printf
        ("       -c <cmds file>   Non-interactive mode, reading commands from <cmds file>\n");
  }
  for (i = 1; i < argc; i++) {
    if (0 == strcmp ("-s", argv[i])) {
      sinkname = argv[i + 1];
      i++;
    }
    if (0 == strcmp ("-n", argv[i])) {
      use_vpe = FALSE;
      use_scaling = FALSE;
      printf ("Not using VPE...\n");
    }
    if (0 == strcmp ("-r", argv[i])) {
      if ((i + 1) < argc
          && 2 == sscanf (argv[i + 1], "%dx%d", &scale_w, &scale_h)) {
        printf ("Scaling output to %dx%d\n", scale_w, scale_h);
      } else {
        use_scaling = FALSE;
        printf ("Not using Scaling...\n");
      }
    }
    if (0 == strcmp ("-a", argv[i])) {
      use_avsync = FALSE;
      printf ("No A/V Sync, playing as fast as possible...\n");
    }
    if (0 == strcmp ("-c", argv[i]) && (i + 1) < argc) {
      in = fopen (argv[i + 1], "r");
      if (!in) {
        printf
            ("cannot open %s to read in commands, reverting to interactive mode\n",
            argv[i + 1]);
        in = stdin;
      }
      i++;
    }
  }
  printf ("Using videosink=%s\n", sinkname);

// To detect memory leaks
  if (!gst_alloc_trace_available ()) {
    printf ("Trace not available (recompile with trace enabled).\n");
  } else {
    gst_alloc_trace_set_flags_all (GST_ALLOC_TRACE_LIVE |
        GST_ALLOC_TRACE_MEM_LIVE);
    gst_alloc_trace_print_live ();
  }
  while (!sigtermed) {
    if (in == stdin) {
      fflush (stdout);
      FD_ZERO (&f);
      FD_SET (0, &f);
    }
    printf ("<Enter ip> ");
    line = fgets (linebuf, 1023, in);
    if (!line)
      continue;
    line[1023] = '\0';
    n = 0;
    while (*line) {
      args[n++] = line;
      while (!isspace (*line))
        line++;
      if (!(*line))
        break;
      *line = '\0';
      line++;
      while (isspace (*line))
        line++;
    }
    if (3 == n && 0 == strcmp ("start", args[0])) {
      i = atoi (args[1]);
      printf ("Starting pipeline %d\n", i);
      p[i] = create_pipeline (args[2]);
    }

    else if (2 == n && 0 == strcmp ("stop", args[0])) {
      i = atoi (args[1]);
      if (p[i]) {
        gst_element_set_state (p[i], GST_STATE_NULL);
        gst_object_unref (p[i]);
        p[i] = NULL;
      }
    }

    else if (2 == n && 0 == strcmp ("pause", args[0])) {
      i = atoi (args[1]);
      if (p[i])
        gst_element_set_state (p[i], GST_STATE_PAUSED);
    }

    else if (2 == n && 0 == strcmp ("resume", args[0])) {
      i = atoi (args[1]);
      if (p[i])
        gst_element_set_state (p[i], GST_STATE_PLAYING);
    }

    else if (3 <= n && 0 == strcmp ("seek", args[0])) {
      i = atoi (args[1]);
      time = atoi (args[2]);
      rate = (n == 3) ? 1 : atoi (args[3]);
      if (p[i]) {
        if (rate == 1) {
          gst_element_seek_simple (p[i], GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
              time * GST_SECOND);
        } else {
          if (rate > 1) {
            seek_event =
                gst_event_new_seek (rate, GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SEEK_TYPE_SET,
                time * GST_SECOND, GST_SEEK_TYPE_NONE, 0);
          } else {
            seek_event =
                gst_event_new_seek (rate, GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SEEK_TYPE_SET,
                0, GST_SEEK_TYPE_SET, time * GST_SECOND);
          }
          gst_element_send_event (p[i], seek_event);
        }
      }
    }

    else if (2 == n && 0 == strcmp ("step", args[0])) {
      i = atoi (args[1]);
      rate = atoi (args[2]);
      if (p[i]) {
        gst_element_send_event (p[i],
            gst_event_new_step (GST_FORMAT_BUFFERS, 1, rate, TRUE, FALSE));
      }
    }

    else if (2 == n && 0 == strcmp ("sleep", args[0])) {
      sleep (atoi (args[1]));
    }

    else if (2 == n && 0 == strcmp ("rewind", args[0])) {
      if (in != stdin) {
        rewind (in);
        j = atoi (args[1]);
        while (--j)
          fgets (linebuf, 1023, in);
      }
    }

    else if (1 == n && 0 == strcmp ("exit", args[0])) {
      break;
    }

    else if (1 == n && 0 == strcmp ("help", args[0])) {
      printf ("Commands available:\n");
      printf (" start  <instance num> <filename>\n");
      printf (" stop   <instance num>\n");
      printf (" pause  <instance num>\n");
      printf (" resume <instance num>\n");
      printf
          (" seek   <instance num> <seek to time in seconds> <optional: playback speed>\n");
      printf (" sleep   <sleep time in seconds>\n");
      printf
          (" rewind <line number> <rewind command file go to line number>\n");
      printf (" exit\n");
    }
  }
  for (i = 0; i < MAX_PIPELINES; i++) {
    if (p[i]) {
      gst_element_set_state (p[i], GST_STATE_NULL);
      gst_object_unref (p[i]);
      p[i] = NULL;
    }
  }
  sleep (1);
  gst_alloc_trace_print_live ();
  {
    char command[256];
    sprintf (command, "ls -l //proc//%d//fd//", getpid ());
    system (command);
  } return 0;
}
