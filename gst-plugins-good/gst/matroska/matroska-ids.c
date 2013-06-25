/* GStreamer Matroska muxer/demuxer
 * (C) 2003 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * (C) 2006 Tim-Philipp Müller <tim centricular net>
 *
 * matroska-ids.c: matroska track context utility functions
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "matroska-ids.h"

#include <string.h>

gboolean
gst_matroska_track_init_video_context (GstMatroskaTrackContext ** p_context)
{
  GstMatroskaTrackVideoContext *video_context;

  g_assert (p_context != NULL && *p_context != NULL);

  /* already set up? (track info might come before track type) */
  if ((*p_context)->type == GST_MATROSKA_TRACK_TYPE_VIDEO) {
    GST_LOG ("video context already set up");
    return TRUE;
  }

  /* it better not have been set up as some other track type ... */
  if ((*p_context)->type != 0) {
    g_return_val_if_reached (FALSE);
  }

  video_context = g_renew (GstMatroskaTrackVideoContext, *p_context, 1);
  *p_context = (GstMatroskaTrackContext *) video_context;

  /* defaults */
  (*p_context)->type = GST_MATROSKA_TRACK_TYPE_VIDEO;
  video_context->display_width = 0;
  video_context->display_height = 0;
  video_context->pixel_width = 0;
  video_context->pixel_height = 0;
  video_context->asr_mode = 0;
  video_context->fourcc = 0;
  video_context->default_fps = 0.0;
  video_context->earliest_time = GST_CLOCK_TIME_NONE;
  return TRUE;
}

gboolean
gst_matroska_track_init_audio_context (GstMatroskaTrackContext ** p_context)
{
  GstMatroskaTrackAudioContext *audio_context;

  g_assert (p_context != NULL && *p_context != NULL);

  /* already set up? (track info might come before track type) */
  if ((*p_context)->type == GST_MATROSKA_TRACK_TYPE_AUDIO)
    return TRUE;

  /* it better not have been set up as some other track type ... */
  if ((*p_context)->type != 0) {
    g_return_val_if_reached (FALSE);
  }

  audio_context = g_renew (GstMatroskaTrackAudioContext, *p_context, 1);
  *p_context = (GstMatroskaTrackContext *) audio_context;

  /* defaults */
  (*p_context)->type = GST_MATROSKA_TRACK_TYPE_AUDIO;
  audio_context->channels = 1;
  audio_context->samplerate = 8000;
  return TRUE;
}

gboolean
gst_matroska_track_init_subtitle_context (GstMatroskaTrackContext ** p_context)
{
  GstMatroskaTrackSubtitleContext *subtitle_context;

  g_assert (p_context != NULL && *p_context != NULL);

  /* already set up? (track info might come before track type) */
  if ((*p_context)->type == GST_MATROSKA_TRACK_TYPE_SUBTITLE)
    return TRUE;

  /* it better not have been set up as some other track type ... */
  if ((*p_context)->type != 0) {
    g_return_val_if_reached (FALSE);
  }

  subtitle_context = g_renew (GstMatroskaTrackSubtitleContext, *p_context, 1);
  *p_context = (GstMatroskaTrackContext *) subtitle_context;

  (*p_context)->type = GST_MATROSKA_TRACK_TYPE_SUBTITLE;
  subtitle_context->invalid_utf8 = FALSE;
  subtitle_context->seen_markup_tag = FALSE;
  return TRUE;
}

void
gst_matroska_register_tags (void)
{
  /* TODO: register other custom tags */
}

