/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2013, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 * Copyright (C) 2013, Fluendo S.A.
 *   Author: Josep Torra <josep@fluendo.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <string.h>

#include "gstomxvideodec.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_video_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_video_dec_debug_category

#define N_EGL_IMAGES 4

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* prototypes */
static void gst_omx_video_dec_finalize (GObject * object);
static void gst_omx_video_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_omx_video_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_omx_video_dec_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_omx_video_dec_open (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_close (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_start (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_omx_video_dec_reset (GstVideoDecoder * decoder,
    gboolean hard);
static GstFlowReturn gst_omx_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_omx_video_dec_finish (GstVideoDecoder * decoder);

static GstFlowReturn gst_omx_video_dec_drain (GstOMXVideoDec * self,
    gboolean is_eos);

static OMX_ERRORTYPE gst_omx_video_dec_allocate_output_buffers (GstOMXVideoDec *
    self);
static OMX_ERRORTYPE gst_omx_video_dec_deallocate_output_buffers (GstOMXVideoDec
    * self);

enum
{
  PROP_0
#ifdef USE_EGL_RPI
      , PROP_POOL
#endif
};

/* class initialization */

#define DEBUG_INIT(bla) \
  GST_DEBUG_CATEGORY_INIT (gst_omx_video_dec_debug_category, "omxvideodec", 0, \
      "debug category for gst-omx video decoder base class");

GST_BOILERPLATE_FULL (GstOMXVideoDec, gst_omx_video_dec, GstVideoDecoder,
    GST_TYPE_VIDEO_DECODER, DEBUG_INIT);

static void
gst_omx_video_dec_base_init (gpointer g_class)
{
}

static void
gst_omx_video_dec_class_init (GstOMXVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->finalize = gst_omx_video_dec_finalize;
  gobject_class->set_property = gst_omx_video_dec_set_property;
  gobject_class->get_property = gst_omx_video_dec_get_property;

#ifdef USE_EGL_RPI
  g_object_class_install_property (gobject_class, PROP_POOL,
      g_param_spec_boxed ("pool", "Pool", "The EGL pool",
          GST_TYPE_EGL_IMAGE_MEMORY_POOL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#endif

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_change_state);

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_omx_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_omx_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_omx_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_omx_video_dec_stop);
  video_decoder_class->reset = GST_DEBUG_FUNCPTR (gst_omx_video_dec_reset);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_handle_frame);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_omx_video_dec_finish);

  klass->cdata.type = GST_OMX_COMPONENT_TYPE_FILTER;
  klass->cdata.default_src_template_caps = "video/x-raw-yuv, "
      "width = " GST_VIDEO_SIZE_RANGE ", "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE
      "; video/x-raw-rgb, "
      "width = " GST_VIDEO_SIZE_RANGE ", "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;
}

static void
gst_omx_video_dec_init (GstOMXVideoDec * self, GstOMXVideoDecClass * klass)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);

  self->drain_lock = g_mutex_new ();
  self->drain_cond = g_cond_new ();
}

static gboolean
gst_omx_video_dec_open (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (decoder);
  GstOMXVideoDecClass *klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);
  gint in_port_index, out_port_index;

  GST_DEBUG_OBJECT (self, "Opening decoder");

  self->dec =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);
  self->started = FALSE;

  if (!self->dec)
    return FALSE;

  if (gst_omx_component_get_state (self->dec,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  in_port_index = klass->cdata.in_port_index;
  out_port_index = klass->cdata.out_port_index;

  if (in_port_index == -1 || out_port_index == -1) {
    OMX_PORT_PARAM_TYPE param;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&param);

    err =
        gst_omx_component_get_parameter (self->dec, OMX_IndexParamVideoInit,
        &param);
    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't get port information: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      /* Fallback */
      in_port_index = 0;
      out_port_index = 1;
    } else {
      GST_DEBUG_OBJECT (self, "Detected %u ports, starting at %u",
          (guint) param.nPorts, (guint) param.nStartPortNumber);
      in_port_index = param.nStartPortNumber + 0;
      out_port_index = param.nStartPortNumber + 1;
    }
  }
  self->dec_in_port = gst_omx_component_add_port (self->dec, in_port_index);
  self->dec_out_port = gst_omx_component_add_port (self->dec, out_port_index);

  if (!self->dec_in_port || !self->dec_out_port)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Opened decoder");

#ifdef USE_EGL_RPI
  GST_DEBUG_OBJECT (self, "Opening EGL renderer");
  self->egl_render =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      "OMX.broadcom.egl_render", NULL, klass->cdata.hacks);

  if (!self->egl_render)
    return FALSE;

  if (gst_omx_component_get_state (self->egl_render,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  {
    OMX_PORT_PARAM_TYPE param;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&param);

    err =
        gst_omx_component_get_parameter (self->egl_render,
        OMX_IndexParamVideoInit, &param);
    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't get port information: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      /* Fallback */
      in_port_index = 0;
      out_port_index = 1;
    } else {
      GST_DEBUG_OBJECT (self, "Detected %u ports, starting at %u", param.nPorts,
          param.nStartPortNumber);
      in_port_index = param.nStartPortNumber + 0;
      out_port_index = param.nStartPortNumber + 1;
    }
  }

  self->egl_in_port =
      gst_omx_component_add_port (self->egl_render, in_port_index);
  self->egl_out_port =
      gst_omx_component_add_port (self->egl_render, out_port_index);

  if (!self->egl_in_port || !self->egl_out_port)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Opened EGL renderer");
#endif

  return TRUE;
}

static gboolean
gst_omx_video_dec_shutdown (GstOMXVideoDec * self)
{
  OMX_STATETYPE state;

  GST_DEBUG_OBJECT (self, "Shutting down decoder");

#ifdef USE_EGL_RPI
  state = gst_omx_component_get_state (self->egl_render, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->egl_render, OMX_StateIdle);
      gst_omx_component_set_state (self->dec, OMX_StateIdle);
      gst_omx_component_get_state (self->egl_render, 5 * GST_SECOND);
      gst_omx_component_get_state (self->dec, 1 * GST_SECOND);
    }
    gst_omx_component_set_state (self->egl_render, OMX_StateLoaded);
    gst_omx_component_set_state (self->dec, OMX_StateLoaded);

    gst_omx_port_deallocate_buffers (self->dec_in_port);
    gst_omx_video_dec_deallocate_output_buffers (self);
    gst_omx_component_close_tunnel (self->dec, self->dec_out_port,
        self->egl_render, self->egl_in_port);
    if (state > OMX_StateLoaded) {
      gst_omx_component_get_state (self->egl_render, 5 * GST_SECOND);
      gst_omx_component_get_state (self->dec, 1 * GST_SECOND);
    }
  }

  /* Otherwise we didn't use EGL and just fall back to 
   * shutting down the decoder */
#endif

  state = gst_omx_component_get_state (self->dec, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->dec, OMX_StateIdle);
      gst_omx_component_get_state (self->dec, 5 * GST_SECOND);
    }
    gst_omx_component_set_state (self->dec, OMX_StateLoaded);
    gst_omx_port_deallocate_buffers (self->dec_in_port);
    gst_omx_video_dec_deallocate_output_buffers (self);
    if (state > OMX_StateLoaded)
      gst_omx_component_get_state (self->dec, 5 * GST_SECOND);
  }

  return TRUE;
}

static gboolean
gst_omx_video_dec_close (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing decoder");

  if (!gst_omx_video_dec_shutdown (self))
    return FALSE;

  self->dec_in_port = NULL;
  self->dec_out_port = NULL;
  if (self->dec)
    gst_omx_component_free (self->dec);
  self->dec = NULL;

#ifdef USE_EGL_RPI
  self->egl_in_port = NULL;
  self->egl_out_port = NULL;
  if (self->egl_render)
    gst_omx_component_free (self->egl_render);
  self->egl_render = NULL;

  if (self->egl_display)
    gst_egl_display_unref (self->egl_display);

  self->egl_display = NULL;
#endif

  self->started = FALSE;

  GST_DEBUG_OBJECT (self, "Closed decoder");

  return TRUE;
}

