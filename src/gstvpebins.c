
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

#include "gstvpebins.h"

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("NV12")));

/* HACK!!: The following sink caps are copied from gst-plugins-ducati source
 * If something changes there, the same should be changed here as well !!
 */

/* *INDENT-OFF* */
static GstStaticPadTemplate ducatih264dec_sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "stream-format = byte-stream, "   /* only byte-stream */
        "alignment = au, "          /* only entire frames */
        "width = (int)[ 16, 2048 ], "
        "height = (int)[ 16, 2048 ]; "
        "video/x-h264, "
        "stream-format = byte-stream, "   /* only byte-stream */
        "alignment = au, "          /* only entire frames */
        "width = (int)[ 16, 2048 ], "
        "height = (int)[ 16, 2048 ], "
        "profile = (string) {high, high-10-intra, high-10, high-4:2:2-intra, "
        "high-4:2:2, high-4:4:4-intra, high-4:4:4, cavlc-4:4:4-intra}, "
        "level = (string) {1, 1b, 1.1, 1.2, 1.3, 2, 2.1, 2.2, 3, 3.1, 3.2, 4, 4.1, 4.2, 5.1};")
    );

static GstStaticPadTemplate ducatimpeg2dec_sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpeg, " "mpegversion = (int)[ 1, 2 ], "     // XXX check on MPEG-1..
        "systemstream = (boolean)false, "
        "parsed = (boolean)true, "
        "width = (int)[ 64, 2048 ], "
        "height = (int)[ 64, 2048 ], " "framerate = (fraction)[ 0, max ];")
    );

#define MPEG4DEC_SINKCAPS_COMMON \
    "width = (int)[ 16, 2048 ], " \
    "height = (int)[ 16, 2048 ], " \
    "framerate = (fraction)[ 0, max ]"

static GstStaticPadTemplate ducatimpeg4dec_sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpeg, " "mpegversion = (int)4, " "systemstream = (boolean)false, " MPEG4DEC_SINKCAPS_COMMON ";" "video/x-divx, " "divxversion = (int)[4, 5], "      /* TODO check this */
        MPEG4DEC_SINKCAPS_COMMON ";"
        "video/x-xvid, "
        MPEG4DEC_SINKCAPS_COMMON ";"
        "video/x-3ivx, " MPEG4DEC_SINKCAPS_COMMON ";")
    );

static GstStaticPadTemplate ducativc1dec_sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-wmv, "
        "wmvversion = (int) 3, "
        "format = (string){ WVC1, WMV3 }, "
        "width = (int)[ 16, 2048 ], "
        "height = (int)[ 16, 2048 ], " "framerate = (fraction)[ 0, max ];")
    );

static GstStaticPadTemplate ducatijpegdec_sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg, "
        "parsed = (boolean)true, "
        "width = (int)[ 32, 4096 ], "
        "height = (int)[ 32, 4096 ], " "framerate = (fraction)[ 0, max ];")
    );

#define __DECODER_VPE_BIN_TEMPLATE(type, decoder_name)    \
  \
typedef struct {               \
  GstBin parent;               \
} type;                        \
typedef struct {               \
  GstBinClass parent_class;    \
} type ## Class;               \
  \
static void gst_vpe_ ## decoder_name ## _class_init   (gpointer      g_class); \
static void gst_vpe_ ## decoder_name ## _init         (type          *self,    \
       type ## Class *klass);\
