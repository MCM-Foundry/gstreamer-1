/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifndef __GST_OMX_VIDEO_DEC_H__
#define __GST_OMX_VIDEO_DEC_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#if defined(USE_EGL_RPI)
#if defined (__GNUC__)
#ifndef __VCCOREVER__
#define __VCCOREVER__ 0x04000000
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC optimize ("gnu89-inline")
#endif

#include <gst/egl/egl.h>

#if defined (__GNUC__)
#pragma GCC reset_options
#pragma GCC diagnostic pop
#endif
#endif /* defined(USE_EGL_RPI) */

#ifdef HAVE_VIDEO_BASE_CLASSES
#include <gst/video/gstvideodecoder.h>
#else
#include "video/gstvideodecoder.h"
#endif

#include "gstomx.h"

G_BEGIN_DECLS

#define GST_TYPE_OMX_VIDEO_DEC \
  (gst_omx_video_dec_get_type())
#define GST_OMX_VIDEO_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OMX_VIDEO_DEC,GstOMXVideoDec))
#define GST_OMX_VIDEO_DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OMX_VIDEO_DEC,GstOMXVideoDecClass))
#define GST_OMX_VIDEO_DEC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_OMX_VIDEO_DEC,GstOMXVideoDecClass))
#define GST_IS_OMX_VIDEO_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OMX_VIDEO_DEC))
#define GST_IS_OMX_VIDEO_DEC_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OMX_VIDEO_DEC))

typedef struct _GstOMXVideoDec GstOMXVideoDec;
typedef struct _GstOMXVideoDecClass GstOMXVideoDecClass;

struct _GstOMXVideoDec
{
  GstVideoDecoder parent;

  /* < protected > */
  GstOMXComponent *dec;
  GstOMXPort *dec_in_port, *dec_out_port;

  /* < private > */
  GstVideoCodecState *input_state;
  GstBuffer *codec_data;
  /* TRUE if the component is configured and saw
   * the first buffer */
  gboolean started;

  GstClockTime last_upstream_ts;

  /* Draining state */
  GMutex *drain_lock;
  GCond *drain_cond;
  /* TRUE if EOS buffers shouldn't be forwarded */
  gboolean draining;

  /* TRUE if upstream is EOS */
  gboolean eos;

  GstFlowReturn downstream_flow_ret;

#ifdef USE_EGL_RPI
  gboolean egl_transport_allowed;
  gboolean egl_transport_active;

  GstEGLDisplay * egl_display;

  GstOMXComponent *egl_render;
  GstOMXPort *egl_in_port, *egl_out_port;
  
  GstEGLImageMemoryPool * out_port_pool;
#endif
};

struct _GstOMXVideoDecClass
{
  GstVideoDecoderClass parent_class;

  GstOMXClassData cdata;

  gboolean (*is_format_change) (GstOMXVideoDec * self, GstOMXPort * port, GstVideoCodecState * state);
  gboolean (*set_format)       (GstOMXVideoDec * self, GstOMXPort * port, GstVideoCodecState * state);
  GstFlowReturn (*prepare_frame)   (GstOMXVideoDec * self, GstVideoCodecFrame *frame);
};

GType gst_omx_video_dec_get_type (void);

G_END_DECLS

#endif /* __GST_OMX_VIDEO_DEC_H__ */