static void
gst_omx_video_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (object);

  switch (prop_id) {
#ifdef USE_EGL_RPI
    case PROP_POOL:
      if (self->out_port_pool) {
        gst_egl_image_memory_pool_unref (self->out_port_pool);
      }
      self->out_port_pool = g_value_get_boxed (value);
      break;
#endif
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_video_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (object);

  switch (prop_id) {
#ifdef USE_EGL_RPI
    case PROP_POOL:
      g_value_set_boxed (value, self->out_port_pool);
      break;
#endif
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_video_dec_finalize (GObject * object)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (object);

  g_mutex_free (self->drain_lock);
  g_cond_free (self->drain_cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_omx_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstOMXVideoDec *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  g_return_val_if_fail (GST_IS_OMX_VIDEO_DEC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_OMX_VIDEO_DEC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;
      self->draining = FALSE;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->dec_in_port)
        gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
      if (self->dec_out_port)
        gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);
#ifdef USE_EGL_RPI
      if (self->egl_in_port)
        gst_omx_port_set_flushing (self->egl_in_port, 5 * GST_SECOND, TRUE);
      if (self->egl_out_port)
        gst_omx_port_set_flushing (self->egl_out_port, 5 * GST_SECOND, TRUE);
#endif

      g_mutex_lock (self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (self->drain_cond);
      g_mutex_unlock (self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_WRONG_STATE;
      self->started = FALSE;

      if (!gst_omx_video_dec_shutdown (self))
        ret = GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

#define MAX_FRAME_DIST_TICKS  (5 * OMX_TICKS_PER_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstVideoCodecFrame *
_find_nearest_frame (GstOMXVideoDec * self, GstOMXBuffer * buf)
{
  GList *l, *best_l = NULL;
  GList *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GList *frames;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));

  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *tmp = l->data;
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > buf->omx_buf->nTimeStamp)
      diff = timestamp - buf->omx_buf->nTimeStamp;
    else
      diff = buf->omx_buf->nTimeStamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_l = l;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((buf->omx_buf->nTimeStamp == 0 && timestamp == 0) || diff == 0)
        break;
    }
  }

  if (FALSE && best_id) {
    for (l = frames; l && l != best_l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_ticks, diff_frames;

      /* This happens for frames that were just added but
       * which were not passed to the component yet. Ignore
       * them here!
       */
      if (!id)
        continue;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_ticks = 0;
      else
        diff_ticks = best_timestamp - id->timestamp;
      diff_frames = best->system_frame_number - tmp->system_frame_number;

      if (diff_ticks > MAX_FRAME_DIST_TICKS
          || diff_frames > MAX_FRAME_DIST_FRAMES) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }

  if (FALSE && finish_frames) {
    g_warning ("Too old frames, bug in decoder -- please file a bug");
    for (l = finish_frames; l; l = l->next) {
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), l->data);
    }
  }

  if (best)
    gst_video_codec_frame_ref (best);

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}

static gboolean
gst_omx_video_dec_fill_buffer (GstOMXVideoDec * self,
    GstOMXBuffer * inbuf, GstBuffer * outbuf)
{
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  GstVideoInfo *vinfo = &state->info;
  OMX_PARAM_PORTDEFINITIONTYPE *port_def = &self->dec_out_port->port_def;
  gboolean ret = FALSE;

  if (vinfo->width != port_def->format.video.nFrameWidth ||
      vinfo->height != port_def->format.video.nFrameHeight) {
    GST_ERROR_OBJECT (self, "Resolution do not match: port=%ux%u vinfo=%dx%d",
        (guint) port_def->format.video.nFrameWidth,
        (guint) port_def->format.video.nFrameHeight,
        vinfo->width, vinfo->height);
    goto done;
  }

  /* Same strides and everything */
  if (GST_BUFFER_SIZE (outbuf) == inbuf->omx_buf->nFilledLen) {
    memcpy (GST_BUFFER_DATA (outbuf),
        inbuf->omx_buf->pBuffer + inbuf->omx_buf->nOffset,
        inbuf->omx_buf->nFilledLen);
    ret = TRUE;
    goto done;
  }

  /* Different strides */

  switch (vinfo->finfo->format) {
    case GST_VIDEO_FORMAT_I420:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      for (i = 0; i < 3; i++) {
        if (i == 0) {
          src_stride = port_def->format.video.nStride;
          dest_stride =
              gst_video_format_get_row_stride (vinfo->finfo->format, 0,
              vinfo->width);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        } else {
          src_stride = port_def->format.video.nStride / 2;
          dest_stride =
              gst_video_format_get_row_stride (vinfo->finfo->format, 1,
              vinfo->width);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        }

        src = inbuf->omx_buf->pBuffer + inbuf->omx_buf->nOffset;
        if (i > 0)
          src +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;
        if (i == 2)
          src +=
              (port_def->format.video.nSliceHeight / 2) *
              (port_def->format.video.nStride / 2);

        dest =
            GST_BUFFER_DATA (outbuf) +
            gst_video_format_get_component_offset (vinfo->finfo->format, i,
            vinfo->width, vinfo->height);

        height =
            gst_video_format_get_component_height (vinfo->finfo->format, i,
            vinfo->height);

        width =
            gst_video_format_get_component_width (vinfo->finfo->format, i,
            vinfo->width);

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          src += src_stride;
          dest += dest_stride;
        }
      }
      ret = TRUE;
      break;
    }
    case GST_VIDEO_FORMAT_NV12:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      for (i = 0; i < 2; i++) {
        if (i == 0) {
          src_stride = port_def->format.video.nStride;
          dest_stride =
              gst_video_format_get_row_stride (vinfo->finfo->format, 0,
              vinfo->width);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        } else {
          src_stride = port_def->format.video.nStride;
          dest_stride =
              gst_video_format_get_row_stride (vinfo->finfo->format, 1,
              vinfo->width);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        }

        src = inbuf->omx_buf->pBuffer + inbuf->omx_buf->nOffset;
        if (i == 1)
          src +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;

        dest =
            GST_BUFFER_DATA (outbuf) +
            gst_video_format_get_component_offset (vinfo->finfo->format, i,
            vinfo->width, vinfo->height);

        height =
            gst_video_format_get_component_height (vinfo->finfo->format, i,
            vinfo->height);

        width =
            gst_video_format_get_component_width (vinfo->finfo->format, i,
            vinfo->width);

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          src += src_stride;
          dest += dest_stride;
        }
      }
      ret = TRUE;
      break;
    }
    default:
      GST_ERROR_OBJECT (self, "Unsupported format");
      goto done;
      break;
  }


done:
  if (ret) {
    GST_BUFFER_TIMESTAMP (outbuf) =
        gst_util_uint64_scale (inbuf->omx_buf->nTimeStamp, GST_SECOND,
        OMX_TICKS_PER_SECOND);
    if (inbuf->omx_buf->nTickCount != 0)
      GST_BUFFER_DURATION (outbuf) =
          gst_util_uint64_scale (inbuf->omx_buf->nTickCount, GST_SECOND,
          OMX_TICKS_PER_SECOND);
  }

  gst_video_codec_state_unref (state);

  return ret;
}