static GstBinClass *decoder_name ## _parent_class = NULL;        \
static void                                                      \
gst_vpe_ ## decoder_name ## _class_init (gpointer g_class)       \
{                                                                            \
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);              \
  gst_element_class_set_static_metadata (element_class,                      \
      #decoder_name "vpe",                                                   \
      "Codec/Decoder/Video",                                                 \
      #decoder_name " + vpe bin", "Harinarayan Bhatta <harinarayan@ti.com>");\
  gst_element_class_add_pad_template (element_class,                         \
      gst_static_pad_template_get (&decoder_name ## _sink_factory));         \
  gst_element_class_add_pad_template (element_class,                         \
      gst_static_pad_template_get (&src_factory));                           \
  decoder_name ## _parent_class =                                \
  (GstBinClass *)g_type_class_peek_parent (g_class);             \
}                                                                \
                                                                             \
static void gst_vpe_##decoder_name##_init (type *self, type ## Class *Klass) \
{                                                                            \
  GstElement *dec, *vpe;                                                     \
  GstPad *srcpad = NULL, *sinkpad = NULL, *tmp;                              \
  vpe = gst_element_factory_make ("vpe", "vpe");                             \
  if (!vpe) {                                                                \
    GST_ERROR("Cannot create (vpe) element");                                \
    return;                                                                  \
  }                                                                          \
  dec = gst_element_factory_make (#decoder_name, "decoder");                 \
  if (!dec) {                                                                \
    GST_ERROR("Cannot create (" #decoder_name ") element");                  \
    gst_object_unref(GST_OBJECT(vpe));                                       \
    return;                                                                  \
  }                                                                          \
  gst_bin_add_many(GST_BIN(self), dec, vpe, NULL);                           \
  gst_element_link_many(dec, vpe, NULL);                                     \
  g_object_set(G_OBJECT (vpe), "num-input-buffers", 0, NULL);                \
  g_object_set(G_OBJECT (vpe), "num-output-buffers", 8, NULL);               \
  g_object_set(G_OBJECT (dec), "max-reorder-frames", 0, NULL);               \
  tmp = gst_element_get_static_pad(dec, "sink");                             \
  if (tmp) {                                                                 \
    sinkpad = gst_ghost_pad_new("sink", tmp);                                \
    gst_object_unref(GST_OBJECT(tmp));                                       \
  }                                                                          \
  tmp = gst_element_get_static_pad(vpe, "src");                              \
  if (tmp) {                                                                 \
    srcpad = gst_ghost_pad_new("src", tmp);                                  \
    gst_object_unref(GST_OBJECT(tmp));                                       \
  }                                                                          \
  if (sinkpad) {                                                             \
    gst_element_add_pad (GST_ELEMENT (self), sinkpad);                       \
  } else {                                                                   \
    GST_ERROR("Cannot add sinkpad");                                         \
  }                                                                          \
  if (srcpad) {                                                              \
    gst_element_add_pad (GST_ELEMENT (self), srcpad);                        \
  } else {                                                                   \
    GST_ERROR("Cannot add sinkpad");                                         \
  }                                                                          \
}                                                                            \
GType                                                          \
gst_vpe_ ## decoder_name ## _get_type (void)                   \
{                                                              \
  static volatile gsize gonce_data = 0;                        \
  if (g_once_init_enter (&gonce_data)) {                       \
  GType _type;                                                 \
  _type = g_type_register_static_simple (GST_TYPE_BIN,         \
  g_intern_static_string (#type),                              \
  sizeof (type ## Class),                                      \
  (GClassInitFunc) gst_vpe_ ## decoder_name ## _class_init,    \
  sizeof (type),                                               \
  (GInstanceInitFunc) gst_vpe_ ## decoder_name ## _init,       \
  (GTypeFlags) 0);                                             \
  g_once_init_leave (&gonce_data, (gsize) _type);              \
  }                                                            \
  return (GType) gonce_data;                                   \
}

__DECODER_VPE_BIN_TEMPLATE(GstDucatiH264decVpe, ducatih264dec);
__DECODER_VPE_BIN_TEMPLATE(GstDucatiMpeg2decVpe, ducatimpeg2dec);
__DECODER_VPE_BIN_TEMPLATE(GstDucatiMpeg4decVpe, ducatimpeg4dec);
__DECODER_VPE_BIN_TEMPLATE(GstDucatiVc1decVpe, ducativc1dec);
__DECODER_VPE_BIN_TEMPLATE(GstDucatiJpegdecVpe, ducatijpegdec);