GstBufferList *
gst_matroska_parse_xiph_stream_headers (gpointer codec_data,
    gsize codec_data_size)
{
  GstBufferList *list = NULL;
  guint8 *p = codec_data;
  gint i, offset, num_packets;
  guint *length, last;

  GST_MEMDUMP ("xiph codec data", codec_data, codec_data_size);

  if (codec_data == NULL || codec_data_size == 0)
    goto error;

  /* start of the stream and vorbis audio or theora video, need to
   * send the codec_priv data as first three packets */
  num_packets = p[0] + 1;
  GST_DEBUG ("%u stream headers, total length=%" G_GSIZE_FORMAT " bytes",
      (guint) num_packets, codec_data_size);

  length = g_alloca (num_packets * sizeof (guint));
  last = 0;
  offset = 1;

  /* first packets, read length values */
  for (i = 0; i < num_packets - 1; i++) {
    length[i] = 0;
    while (offset < codec_data_size) {
      length[i] += p[offset];
      if (p[offset++] != 0xff)
        break;
    }
    last += length[i];
  }
  if (offset + last > codec_data_size)
    goto error;

  /* last packet is the remaining size */
  length[i] = codec_data_size - offset - last;

  list = gst_buffer_list_new ();

  for (i = 0; i < num_packets; i++) {
    GstBuffer *hdr;

    GST_DEBUG ("buffer %d: %u bytes", i, (guint) length[i]);

    if (offset + length[i] > codec_data_size)
      goto error;

    hdr = gst_buffer_new_wrapped (g_memdup (p + offset, length[i]), length[i]);
    gst_buffer_list_add (list, hdr);

    offset += length[i];
  }

  return list;

/* ERRORS */
error:
  {
    if (list != NULL)
      gst_buffer_list_unref (list);
    return NULL;
  }
}

GstBufferList *
gst_matroska_parse_speex_stream_headers (gpointer codec_data,
    gsize codec_data_size)
{
  GstBufferList *list = NULL;
  GstBuffer *hdr;
  guint8 *pdata = codec_data;

  GST_MEMDUMP ("speex codec data", codec_data, codec_data_size);

  if (codec_data == NULL || codec_data_size < 80) {
    GST_WARNING ("not enough codec priv data for speex headers");
    return NULL;
  }

  if (memcmp (pdata, "Speex   ", 8) != 0) {
    GST_WARNING ("no Speex marker at start of stream headers");
    return NULL;
  }

  list = gst_buffer_list_new ();

  hdr = gst_buffer_new_wrapped (g_memdup (pdata, 80), 80);
  gst_buffer_list_add (list, hdr);

  if (codec_data_size > 80) {
    hdr = gst_buffer_new_wrapped (g_memdup (pdata + 80, codec_data_size - 80),
        codec_data_size - 80);
    gst_buffer_list_add (list, hdr);
  }

  return list;
}

GstBufferList *
gst_matroska_parse_flac_stream_headers (gpointer codec_data,
    gsize codec_data_size)
{
  GstBufferList *list = NULL;
  GstBuffer *hdr;
  guint8 *pdata = codec_data;
  guint len, off;

  GST_MEMDUMP ("flac codec data", codec_data, codec_data_size);

  /* need at least 'fLaC' marker + STREAMINFO metadata block */
  if (codec_data == NULL || codec_data_size < ((4) + (4 + 34))) {
    GST_WARNING ("not enough codec priv data for flac headers");
    return NULL;
  }

  if (memcmp (pdata, "fLaC", 4) != 0) {
    GST_WARNING ("no flac marker at start of stream headers");
    return NULL;
  }

  list = gst_buffer_list_new ();

  hdr = gst_buffer_new_wrapped (g_memdup (pdata, 4), 4);
  gst_buffer_list_add (list, hdr);

  /* skip fLaC marker */
  off = 4;

  /* FIXME: check size remaining */
  while (off < codec_data_size) {
    len = GST_READ_UINT8 (pdata + off + 1) << 16;
    len |= GST_READ_UINT8 (pdata + off + 2) << 8;
    len |= GST_READ_UINT8 (pdata + off + 3);

    GST_DEBUG ("header packet: len=%u bytes, flags=0x%02x", len, pdata[off]);

    /* FIXME: check size remaining */
    hdr = gst_buffer_new_wrapped (g_memdup (pdata + off, len + 4), len + 4);
    gst_buffer_list_add (list, hdr);

    off += 4 + len;
  }
  return list;
}