static OMX_ERRORTYPE
gst_omx_video_dec_allocate_output_buffers (GstOMXVideoDec * self)
{
  OMX_ERRORTYPE err = OMX_ErrorNone;
  GstOMXPort *port;
  guint min = 1;
  gboolean was_enabled = TRUE;
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  GstVideoInfo *vinfo = &state->info;

#ifdef USE_EGL_RPI
  if (self->egl_transport_active) {
    GList *images = NULL;

    port = self->egl_out_port;
    min = N_EGL_IMAGES;

    if (!self->out_port_pool) {
      gst_element_post_message (GST_ELEMENT_CAST (self),
          gst_message_new_need_egl_pool (GST_OBJECT (self), min,
              vinfo->width, vinfo->height));
      if (!self->out_port_pool) {
        err = OMX_ErrorUndefined;
        goto done;
      }
    }

    if (self->egl_display) {
      gst_egl_display_unref (self->egl_display);
    }
    self->egl_display =
        gst_egl_image_memory_pool_get_display (self->out_port_pool);
    images = gst_egl_image_memory_pool_get_images (self->out_port_pool);

    if (images) {
      GST_DEBUG_OBJECT (self, "Setting EGLDisplay");
      self->egl_out_port->port_def.format.video.pNativeWindow =
          gst_egl_display_get (self->egl_display);
      err =
          gst_omx_port_update_port_definition (self->egl_out_port,
          &self->egl_out_port->port_def);

      if (err != OMX_ErrorNone) {
        GST_INFO_OBJECT (self,
            "Failed to set EGLDisplay on port: %s (0x%08x)",
            gst_omx_error_to_string (err), err);
        g_list_free (images);
        /* TODO: For non-RPi targets we want to use the normal memory code below */
        /* Retry without EGLImage */
        goto done;
      } else {
        if (min != port->port_def.nBufferCountActual) {
          err = gst_omx_port_update_port_definition (port, NULL);
          if (err == OMX_ErrorNone) {
            port->port_def.nBufferCountActual = min;
            err = gst_omx_port_update_port_definition (port, &port->port_def);
          }

          if (err != OMX_ErrorNone) {
            GST_INFO_OBJECT (self,
                "Failed to configure %u output buffers: %s (0x%08x)", min,
                gst_omx_error_to_string (err), err);
            g_list_free (images);
            /* TODO: For non-RPi targets we want to use the normal memory code below */
            /* Retry without EGLImage */

            goto done;
          }
        }

        if (!gst_omx_port_is_enabled (port)) {
          err = gst_omx_port_set_enabled (port, TRUE);
          if (err != OMX_ErrorNone) {
            GST_INFO_OBJECT (self,
                "Failed to enable port: %s (0x%08x)",
                gst_omx_error_to_string (err), err);
            g_list_free (images);
            /* TODO: For non-RPi targets we want to use the normal memory code below */
            /* Retry without EGLImage */
            goto done;
          }
        }

        err = gst_omx_port_use_eglimages (port, images);
        g_list_free (images);

        if (err != OMX_ErrorNone) {
          GST_INFO_OBJECT (self,
              "Failed to pass EGLImages to port: %s (0x%08x)",
              gst_omx_error_to_string (err), err);
          /* TODO: For non-RPi targets we want to use the normal memory code below */
          /* Retry without EGLImage */
          goto done;
        }

        err = gst_omx_port_wait_enabled (port, 2 * GST_SECOND);
        if (err != OMX_ErrorNone) {
          GST_INFO_OBJECT (self,
              "Failed to wait until port is enabled: %s (0x%08x)",
              gst_omx_error_to_string (err), err);
          /* TODO: For non-RPi targets we want to use the normal memory code below */
          /* Retry without EGLImage */
          goto done;
        }
        GST_DEBUG_OBJECT (self, "Activate EGL memory pool");
        gst_egl_image_memory_pool_set_active (self->out_port_pool, TRUE);
      }
      /* All good and done */
      err = OMX_ErrorNone;
      goto done;
    }
  } else {
    port = self->dec_out_port;
  }
#else
  port = self->dec_out_port;
#endif

  min = MAX (min, port->port_def.nBufferCountMin);

  if (min != port->port_def.nBufferCountActual) {
    err = gst_omx_port_update_port_definition (port, NULL);
    if (err == OMX_ErrorNone) {
      port->port_def.nBufferCountActual = min;
      err = gst_omx_port_update_port_definition (port, &port->port_def);
    }

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (self,
          "Failed to configure %u output buffers: %s (0x%08x)", min,
          gst_omx_error_to_string (err), err);
      goto done;
    }
    GST_DEBUG_OBJECT (self, "Configured %u output buffers", min);
  }

  if (!gst_omx_port_is_enabled (port)) {
    err = gst_omx_port_set_enabled (port, TRUE);
    if (err != OMX_ErrorNone) {
      GST_INFO_OBJECT (self,
          "Failed to enable port: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      goto done;
    }
    was_enabled = FALSE;
  }

  err = gst_omx_port_allocate_buffers (port);
  if (err != OMX_ErrorNone && min > port->port_def.nBufferCountMin) {
    GST_ERROR_OBJECT (self,
        "Failed to allocate required number of buffers %d, trying less and copying",
        min);
    min = port->port_def.nBufferCountMin;

    if (!was_enabled)
      gst_omx_port_set_enabled (port, FALSE);

    if (min != port->port_def.nBufferCountActual) {
      err = gst_omx_port_update_port_definition (port, NULL);
      if (err == OMX_ErrorNone) {
        port->port_def.nBufferCountActual = min;
        err = gst_omx_port_update_port_definition (port, &port->port_def);
      }

      if (err != OMX_ErrorNone) {
        GST_ERROR_OBJECT (self,
            "Failed to configure %u output buffers: %s (0x%08x)", min,
            gst_omx_error_to_string (err), err);
        goto done;
      }
    }

    err = gst_omx_port_allocate_buffers (port);
  }

  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to allocate %d buffers: %s (0x%08x)", min,
        gst_omx_error_to_string (err), err);
    goto done;
  }

  if (!was_enabled) {
    err = gst_omx_port_wait_enabled (port, 2 * GST_SECOND);
    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (self,
          "Failed to wait until port is enabled: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      goto done;
    }
  }

  err = OMX_ErrorNone;

done:
  gst_video_codec_state_unref (state);
  return err;
}

static OMX_ERRORTYPE
gst_omx_video_dec_deallocate_output_buffers (GstOMXVideoDec * self)
{
  OMX_ERRORTYPE err;
  GstOMXPort *port = self->dec_out_port;

#ifdef USE_EGL_RPI
  if (self->out_port_pool) {
    gst_egl_image_memory_pool_set_active (self->out_port_pool, FALSE);
    gst_egl_image_memory_pool_wait_released (self->out_port_pool);
    gst_egl_image_memory_pool_unref (self->out_port_pool);
    self->out_port_pool = NULL;
  }
  if (self->egl_transport_active) {
    port = self->egl_out_port;
  }
#endif

  err = gst_omx_port_deallocate_buffers (port);

  return err;
}

