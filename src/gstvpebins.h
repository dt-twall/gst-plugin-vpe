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

#ifndef __GST_VPE_BINS_H__
#define __GST_VPE_BINS_H__

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

GType gst_vpe_ducatih264dec_get_type (void);
GType gst_vpe_ducatimpeg2dec_get_type (void);
GType gst_vpe_ducatimpeg4dec_get_type (void);
GType gst_vpe_ducativc1dec_get_type (void);
GType gst_vpe_ducatijpegdec_get_type (void);

#endif // __GST_VPE_BINS_H__