static OMX_ERRORTYPE
gst_omx_video_dec_reconfigure_output_port (GstOMXVideoDec * self)
{
  GstOMXPort *port;
  OMX_ERRORTYPE err;
  GstVideoCodecState *state;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  GstVideoFormat format;

  /* At this point the decoder output port is disabled */

#ifdef USE_EGL_RPI
  if (!self->egl_transport_allowed) {
    goto no_egl;
  } else {
    OMX_STATETYPE egl_state;

    if (self->egl_transport_active) {
      /* Nothing to do here, we could however fall back to non-EGLImage in theory */
      port = self->egl_out_port;
      err = OMX_ErrorNone;
      goto enable_port;
    } else {
      /* Set up egl_render */
      self->egl_transport_active = TRUE;
      gst_omx_port_get_port_definition (self->dec_out_port, &port_def);
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
          GST_VIDEO_FORMAT_RGBA, port_def.format.video.nFrameWidth,
          port_def.format.video.nFrameHeight, self->input_state);

      if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
        gst_video_codec_state_unref (state);
        GST_ERROR_OBJECT (self, "Failed to negotiate RGBA for EGLImage");
        GST_VIDEO_DECODER_STREAM_UNLOCK (self);
        goto no_egl;
      }

      gst_video_codec_state_unref (state);
      GST_VIDEO_DECODER_STREAM_UNLOCK (self);

      /* Now link it all together */

      err = gst_omx_port_set_enabled (self->egl_in_port, FALSE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_wait_enabled (self->egl_in_port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_set_enabled (self->egl_out_port, FALSE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_wait_enabled (self->egl_out_port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto no_egl;

      {
#define OMX_IndexParamBrcmVideoEGLRenderDiscardMode 0x7f0000db
        OMX_CONFIG_PORTBOOLEANTYPE discardMode;
        memset (&discardMode, 0, sizeof (discardMode));
        discardMode.nSize = sizeof (discardMode);
        discardMode.nPortIndex = 220;
        discardMode.nVersion.nVersion = OMX_VERSION;
        discardMode.bEnabled = OMX_FALSE;
        if (gst_omx_component_set_parameter (self->egl_render,
                OMX_IndexParamBrcmVideoEGLRenderDiscardMode,
                &discardMode) != OMX_ErrorNone)
          goto no_egl;
#undef OMX_IndexParamBrcmVideoEGLRenderDiscardMode
      }

      err =
          gst_omx_component_setup_tunnel (self->dec, self->dec_out_port,
          self->egl_render, self->egl_in_port);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_set_enabled (self->egl_in_port, TRUE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_component_set_state (self->egl_render, OMX_StateIdle);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_wait_enabled (self->egl_in_port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto no_egl;

      if (gst_omx_component_get_state (self->egl_render,
              GST_CLOCK_TIME_NONE) != OMX_StateIdle)
        goto no_egl;

      err = gst_omx_video_dec_allocate_output_buffers (self);
      if (err != OMX_ErrorNone)
        goto no_egl;

      if (gst_omx_component_set_state (self->egl_render,
              OMX_StateExecuting) != OMX_ErrorNone)
        goto no_egl;

      if (gst_omx_component_get_state (self->egl_render,
              GST_CLOCK_TIME_NONE) != OMX_StateExecuting)
        goto no_egl;

      err =
          gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, FALSE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err =
          gst_omx_port_set_flushing (self->egl_in_port, 5 * GST_SECOND, FALSE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err =
          gst_omx_port_set_flushing (self->egl_out_port, 5 * GST_SECOND, FALSE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_populate (self->egl_out_port);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_set_enabled (self->dec_out_port, TRUE);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_wait_enabled (self->dec_out_port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto no_egl;


      err = gst_omx_port_mark_reconfigured (self->dec_out_port);
      if (err != OMX_ErrorNone)
        goto no_egl;

      err = gst_omx_port_mark_reconfigured (self->egl_out_port);
      if (err != OMX_ErrorNone)
        goto no_egl;

      goto done;
    }

  no_egl:

    gst_omx_port_set_enabled (self->dec_out_port, FALSE);
    gst_omx_port_wait_enabled (self->dec_out_port, 1 * GST_SECOND);
    egl_state = gst_omx_component_get_state (self->egl_render, 0);
    if (egl_state > OMX_StateLoaded || egl_state == OMX_StateInvalid) {
      if (egl_state > OMX_StateIdle) {
        gst_omx_component_set_state (self->egl_render, OMX_StateIdle);
        gst_omx_component_get_state (self->egl_render, 5 * GST_SECOND);
      }
      gst_omx_component_set_state (self->egl_render, OMX_StateLoaded);

      gst_omx_video_dec_deallocate_output_buffers (self);
      gst_omx_component_close_tunnel (self->dec, self->dec_out_port,
          self->egl_render, self->egl_in_port);

      if (egl_state > OMX_StateLoaded) {
        gst_omx_component_get_state (self->egl_render, 5 * GST_SECOND);
      }
    }

    /* After this egl_render should be deactivated
     * and the decoder's output port disabled */
    self->egl_transport_active = FALSE;
  }
#endif
  port = self->dec_out_port;

  /* Update caps */
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  gst_omx_port_get_port_definition (port, &port_def);
  g_assert (port_def.format.video.eCompressionFormat == OMX_VIDEO_CodingUnused);

  switch (port_def.format.video.eColorFormat) {
    case OMX_COLOR_FormatYUV420Planar:
    case OMX_COLOR_FormatYUV420PackedPlanar:
      GST_DEBUG_OBJECT (self, "Output is I420 (%d)",
          port_def.format.video.eColorFormat);
      format = GST_VIDEO_FORMAT_I420;
      break;
    case OMX_COLOR_FormatYUV420SemiPlanar:
      GST_DEBUG_OBJECT (self, "Output is NV12 (%d)",
          port_def.format.video.eColorFormat);
      format = GST_VIDEO_FORMAT_NV12;
      break;
    default:
      GST_ERROR_OBJECT (self, "Unsupported color format: %d",
          port_def.format.video.eColorFormat);
      GST_VIDEO_DECODER_STREAM_UNLOCK (self);
      err = OMX_ErrorUndefined;
      goto done;
      break;
  }

  GST_DEBUG_OBJECT (self,
      "Setting output state: format %s, width %u, height %u",
      gst_video_format_to_string (format),
      (guint) port_def.format.video.nFrameWidth,
      (guint) port_def.format.video.nFrameHeight);

  state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      format, port_def.format.video.nFrameWidth,
      port_def.format.video.nFrameHeight, self->input_state);

  if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
    gst_video_codec_state_unref (state);
    GST_ERROR_OBJECT (self, "Failed to negotiate");
    err = OMX_ErrorUndefined;
    goto done;
  }

  gst_video_codec_state_unref (state);

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

#ifdef USE_EGL_RPI
enable_port:
#endif
  err = gst_omx_video_dec_allocate_output_buffers (self);
  if (err != OMX_ErrorNone)
    goto done;

  err = gst_omx_port_populate (port);
  if (err != OMX_ErrorNone)
    goto done;

  err = gst_omx_port_mark_reconfigured (port);
  if (err != OMX_ErrorNone)
    goto done;

done:

  return err;
}

static void
gst_omx_video_dec_release_egl_buffer (GstOMXBuffer * buf)
{
  g_return_if_fail (buf != NULL);

  gst_omx_port_release_buffer (buf->port, buf);
}

static void
gst_omx_video_dec_loop (GstOMXVideoDec * self)
{
  GstOMXPort *port;
  GstOMXBuffer *buf = NULL;
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstOMXAcquireBufferReturn acq_return;
  GstClockTimeDiff deadline;
  OMX_ERRORTYPE err;

#ifdef USE_EGL_RPI
  port = self->egl_transport_active ? self->egl_out_port : self->dec_out_port;
#else
  port = self->dec_out_port;
#endif

  acq_return = gst_omx_port_acquire_buffer (port, &buf);
  if (acq_return == GST_OMX_ACQUIRE_BUFFER_ERROR) {
    goto component_error;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
    goto flushing;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_EOS) {
    goto eos;
  }

  if (!GST_PAD_CAPS (GST_VIDEO_DECODER_SRC_PAD (self)) ||
      acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
    GstVideoCodecState *state;
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    GstVideoFormat format;

    GST_DEBUG_OBJECT (self, "Port settings have changed, updating caps");

    /* Reallocate all buffers */
    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE
        && gst_omx_port_is_enabled (port)) {
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_video_dec_deallocate_output_buffers (self);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    }

    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      /* We have the possibility to reconfigure everything now */
      err = gst_omx_video_dec_reconfigure_output_port (self);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    } else {
      /* Just update caps */
      GST_VIDEO_DECODER_STREAM_LOCK (self);

      gst_omx_port_get_port_definition (port, &port_def);
      g_assert (port_def.format.video.eCompressionFormat ==
          OMX_VIDEO_CodingUnused);

      switch (port_def.format.video.eColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420PackedPlanar:
          GST_DEBUG_OBJECT (self, "Output is I420 (%d)",
              port_def.format.video.eColorFormat);
          format = GST_VIDEO_FORMAT_I420;
          break;
        case OMX_COLOR_FormatYUV420SemiPlanar:
          GST_DEBUG_OBJECT (self, "Output is NV12 (%d)",
              port_def.format.video.eColorFormat);
          format = GST_VIDEO_FORMAT_NV12;
          break;
        default:
          GST_ERROR_OBJECT (self, "Unsupported color format: %d",
              port_def.format.video.eColorFormat);
          if (buf)
            gst_omx_port_release_buffer (port, buf);
          GST_VIDEO_DECODER_STREAM_UNLOCK (self);
          goto caps_failed;
          break;
      }

      GST_DEBUG_OBJECT (self,
          "Setting output state: format %s, width %u, height %u",
          gst_video_format_to_string (format),
          (guint) port_def.format.video.nFrameWidth,
          (guint) port_def.format.video.nFrameHeight);

      state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
          format, port_def.format.video.nFrameWidth,
          port_def.format.video.nFrameHeight, self->input_state);

      /* Take framerate and pixel-aspect-ratio from sinkpad caps */

      if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
        if (buf)
          gst_omx_port_release_buffer (port, buf);
        gst_video_codec_state_unref (state);
        goto caps_failed;
      }

      gst_video_codec_state_unref (state);

      GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    }

    /* Now get a buffer */
    if (acq_return != GST_OMX_ACQUIRE_BUFFER_OK) {
      return;
    }
  }

  g_assert (acq_return == GST_OMX_ACQUIRE_BUFFER_OK);

  /* This prevents a deadlock between the srcpad stream
   * lock and the videocodec stream lock, if ::reset()
   * is called at the wrong time
   */
  if (gst_omx_port_is_flushing (port)) {
    GST_DEBUG_OBJECT (self, "Flushing");
    gst_omx_port_release_buffer (port, buf);
    goto flushing;
  }

  GST_DEBUG_OBJECT (self, "Handling buffer: 0x%08x %" G_GUINT64_FORMAT,
      (guint) buf->omx_buf->nFlags, (guint64) buf->omx_buf->nTimeStamp);

  GST_VIDEO_DECODER_STREAM_LOCK (self);
  frame = _find_nearest_frame (self, buf);

  if (frame
      && (deadline = gst_video_decoder_get_max_decode_time
          (GST_VIDEO_DECODER (self), frame)) < 0) {
    GST_WARNING_OBJECT (self,
        "Frame is too late, dropping (deadline %" GST_TIME_FORMAT ")",
        GST_TIME_ARGS (-deadline));
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    frame = NULL;
  } else if (!frame && (buf->omx_buf->nFilledLen > 0 || buf->eglimage > -1)) {
    GstBuffer *outbuf;

    /* This sometimes happens at EOS or if the input is not properly framed,
     * let's handle it gracefully by allocating a new buffer for the current
     * caps and filling it
     */

    GST_ERROR_OBJECT (self, "No corresponding frame found");

    if (self->out_port_pool) {
      outbuf = gst_egl_image_memory_pool_acquire_buffer (self->out_port_pool,
          buf->eglimage, buf,
          (GDestroyNotify) gst_omx_video_dec_release_egl_buffer);
      if (!outbuf) {
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
      buf = NULL;
    } else {
      outbuf = gst_video_decoder_alloc_output_buffer (GST_VIDEO_DECODER (self));
      if (!gst_omx_video_dec_fill_buffer (self, buf, outbuf)) {
        gst_buffer_unref (outbuf);
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
    }

    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
  } else if (buf->omx_buf->nFilledLen > 0 || buf->eglimage > -1) {
    if (self->out_port_pool) {
      frame->output_buffer =
          gst_egl_image_memory_pool_acquire_buffer (self->out_port_pool,
          buf->eglimage, buf,
          (GDestroyNotify) gst_omx_video_dec_release_egl_buffer);
      if (!frame->output_buffer) {
        flow_ret =
            gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
        frame = NULL;
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
      flow_ret =
          gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
      frame = NULL;
      buf = NULL;
    } else {
      if ((flow_ret =
              gst_video_decoder_alloc_output_frame (GST_VIDEO_DECODER
                  (self), frame)) == GST_FLOW_OK) {
        /* FIXME: This currently happens because of a race condition too.
         * We first need to reconfigure the output port and then the input
         * port if both need reconfiguration.
         */
        if (!gst_omx_video_dec_fill_buffer (self, buf, frame->output_buffer)) {
          gst_buffer_replace (&frame->output_buffer, NULL);
          flow_ret =
              gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
          frame = NULL;
          gst_omx_port_release_buffer (port, buf);
          goto invalid_buffer;
        }
        flow_ret =
            gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
        frame = NULL;
      }
    }
  } else if (frame != NULL) {
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    frame = NULL;
  }

  GST_DEBUG_OBJECT (self, "Read frame from component");

  GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));

  if (buf) {
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;
  }

  self->downstream_flow_ret = flow_ret;

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  return;

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->dec),
            gst_omx_component_get_last_error (self->dec)));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    return;
  }

flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_WRONG_STATE;
    self->started = FALSE;
    return;
  }

eos:
  {
    g_mutex_lock (self->drain_lock);
    if (self->draining) {
#ifdef USE_EGL_RPI
      if (self->egl_transport_active) {
        GstBuffer *reclaim = gst_buffer_new ();
        GST_BUFFER_FLAG_SET (reclaim, GST_BUFFER_FLAG_PREROLL);
        GST_DEBUG_OBJECT (self, "pushing a reclaim buffer");
        flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), reclaim);
      }
#endif
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (self->drain_cond);
      flow_ret = GST_FLOW_OK;
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_UNEXPECTED;
    }
    g_mutex_unlock (self->drain_lock);

    GST_VIDEO_DECODER_STREAM_LOCK (self);
    self->downstream_flow_ret = flow_ret;

    /* Here we fallback and pause the task for the EOS case */
    if (flow_ret != GST_FLOW_OK)
      goto flow_error;

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);

    return;
  }

flow_error:
  {
    if (flow_ret == GST_FLOW_UNEXPECTED) {
      GST_DEBUG_OBJECT (self, "EOS");

      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_NOT_LINKED
        || flow_ret < GST_FLOW_UNEXPECTED) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Internal data stream error."),
          ("stream stopped, reason %s", gst_flow_get_name (flow_ret)));

      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    }
    self->started = FALSE;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }

reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure output port"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    return;
  }

invalid_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Invalid sized input buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }

caps_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL), ("Failed to set caps"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;
    return;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase output buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
}

static gboolean
gst_omx_video_dec_start (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_omx_video_dec_stop (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping decoder");

  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);

#ifdef USE_EGL_RPI
  gst_omx_port_set_flushing (self->egl_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->egl_out_port, 5 * GST_SECOND, TRUE);
#endif

  gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));

  if (gst_omx_component_get_state (self->dec, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->dec, OMX_StateIdle);
#ifdef USE_EGL_RPI
  if (gst_omx_component_get_state (self->egl_render, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->egl_render, OMX_StateIdle);
#endif

  self->downstream_flow_ret = GST_FLOW_WRONG_STATE;
  self->started = FALSE;
  self->eos = FALSE;

  g_mutex_lock (self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (self->drain_cond);
  g_mutex_unlock (self->drain_lock);

  gst_omx_component_get_state (self->dec, 5 * GST_SECOND);
#ifdef USE_EGL_RPI
  gst_omx_component_get_state (self->egl_render, 1 * GST_SECOND);
#endif

  gst_buffer_replace (&self->codec_data, NULL);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  GST_DEBUG_OBJECT (self, "Stopped decoder");

  return TRUE;
}

typedef struct
{
  GstVideoFormat format;
  OMX_COLOR_FORMATTYPE type;
} VideoNegotiationMap;

static void
video_negotiation_map_free (VideoNegotiationMap * m)
{
  g_slice_free (VideoNegotiationMap, m);
}

static GList *
gst_omx_video_dec_get_supported_colorformats (GstOMXVideoDec * self)
{
  GstOMXComponent *comp;
  GstOMXPort *port;
  GstVideoCodecState *state = self->input_state;
  OMX_VIDEO_PARAM_PORTFORMATTYPE param;
  OMX_ERRORTYPE err;
  GList *negotiation_map = NULL;
  gint old_index;
  VideoNegotiationMap *m;

  port = self->dec_out_port;
  comp = self->dec;

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = port->index;
  param.nIndex = 0;
  if (!state || state->info.fps_n == 0)
    param.xFramerate = 0;
  else
    param.xFramerate = (state->info.fps_n << 16) / (state->info.fps_d);

  old_index = -1;
  do {
    err =
        gst_omx_component_get_parameter (comp,
        OMX_IndexParamVideoPortFormat, &param);

    /* FIXME: Workaround for Bellagio that simply always
     * returns the same value regardless of nIndex and
     * never returns OMX_ErrorNoMore
     */
    if (old_index == param.nIndex)
      break;

    if (err == OMX_ErrorNone || err == OMX_ErrorNoMore) {
      switch (param.eColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420PackedPlanar:
          m = g_slice_new (VideoNegotiationMap);
          m->format = GST_VIDEO_FORMAT_I420;
          m->type = param.eColorFormat;
          negotiation_map = g_list_append (negotiation_map, m);
          GST_DEBUG_OBJECT (self, "Component supports I420 (%d) at index %u",
              param.eColorFormat, (guint) param.nIndex);
          break;
        case OMX_COLOR_FormatYUV420SemiPlanar:
          m = g_slice_new (VideoNegotiationMap);
          m->format = GST_VIDEO_FORMAT_NV12;
          m->type = param.eColorFormat;
          negotiation_map = g_list_append (negotiation_map, m);
          GST_DEBUG_OBJECT (self, "Component supports NV12 (%d) at index %u",
              param.eColorFormat, (guint) param.nIndex);
          break;
        default:
          GST_DEBUG_OBJECT (self,
              "Component supports unsupported color format %d at index %u",
              param.eColorFormat, (guint) param.nIndex);
          break;
      }
    }
    old_index = param.nIndex++;
  } while (err == OMX_ErrorNone);

  return negotiation_map;
}

static gboolean
gst_omx_video_dec_negotiate (GstOMXVideoDec * self, GstVideoInfo * info)
{
  OMX_VIDEO_PARAM_PORTFORMATTYPE param;
  OMX_ERRORTYPE err;
  GstCaps *comp_supported_caps;
  GList *negotiation_map = NULL, *l;
  GstCaps *templ_caps, *peer_caps, *intersection;
  GstVideoFormat format;
  GstStructure *s;
  guint32 fourcc;
#ifdef USE_EGL_RPI
  gboolean peer_caps_are_any = FALSE;
#endif

  GST_DEBUG_OBJECT (self, "Trying to negotiate a video format with downstream");

  templ_caps =
      gst_caps_copy (gst_pad_get_pad_template_caps (GST_VIDEO_DECODER_SRC_PAD
          (self)));
  peer_caps = gst_pad_peer_get_caps (GST_VIDEO_DECODER_SRC_PAD (self));
  if (peer_caps) {
#ifdef USE_EGL_RPI
    peer_caps_are_any = gst_caps_is_any (peer_caps);
#endif
    intersection = gst_caps_intersect (templ_caps, peer_caps);
    gst_caps_unref (templ_caps);
    gst_caps_unref (peer_caps);
  } else {
    intersection = templ_caps;
  }

  comp_supported_caps = gst_caps_new_empty ();

#ifdef USE_EGL_RPI
  /* Hack to allow negotiate EGL transport in playbin2 */
  if (peer_caps_are_any) {
    if (!self->out_port_pool) {
      /* Ask for an EGL pool */
      gst_element_post_message (GST_ELEMENT_CAST (self),
          gst_message_new_need_egl_pool (GST_OBJECT (self), N_EGL_IMAGES,
              info->width, info->height));
      /* If appliction provided one then allow EGL transport */
      if (self->out_port_pool) {
        self->egl_transport_allowed = TRUE;
        gst_caps_append_structure (comp_supported_caps,
            gst_structure_new ("video/x-raw-rgb",
                "transport", G_TYPE_STRING, "egl", NULL));
      }
    }
  } else {
    gboolean egl_transport = FALSE;
    GstCaps *filtered = gst_caps_new_empty ();
    gint i, nstrus = gst_caps_get_size (intersection);

    for (i = 0; i < nstrus; i++) {
      s = gst_caps_get_structure (intersection, i);
      if (gst_structure_has_name (s, "video/x-raw-rgb")) {
        const gchar *transport;
        if ((transport = gst_structure_get_string (s, "transport"))) {
          if (g_strcmp0 (transport, "egl") == 0) {
            gst_caps_append_structure (filtered, gst_structure_copy (s));
            egl_transport = TRUE;
          }
        }
      } else {
        gst_caps_append_structure (filtered, gst_structure_copy (s));
      }
    }
    gst_caps_unref (intersection);
    intersection = filtered;

    if (egl_transport) {
      self->egl_transport_allowed = TRUE;
      gst_caps_append_structure (comp_supported_caps,
          gst_structure_new ("video/x-raw-rgb",
              "transport", G_TYPE_STRING, "egl", NULL));
    }
  }
#endif

  negotiation_map = gst_omx_video_dec_get_supported_colorformats (self);
  for (l = negotiation_map; l; l = l->next) {
    VideoNegotiationMap *map = l->data;

    gst_caps_append_structure (comp_supported_caps,
        gst_structure_new ("video/x-raw-yuv",
            "format", GST_TYPE_FOURCC,
            gst_video_format_to_fourcc (map->format), NULL));
  }

  GST_DEBUG_OBJECT (self, "Allowed downstream caps: %" GST_PTR_FORMAT,
      intersection);

  if (!gst_caps_is_empty (comp_supported_caps)) {
    GstCaps *tmp;

    tmp = gst_caps_intersect (comp_supported_caps, intersection);
    gst_caps_unref (intersection);
    intersection = tmp;
  }
  gst_caps_unref (comp_supported_caps);

  if (gst_caps_is_empty (intersection)) {
    gst_caps_unref (intersection);
    GST_ERROR_OBJECT (self, "Empty caps");
    g_list_free_full (negotiation_map,
        (GDestroyNotify) video_negotiation_map_free);
    return FALSE;
  }

  gst_caps_truncate (intersection);
  gst_pad_fixate_caps (GST_VIDEO_DECODER_SRC_PAD (self), intersection);

  s = gst_caps_get_structure (intersection, 0);
#ifdef USE_EGL_RPI
  if (gst_structure_has_name (s, "video/x-raw-rgb")) {
    g_list_free_full (negotiation_map,
        (GDestroyNotify) video_negotiation_map_free);
    GST_DEBUG_OBJECT (self, "Negotiated EGL transport");
    return TRUE;
  }
#endif
  if (!gst_structure_get_fourcc (s, "format", &fourcc) ||
      (format =
          gst_video_format_from_fourcc (fourcc)) == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Invalid caps: %" GST_PTR_FORMAT, intersection);
    g_list_free_full (negotiation_map,
        (GDestroyNotify) video_negotiation_map_free);
    return FALSE;
  }

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = self->dec_out_port->index;

  for (l = negotiation_map; l; l = l->next) {
    VideoNegotiationMap *m = l->data;

    if (m->format == format) {
      param.eColorFormat = m->type;
      break;
    }
  }

  GST_DEBUG_OBJECT (self, "Negotiating color format %s (%d)",
      gst_video_format_to_string (format), param.eColorFormat);

  /* We must find something here */
  g_assert (l != NULL);
  g_list_free_full (negotiation_map,
      (GDestroyNotify) video_negotiation_map_free);

  err =
      gst_omx_component_set_parameter (self->dec,
      OMX_IndexParamVideoPortFormat, &param);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to set video port format: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
  }

  return (err == OMX_ErrorNone);
}

static gboolean
gst_omx_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstOMXVideoDec *self;
  GstOMXVideoDecClass *klass;
  GstVideoInfo *info = &state->info;
  gboolean is_format_change = FALSE;
  gboolean needs_disable = FALSE;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  self = GST_OMX_VIDEO_DEC (decoder);
  klass = GST_OMX_VIDEO_DEC_GET_CLASS (decoder);

  GST_DEBUG_OBJECT (self, "Setting new caps %" GST_PTR_FORMAT, state->caps);

  gst_omx_port_get_port_definition (self->dec_in_port, &port_def);

  /* Check if the caps change is a real format change or if only irrelevant
   * parts of the caps have changed or nothing at all.
   */
  is_format_change |= port_def.format.video.nFrameWidth != info->width;
  is_format_change |= port_def.format.video.nFrameHeight != info->height;
  is_format_change |= (port_def.format.video.xFramerate == 0
      && info->fps_n != 0)
      || (port_def.format.video.xFramerate !=
      (info->fps_n << 16) / (info->fps_d));
  is_format_change |= (self->codec_data != state->codec_data);
  if (klass->is_format_change)
    is_format_change |=
        klass->is_format_change (self, self->dec_in_port, state);

  needs_disable =
      gst_omx_component_get_state (self->dec,
      GST_CLOCK_TIME_NONE) != OMX_StateLoaded;
  /* If the component is not in Loaded state and a real format change happens
   * we have to disable the port and re-allocate all buffers. If no real
   * format change happened we can just exit here.
   */
  if (needs_disable && !is_format_change) {
    GST_DEBUG_OBJECT (self,
        "Already running and caps did not change the format");
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = gst_video_codec_state_ref (state);
    return TRUE;
  }

  if (needs_disable && is_format_change) {
#ifdef USE_EGL_RPI
    GstOMXPort *out_port =
        self->egl_transport_active ? self->egl_out_port : self->dec_out_port;
#else
    GstOMXPort *out_port = self->dec_out_port;
#endif

    GST_DEBUG_OBJECT (self, "Need to disable and drain decoder");

    gst_omx_video_dec_drain (self, FALSE);
    gst_omx_port_set_flushing (out_port, 5 * GST_SECOND, TRUE);

    /* Wait until the srcpad loop is finished,
     * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
     * caused by using this lock from inside the loop function */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    if (klass->cdata.hacks & GST_OMX_HACK_NO_COMPONENT_RECONFIGURE) {
      GST_VIDEO_DECODER_STREAM_UNLOCK (self);
      gst_omx_video_dec_stop (GST_VIDEO_DECODER (self));
      gst_omx_video_dec_close (GST_VIDEO_DECODER (self));
      GST_VIDEO_DECODER_STREAM_LOCK (self);

      if (!gst_omx_video_dec_open (GST_VIDEO_DECODER (self)))
        return FALSE;
      needs_disable = FALSE;
    } else {
#ifdef USE_EGL_RPI
      if (self->egl_transport_active) {
        gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
        gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);
        gst_omx_port_set_flushing (self->egl_in_port, 5 * GST_SECOND, TRUE);
        gst_omx_port_set_flushing (self->egl_out_port, 5 * GST_SECOND, TRUE);
      }
#endif

      if (gst_omx_port_set_enabled (self->dec_in_port, FALSE) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_set_enabled (out_port, FALSE) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_buffers_released (self->dec_in_port,
              5 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_buffers_released (out_port,
              1 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_deallocate_buffers (self->dec_in_port) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_video_dec_deallocate_output_buffers (self) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_enabled (self->dec_in_port,
              1 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_enabled (out_port, 1 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;

#ifdef USE_EGL_RPI
      if (self->egl_transport_active) {
        OMX_STATETYPE egl_state;

        GST_DEBUG_OBJECT (self, "deactivating EGL transport");
        egl_state = gst_omx_component_get_state (self->egl_render, 0);
        if (egl_state > OMX_StateLoaded || egl_state == OMX_StateInvalid) {

          if (egl_state > OMX_StateIdle) {
            gst_omx_component_set_state (self->egl_render, OMX_StateIdle);
            gst_omx_component_set_state (self->dec, OMX_StateIdle);
            egl_state = gst_omx_component_get_state (self->egl_render,
                5 * GST_SECOND);
            gst_omx_component_get_state (self->dec, 1 * GST_SECOND);
          }
          gst_omx_component_set_state (self->egl_render, OMX_StateLoaded);
          gst_omx_component_set_state (self->dec, OMX_StateLoaded);

          gst_omx_component_close_tunnel (self->dec, self->dec_out_port,
              self->egl_render, self->egl_in_port);

          if (egl_state > OMX_StateLoaded) {
            gst_omx_component_get_state (self->egl_render, 5 * GST_SECOND);
          }

          gst_omx_component_set_state (self->dec, OMX_StateIdle);

          gst_omx_component_set_state (self->dec, OMX_StateExecuting);
          gst_omx_component_get_state (self->dec, GST_CLOCK_TIME_NONE);
        }
        self->egl_transport_active = FALSE;
        GST_DEBUG_OBJECT (self, "EGL transport deactivated");
      }
#endif

    }
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    GST_DEBUG_OBJECT (self, "Decoder drained and disabled");
  }

  port_def.format.video.nFrameWidth = info->width;
  port_def.format.video.nFrameHeight = info->height;
  if (info->fps_n == 0)
    port_def.format.video.xFramerate = 0;
  else
    port_def.format.video.xFramerate = (info->fps_n << 16) / (info->fps_d);

  GST_DEBUG_OBJECT (self, "Setting inport port definition");

  if (gst_omx_port_update_port_definition (self->dec_in_port,
          &port_def) != OMX_ErrorNone)
    return FALSE;

  if (klass->set_format) {
    if (!klass->set_format (self, self->dec_in_port, state)) {
      GST_ERROR_OBJECT (self, "Subclass failed to set the new format");
      return FALSE;
    }
  }

  GST_DEBUG_OBJECT (self, "Updating outport port definition");
  if (gst_omx_port_update_port_definition (self->dec_out_port,
          NULL) != OMX_ErrorNone)
    return FALSE;

  gst_buffer_replace (&self->codec_data, state->codec_data);
  self->input_state = gst_video_codec_state_ref (state);

  GST_DEBUG_OBJECT (self, "Enabling component");

  if (needs_disable) {
    if (gst_omx_port_set_enabled (self->dec_in_port, TRUE) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_allocate_buffers (self->dec_in_port) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_enabled (self->dec_in_port,
            5 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_mark_reconfigured (self->dec_in_port) != OMX_ErrorNone)
      return FALSE;
  } else {
    if (!gst_omx_video_dec_negotiate (self, info))
      GST_LOG_OBJECT (self, "Negotiation failed, will get output format later");

    /* Disable output port */
    if (gst_omx_port_set_enabled (self->dec_out_port, FALSE) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_enabled (self->dec_out_port,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_set_state (self->dec, OMX_StateIdle) != OMX_ErrorNone)
      return FALSE;

    /* Need to allocate buffers to reach Idle state */
    if (gst_omx_port_allocate_buffers (self->dec_in_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->dec,
            GST_CLOCK_TIME_NONE) != OMX_StateIdle)
      return FALSE;

    if (gst_omx_component_set_state (self->dec,
            OMX_StateExecuting) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->dec,
            GST_CLOCK_TIME_NONE) != OMX_StateExecuting)
      return FALSE;
  }

  /* Unset flushing to allow ports to accept data again */
  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, FALSE);

  if (gst_omx_component_get_last_error (self->dec) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Component in error state: %s (0x%08x)",
        gst_omx_component_get_last_error_string (self->dec),
        gst_omx_component_get_last_error (self->dec));
    return FALSE;
  }

  /* Start the srcpad loop again */
  GST_DEBUG_OBJECT (self, "Starting task again");

  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_video_dec_loop, decoder);

  return TRUE;
}

static gboolean
gst_omx_video_dec_reset (GstVideoDecoder * decoder, gboolean hard)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  /* FIXME: Handle different values of hard */

  GST_DEBUG_OBJECT (self, "Resetting decoder");

  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);

#ifdef USE_EGL_RPI
  gst_omx_port_set_flushing (self->egl_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->egl_out_port, 5 * GST_SECOND, TRUE);
#endif

  /* Wait until the srcpad loop is finished,
   * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_populate (self->dec_out_port);

#ifdef USE_EGL_RPI
  gst_omx_port_set_flushing (self->egl_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->egl_out_port, 5 * GST_SECOND, FALSE);
#endif

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_video_dec_loop, decoder);

  GST_DEBUG_OBJECT (self, "Reset decoder");

  return TRUE;
}

static GstFlowReturn
gst_omx_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstOMXAcquireBufferReturn acq_ret = GST_OMX_ACQUIRE_BUFFER_ERROR;
  GstOMXVideoDec *self;
  GstOMXVideoDecClass *klass;
  GstOMXPort *port;
  GstOMXBuffer *buf;
  GstBuffer *codec_data = NULL;
  guint offset = 0, size;
  GstClockTime timestamp, duration;
  OMX_ERRORTYPE err;

  self = GST_OMX_VIDEO_DEC (decoder);
  klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "Handling frame");

  if (self->eos) {
    GST_WARNING_OBJECT (self, "Got frame after EOS");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_UNEXPECTED;
  }

  if (!self->started && !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame)) {
    gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    return GST_FLOW_OK;
  }

  timestamp = frame->pts;
  duration = frame->duration;

  if (self->downstream_flow_ret != GST_FLOW_OK) {
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }

  if (klass->prepare_frame) {
    GstFlowReturn ret;

    ret = klass->prepare_frame (self, frame);
    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Preparing frame failed: %s",
          gst_flow_get_name (ret));
      gst_video_codec_frame_unref (frame);
      return ret;
    }
  }

  port = self->dec_in_port;

  size = GST_BUFFER_SIZE (frame->input_buffer);
  while (offset < size) {
    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    acq_ret = gst_omx_port_acquire_buffer (port, &buf);

    if (acq_ret == GST_OMX_ACQUIRE_BUFFER_ERROR) {
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      goto component_error;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      goto flushing;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      /* Reallocate all buffers */
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_deallocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_allocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      /* Now get a new buffer and fill it */
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      continue;
    }
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    g_assert (acq_ret == GST_OMX_ACQUIRE_BUFFER_OK && buf != NULL);

    if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <= 0) {
      gst_omx_port_release_buffer (port, buf);
      goto full_buffer;
    }

    if (self->downstream_flow_ret != GST_FLOW_OK) {
      gst_omx_port_release_buffer (port, buf);
      goto flow_error;
    }

    if (self->codec_data) {
      GST_DEBUG_OBJECT (self, "Passing codec data to the component");

      codec_data = self->codec_data;

      if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <
          GST_BUFFER_SIZE (codec_data)) {
        gst_omx_port_release_buffer (port, buf);
        goto too_large_codec_data;
      }

      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_CODECCONFIG;
      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
      buf->omx_buf->nFilledLen = GST_BUFFER_SIZE (codec_data);
      memcpy (buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
          GST_BUFFER_DATA (codec_data), buf->omx_buf->nFilledLen);
      if (GST_CLOCK_TIME_IS_VALID (timestamp))
        buf->omx_buf->nTimeStamp =
            gst_util_uint64_scale (timestamp, OMX_TICKS_PER_SECOND, GST_SECOND);
      else
        buf->omx_buf->nTimeStamp = 0;
      buf->omx_buf->nTickCount = 0;

      self->started = TRUE;
      err = gst_omx_port_release_buffer (port, buf);
      gst_buffer_replace (&self->codec_data, NULL);
      if (err != OMX_ErrorNone)
        goto release_error;
      /* Acquire new buffer for the actual frame */
      continue;
    }

    /* Now handle the frame */
    GST_DEBUG_OBJECT (self, "Passing frame offset %d to the component", offset);

    /* Copy the buffer content in chunks of size as requested
     * by the port */
    buf->omx_buf->nFilledLen =
        MIN (size - offset, buf->omx_buf->nAllocLen - buf->omx_buf->nOffset);
    memcpy (buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
        GST_BUFFER_DATA (frame->input_buffer) + offset,
        buf->omx_buf->nFilledLen);

    if (timestamp != GST_CLOCK_TIME_NONE) {
      buf->omx_buf->nTimeStamp =
          gst_util_uint64_scale (timestamp, OMX_TICKS_PER_SECOND, GST_SECOND);
      self->last_upstream_ts = timestamp;
    } else {
      buf->omx_buf->nTimeStamp = 0;
    }

    if (duration != GST_CLOCK_TIME_NONE && offset == 0) {
      buf->omx_buf->nTickCount =
          gst_util_uint64_scale (buf->omx_buf->nFilledLen, duration, size);
      self->last_upstream_ts += duration;
    } else {
      buf->omx_buf->nTickCount = 0;
    }

    if (offset == 0) {
      BufferIdentification *id = g_slice_new0 (BufferIdentification);

      if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame))
        buf->omx_buf->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;

      id->timestamp = buf->omx_buf->nTimeStamp;
      gst_video_codec_frame_set_user_data (frame, id,
          (GDestroyNotify) buffer_identification_free);
    }

    /* TODO: Set flags
     *   - OMX_BUFFERFLAG_DECODEONLY for buffers that are outside
     *     the segment
     */

    offset += buf->omx_buf->nFilledLen;

    if (offset == size)
      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

    self->started = TRUE;
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;
  }

  gst_video_codec_frame_unref (frame);

  GST_DEBUG_OBJECT (self, "Passed frame to component");

  return self->downstream_flow_ret;

full_buffer:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Got OpenMAX buffer with no free space (%p, %u/%u)", buf,
            (guint) buf->omx_buf->nOffset, (guint) buf->omx_buf->nAllocLen));
    return GST_FLOW_ERROR;
  }

flow_error:
  {
    gst_video_codec_frame_unref (frame);

    return self->downstream_flow_ret;
  }

too_large_codec_data:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("codec_data larger than supported by OpenMAX port (%zu > %zu)",
            GST_BUFFER_SIZE (codec_data),
            (guint) self->dec_in_port->port_def.nBufferSize));
    return GST_FLOW_ERROR;
  }

component_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->dec),
            gst_omx_component_get_last_error (self->dec)));
    return GST_FLOW_ERROR;
  }

flushing:
  {
    gst_video_codec_frame_unref (frame);
    GST_DEBUG_OBJECT (self, "Flushing -- returning WRONG_STATE");
    return GST_FLOW_WRONG_STATE;
  }
reconfigure_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure input port"));
    return GST_FLOW_ERROR;
  }
release_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase input buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_omx_video_dec_finish (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  return gst_omx_video_dec_drain (self, TRUE);
}

static GstFlowReturn
gst_omx_video_dec_drain (GstOMXVideoDec * self, gboolean is_eos)
{
  GstOMXVideoDecClass *klass;
  GstOMXBuffer *buf;
  GstOMXAcquireBufferReturn acq_ret;
  OMX_ERRORTYPE err;

  GST_DEBUG_OBJECT (self, "Draining component");

  klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Component not started yet");
    return GST_FLOW_OK;
  }
  self->started = FALSE;

  /* Don't send EOS buffer twice, this doesn't work */
  if (self->eos) {
    GST_DEBUG_OBJECT (self, "Component is EOS already");
    return GST_FLOW_OK;
  }
  if (is_eos)
    self->eos = TRUE;

  if ((klass->cdata.hacks & GST_OMX_HACK_NO_EMPTY_EOS_BUFFER)) {
    GST_WARNING_OBJECT (self, "Component does not support empty EOS buffers");
    return GST_FLOW_OK;
  }

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port. */
  acq_ret = gst_omx_port_acquire_buffer (self->dec_in_port, &buf);
  if (acq_ret != GST_OMX_ACQUIRE_BUFFER_OK) {
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for draining: %d",
        acq_ret);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (self->drain_lock);
  self->draining = TRUE;
  buf->omx_buf->nFilledLen = 0;
  buf->omx_buf->nTimeStamp =
      gst_util_uint64_scale (self->last_upstream_ts, OMX_TICKS_PER_SECOND,
      GST_SECOND);
  buf->omx_buf->nTickCount = 0;
  buf->omx_buf->nFlags |= OMX_BUFFERFLAG_EOS;
  err = gst_omx_port_release_buffer (self->dec_in_port, buf);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to drain component: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (self, "Waiting until component is drained");

  if (G_UNLIKELY (self->dec->hacks & GST_OMX_HACK_DRAIN_MAY_NOT_RETURN)) {
    GTimeVal timeval;

    g_get_current_time (&timeval);
    g_time_val_add (&timeval, (G_USEC_PER_SEC / 2));

    if (!g_cond_timed_wait (self->drain_cond, self->drain_lock, &timeval))
      GST_WARNING_OBJECT (self, "Drain timed out");
    else
      GST_DEBUG_OBJECT (self, "Drained component");

  } else {
    g_cond_wait (self->drain_cond, self->drain_lock);
    GST_DEBUG_OBJECT (self, "Drained component");
  }

  g_mutex_unlock (self->drain_lock);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  self->started = FALSE;

  return GST_FLOW_OK;
}
