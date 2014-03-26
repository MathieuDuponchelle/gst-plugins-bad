/* GStreamer
 * Copyright (C) <2009> Руслан Ижбулатов <lrn1986 _at_ gmail _dot_ com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

/**
 * SECTION:element-ssim
 *
 * The ssim calculates SSIM (Structural SIMilarity) index for two or more 
 * streams, for each frame.
 * First stream is the original, other streams are modified (compressed) ones.
 * ssim will calculate SSIM index of each frame of each modified stream, using 
 * original stream as a reference.
 *
 * The ssim accepts only YUV planar top-first data and calculates only Y-SSIM.
 * All streams must have the same width, height and colorspace.
 * Output streams are greyscale video streams, where bright pixels indicate 
 * high SSIM values, dark pixels - low SSIM values.
 * The ssim also calculates mean SSIM index for each frame and emits is as a 
 * message.
 * ssim is intended to be used with videomeasure_collector element to catch the 
 * events (such as mean SSIM index values) and save them into a file.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch ssim name=ssim ssim.src0 ! videoconvert ! glimagesink filesrc
 * location=orig.avi ! decodebin2 ! ssim.original filesrc location=compr.avi !
 * decodebin2 ! ssim.modified0
 * ]| This pipeline produces a video stream that consists of SSIM frames.
 * </refsect2>
 *
 * Last reviewed on 2009-09-06 (0.10.?)
 */
/* Element-Checklist-Version: 5 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvideomeasure_ssim.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GST_CAT_DEFAULT gst_ssim_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define gst_ssim_parent_class parent_class
G_DEFINE_TYPE (GstSSim, gst_ssim, GST_TYPE_BASE_MIXER);

static gboolean gst_ssim_regenerate_windows (GstSSim * ssim);

static void
gst_ssim_post_message (GstSSim * ssim, GstBuffer * buffer, gfloat mssim,
    gfloat lowest, gfloat highest)
{
  GstMessage *m;
  guint64 offset;

  offset = GST_BUFFER_OFFSET (buffer);

  m = gst_message_new_element (GST_OBJECT_CAST (ssim),
      gst_structure_new ("SSIM",
          "offset", G_TYPE_UINT64, offset,
          "timestamp", GST_TYPE_CLOCK_TIME, GST_BUFFER_TIMESTAMP (buffer),
          "mean", G_TYPE_FLOAT, mssim,
          "lowest", G_TYPE_FLOAT, lowest,
          "highest", G_TYPE_FLOAT, highest, NULL));

  GST_DEBUG_OBJECT (GST_OBJECT (ssim), "Frame %" G_GINT64_FORMAT
      " @ %" GST_TIME_FORMAT " mean SSIM is %f, l-h is %f-%f", offset,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)), mssim, lowest, highest);

  gst_element_post_message (GST_ELEMENT_CAST (ssim), m);
}

static void
calculate_mu (GstSSim * ssim, gfloat * outmu, guint8 * buf)
{
  gint oy, ox, iy, ix;

  for (oy = 0; oy < ssim->height; oy++) {
    for (ox = 0; ox < ssim->width; ox++) {
      gfloat mu = 0;
      gfloat elsumm;
      gint weight_y_base, weight_x_base;
      gint weight_offset;
      gint pixel_offset;
      gint winstart_y;
      gint wghstart_y;
      gint winend_y;
      gint winstart_x;
      gint wghstart_x;
      gint winend_x;
      gfloat weight;
      gint source_offset;

      source_offset = oy * ssim->width + ox;

      winstart_x = ssim->windows[source_offset].x_window_start;
      wghstart_x = ssim->windows[source_offset].x_weight_start;
      winend_x = ssim->windows[source_offset].x_window_end;
      winstart_y = ssim->windows[source_offset].y_window_start;
      wghstart_y = ssim->windows[source_offset].y_weight_start;
      winend_y = ssim->windows[source_offset].y_window_end;
      elsumm = ssim->windows[source_offset].element_summ;

      switch (ssim->windowtype) {
        case 0:
          for (iy = winstart_y; iy <= winend_y; iy++) {
            pixel_offset = iy * ssim->width;
            for (ix = winstart_x; ix <= winend_x; ix++)
              mu += buf[pixel_offset + ix];
          }
          mu = mu / elsumm;
          break;
        case 1:

          weight_y_base = wghstart_y - winstart_y;
          weight_x_base = wghstart_x - winstart_x;

          for (iy = winstart_y; iy <= winend_y; iy++) {
            pixel_offset = iy * ssim->width;
            weight_offset = (weight_y_base + iy) * ssim->windowsize +
                weight_x_base;
            for (ix = winstart_x; ix <= winend_x; ix++) {
              weight = ssim->weights[weight_offset + ix];
              mu += weight * buf[pixel_offset + ix];
            }
          }
          mu = mu / elsumm;
          break;
      }
      outmu[oy * ssim->width + ox] = mu;
    }
  }

}

static void
calcssim_without_mu (GstSSim * ssim, guint8 * org, gfloat * orgmu, guint8 * mod,
    guint8 * out, gfloat * mean, gfloat * lowest, gfloat * highest)
{
  gint oy, ox, iy, ix;
  gfloat cumulative_ssim = 0;
  *lowest = G_MAXFLOAT;
  *highest = -G_MAXFLOAT;

  for (oy = 0; oy < ssim->height; oy++) {
    for (ox = 0; ox < ssim->width; ox++) {
      gfloat mu_o = 128, mu_m = 128;
      gdouble sigma_o = 0, sigma_m = 0, sigma_om = 0;
      gfloat tmp1 = 0, tmp2 = 0;
      gfloat elsumm = 0;
      gint weight_y_base, weight_x_base;
      gint weight_offset;
      gint pixel_offset;
      gint winstart_y;
      gint wghstart_y;
      gint winend_y;
      gint winstart_x;
      gint wghstart_x;
      gint winend_x;
      gfloat weight;
      gint source_offset;

      source_offset = oy * ssim->width + ox;

      winstart_x = ssim->windows[source_offset].x_window_start;
      wghstart_x = ssim->windows[source_offset].x_weight_start;
      winend_x = ssim->windows[source_offset].x_window_end;
      winstart_y = ssim->windows[source_offset].y_window_start;
      wghstart_y = ssim->windows[source_offset].y_weight_start;
      winend_y = ssim->windows[source_offset].y_window_end;
      elsumm = ssim->windows[source_offset].element_summ;

      weight_y_base = wghstart_y - winstart_y;
      weight_x_base = wghstart_x - winstart_x;
      switch (ssim->windowtype) {
        case 0:
          for (iy = winstart_y; iy <= winend_y; iy++) {
            guint8 *org_with_offset, *mod_with_offset;
            pixel_offset = iy * ssim->width;
            org_with_offset = &org[pixel_offset];
            mod_with_offset = &mod[pixel_offset];
            for (ix = winstart_x; ix <= winend_x; ix++) {
              tmp1 = org_with_offset[ix] - mu_o;
              sigma_o += tmp1 * tmp1;
              tmp2 = mod_with_offset[ix] - mu_m;
              sigma_m += tmp2 * tmp2;
              sigma_om += tmp1 * tmp2;
            }
          }
          break;
        case 1:

          weight_y_base = wghstart_y - winstart_y;
          weight_x_base = wghstart_x - winstart_x;

          for (iy = winstart_y; iy <= winend_y; iy++) {
            guint8 *org_with_offset, *mod_with_offset;
            gfloat *weights_with_offset;
            gfloat wt1, wt2;
            pixel_offset = iy * ssim->width;
            weight_offset = (weight_y_base + iy) * ssim->windowsize +
                weight_x_base;
            org_with_offset = &org[pixel_offset];
            mod_with_offset = &mod[pixel_offset];
            weights_with_offset = &ssim->weights[weight_offset];
            for (ix = winstart_x; ix <= winend_x; ix++) {
              weight = weights_with_offset[ix];
              tmp1 = org_with_offset[ix] - mu_o;
              tmp2 = mod_with_offset[ix] - mu_m;
              wt1 = weight * tmp1;
              wt2 = weight * tmp2;
              sigma_o += wt1 * tmp1;
              sigma_m += wt2 * tmp2;
              sigma_om += wt1 * tmp2;
            }
          }
          break;
      }
      sigma_o = sqrt (sigma_o / elsumm);
      sigma_m = sqrt (sigma_m / elsumm);
      sigma_om = sigma_om / elsumm;
      tmp1 = (2 * mu_o * mu_m + ssim->const1) * (2 * sigma_om + ssim->const2) /
          ((mu_o * mu_o + mu_m * mu_m + ssim->const1) *
          (sigma_o * sigma_o + sigma_m * sigma_m + ssim->const2));

      /* SSIM can go negative, that's why it is
         127 + index * 128 instead of index * 255 */
      out[oy * ssim->width + ox] = 127 + tmp1 * 128;
      *lowest = MIN (*lowest, tmp1);
      *highest = MAX (*highest, tmp1);
      cumulative_ssim += tmp1;
    }
  }
  *mean = cumulative_ssim / (ssim->width * ssim->height);
}

static void
calcssim_canonical (GstSSim * ssim, guint8 * org, gfloat * orgmu, guint8 * mod,
    guint8 * out, gfloat * mean, gfloat * lowest, gfloat * highest)
{
  gint oy, ox, iy, ix;
  gfloat cumulative_ssim = 0;
  *lowest = G_MAXFLOAT;
  *highest = -G_MAXFLOAT;

  for (oy = 0; oy < ssim->height; oy++) {
    for (ox = 0; ox < ssim->width; ox++) {
      gfloat mu_o = 0, mu_m = 0;
      gdouble sigma_o = 0, sigma_m = 0, sigma_om = 0;
      gfloat tmp1, tmp2;
      gfloat elsumm = 0;
      gint weight_y_base, weight_x_base;
      gint weight_offset;
      gint pixel_offset;
      gint winstart_y;
      gint wghstart_y;
      gint winend_y;
      gint winstart_x;
      gint wghstart_x;
      gint winend_x;
      gfloat weight;
      gint source_offset;

      source_offset = oy * ssim->width + ox;

      winstart_x = ssim->windows[source_offset].x_window_start;
      wghstart_x = ssim->windows[source_offset].x_weight_start;
      winend_x = ssim->windows[source_offset].x_window_end;
      winstart_y = ssim->windows[source_offset].y_window_start;
      wghstart_y = ssim->windows[source_offset].y_weight_start;
      winend_y = ssim->windows[source_offset].y_window_end;
      elsumm = ssim->windows[source_offset].element_summ;

      switch (ssim->windowtype) {
        case 0:
          for (iy = winstart_y; iy <= winend_y; iy++) {
            pixel_offset = iy * ssim->width;
            for (ix = winstart_x; ix <= winend_x; ix++) {
              mu_m += mod[pixel_offset + ix];
            }
          }
          mu_m = mu_m / elsumm;
          mu_o = orgmu[oy * ssim->width + ox];
          for (iy = winstart_y; iy <= winend_y; iy++) {
            pixel_offset = iy * ssim->width;
            for (ix = winstart_x; ix <= winend_x; ix++) {
              tmp1 = org[pixel_offset + ix] - mu_o;
              tmp2 = mod[pixel_offset + ix] - mu_m;
              sigma_o += tmp1 * tmp1;
              sigma_m += tmp2 * tmp2;
              sigma_om += tmp1 * tmp2;
            }
          }
          break;
        case 1:

          weight_y_base = wghstart_y - winstart_y;
          weight_x_base = wghstart_x - winstart_x;

          for (iy = winstart_y; iy <= winend_y; iy++) {
            pixel_offset = iy * ssim->width;
            weight_offset = (weight_y_base + iy) * ssim->windowsize +
                weight_x_base;
            for (ix = winstart_x; ix <= winend_x; ix++) {
              weight = ssim->weights[weight_offset + ix];
              mu_o += weight * org[pixel_offset + ix];
              mu_m += weight * mod[pixel_offset + ix];
            }
          }
          mu_m = mu_m / elsumm;
          mu_o = orgmu[oy * ssim->width + ox];
          for (iy = winstart_y; iy <= winend_y; iy++) {
            gfloat *weights_with_offset;
            guint8 *org_with_offset, *mod_with_offset;
            gfloat wt1, wt2;
            pixel_offset = iy * ssim->width;
            weight_offset = (weight_y_base + iy) * ssim->windowsize +
                weight_x_base;
            weights_with_offset = &ssim->weights[weight_offset];
            org_with_offset = &org[pixel_offset];
            mod_with_offset = &mod[pixel_offset];
            for (ix = winstart_x; ix <= winend_x; ix++) {
              weight = weights_with_offset[ix];
              tmp1 = org_with_offset[ix] - mu_o;
              tmp2 = mod_with_offset[ix] - mu_m;
              wt1 = weight * tmp1;
              wt2 = weight * tmp2;
              sigma_o += wt1 * tmp1;
              sigma_m += wt2 * tmp2;
              sigma_om += wt1 * tmp2;
            }
          }
          break;
      }
      sigma_o = sqrt (sigma_o / elsumm);
      sigma_m = sqrt (sigma_m / elsumm);
      sigma_om = sigma_om / elsumm;
      tmp1 = (2 * mu_o * mu_m + ssim->const1) * (2 * sigma_om + ssim->const2) /
          ((mu_o * mu_o + mu_m * mu_m + ssim->const1) *
          (sigma_o * sigma_o + sigma_m * sigma_m + ssim->const2));

      /* SSIM can go negative, that's why it is
         127 + index * 128 instead of index * 255 */
      out[oy * ssim->width + ox] = 127 + tmp1 * 128;
      *lowest = MIN (*lowest, tmp1);
      *highest = MAX (*highest, tmp1);
      cumulative_ssim += tmp1;
    }
  }
  *mean = cumulative_ssim / (ssim->width * ssim->height);
}

static void
gst_ssim_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSSim *ssim;

  ssim = GST_SSIM (object);

  switch (prop_id) {
    case PROP_SSIM_TYPE:
      ssim->ssimtype = g_value_get_int (value);
      break;
    case PROP_WINDOW_TYPE:
      ssim->windowtype = g_value_get_int (value);
      g_free (ssim->windows);
      ssim->windows = NULL;
      break;
    case PROP_WINDOW_SIZE:
      ssim->windowsize = g_value_get_int (value);
      g_free (ssim->windows);
      ssim->windows = NULL;
      break;
    case PROP_GAUSS_SIGMA:
      ssim->sigma = g_value_get_float (value);
      g_free (ssim->windows);
      ssim->windows = NULL;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_ssim_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstSSim *ssim;

  ssim = GST_SSIM (object);

  switch (prop_id) {
    case PROP_SSIM_TYPE:
      g_value_set_int (value, ssim->ssimtype);
      break;
    case PROP_WINDOW_TYPE:
      g_value_set_int (value, ssim->windowtype);
      break;
    case PROP_WINDOW_SIZE:
      g_value_set_int (value, ssim->windowsize);
      break;
    case PROP_GAUSS_SIGMA:
      g_value_set_float (value, ssim->sigma);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstFlowReturn
gst_ssim_mix_frames (GstBasemixer * mix, GstVideoFrame * outframe)
{
  GstSSim *ssim;
  GSList *tmp;
  GstFlowReturn ret = GST_FLOW_OK;
  gfloat *orgmu = NULL;
  gfloat mssim = 0, lowest = 1, highest = -1;
  GstVideoFrame *reference_frame = NULL;

  ssim = GST_SSIM (mix);

  if (G_UNLIKELY (ssim->windows == NULL)) {
    GST_DEBUG_OBJECT (ssim, "Regenerating windows");
    gst_ssim_regenerate_windows (ssim);
  }

  switch (ssim->ssimtype) {
    case 0:
      ssim->func = (GstSSimFunction) calcssim_canonical;
      break;
    case 1:
      ssim->func = (GstSSimFunction) calcssim_without_mu;
      break;
    default:
      return GST_FLOW_ERROR;
  }

  GST_LOG_OBJECT (ssim, "starting to cycle through streams");

  for (tmp = mix->sinkpads; tmp; tmp = tmp->next) {
    GstMapInfo ref_info;
    GstMapInfo cmp_info;
    //    GstMapInfo out_info;

    GstBasemixerPad *mix_pad = tmp->data;
    GstVideoFrame *frame;
    GValue vmean = { 0 }
    , vlowest = {
    0}
    , vhighest = {
    0};

    frame = mix_pad->mixed_frame;
    if (!frame)
      continue;

    if (reference_frame == NULL) {
      orgmu = g_new (gfloat, ssim->width * ssim->height);
      reference_frame = frame;
      gst_buffer_map (reference_frame->buffer, &ref_info, GST_MAP_READ);
      calculate_mu (ssim, orgmu, ref_info.data);
      gst_buffer_unmap (reference_frame->buffer, &ref_info);
      continue;
    }

    /* TODO : FLAG_GAP ? */

    g_value_init (&vmean, G_TYPE_FLOAT);
    g_value_init (&vlowest, G_TYPE_FLOAT);
    g_value_init (&vhighest, G_TYPE_FLOAT);

    gst_buffer_map (reference_frame->buffer, &ref_info, GST_MAP_READ);
    gst_buffer_map (frame->buffer, &cmp_info, GST_MAP_READ);
    ssim->func (ssim, ref_info.data, orgmu, cmp_info.data,
        GST_VIDEO_FRAME_PLANE_DATA (outframe, 0), &mssim, &lowest, &highest);

    gst_ssim_post_message (ssim, frame->buffer, mssim, lowest, highest);

    gst_buffer_unmap (reference_frame->buffer, &ref_info);
    gst_buffer_unmap (frame->buffer, &cmp_info);

    GST_DEBUG_OBJECT (GST_OBJECT (ssim), "MSSIM is %f, l-h is %f - %f",
        mssim, lowest, highest);

    g_value_set_float (&vmean, mssim);
    g_value_set_float (&vlowest, lowest);
    g_value_set_float (&vhighest, highest);

    /* measured = gst_event_new_measured (offset, */
    /*     GST_BUFFER_TIMESTAMP (inbuf), "SSIM", &vmean, &vlowest, &vhighest); */
    /* gst_pad_push_event (c->pad, measured); */

    /* send it out */
    /* GST_DEBUG_OBJECT (ssim, "pushing outbuf, timestamp %" GST_TIME_FORMAT */
    /*     ", size %d", GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)), */
    /*     GST_BUFFER_SIZE (outbuf)); */
    /* ret &= gst_pad_push (c->pad, outbuf); */

  }

  if (ssim->ssimtype == 0)
    g_free (orgmu);

  return ret;
}

static gboolean
gst_ssim_modify_src_pad_info (GstBasemixer * mix, GstVideoInfo * info)
{
  GstSSim *ssim = GST_SSIM (mix);

  ssim->width = info->width;
  ssim->height = info->height;

  gst_video_info_set_format (info, GST_VIDEO_FORMAT_GRAY8,
      info->width, info->height);

  return TRUE;
}

static GstCaps *
gst_ssim_get_preferred_input_caps (GstBasemixer * mix)
{
  return gst_caps_from_string ("video/x-raw, format=I420");
}

static void
gst_ssim_class_init (GstSSimClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBasemixerClass *base_mixer_class = (GstBasemixerClass *) klass;

  gobject_class->set_property = gst_ssim_set_property;
  gobject_class->get_property = gst_ssim_get_property;

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_SSIM_TYPE,
      g_param_spec_int ("ssim-type", "SSIM type",
          "Type of the SSIM metric. 0 - canonical. 1 - with fixed mu "
          "(almost the same results, but roughly 20% faster)",
          0, 1, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_WINDOW_TYPE,
      g_param_spec_int ("window-type", "Window type",
          "Type of the weighting in the window. "
          "0 - no weighting. 1 - Gaussian weighting (controlled by \"sigma\")",
          0, 1, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_WINDOW_SIZE,
      g_param_spec_int ("window-size", "Window size",
          "Size of a window.", 1, 22, 11,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_GAUSS_SIGMA,
      g_param_spec_float ("gauss-sigma", "Deviation (for Gauss function)",
          "Used to calculate Gussian weights "
          "(only when using Gaussian window).",
          G_MINFLOAT, 10, 1.5, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class, "SSim",
      "Filter/Analyzer/Video",
      "Calculate Y-SSIM for n+2 YUV video streams",
      "Руслан Ижбулатов <lrn1986 _at_ gmail _dot_ com>");

  base_mixer_class->mix_frames = gst_ssim_mix_frames;
  base_mixer_class->modify_src_pad_info = gst_ssim_modify_src_pad_info;
  base_mixer_class->get_preferred_input_caps =
      gst_ssim_get_preferred_input_caps;
}

static void
gst_ssim_init (GstSSim * ssim)
{
  ssim->windowsize = 11;
  ssim->windowtype = 1;
  ssim->windows = NULL;
  ssim->sigma = 1.5;
  ssim->ssimtype = 0;
  ssim->src = g_ptr_array_new ();
  ssim->padcount = 0;
  ssim->collect_event = NULL;
  ssim->sinkcaps = NULL;
}

typedef gfloat (*GstSSimWeightFunc) (GstSSim * ssim, gint y, gint x);

static gfloat
gst_ssim_weight_func_none (GstSSim * ssim, gint y, gint x)
{
  return 1;
}

static gfloat
gst_ssim_weight_func_gauss (GstSSim * ssim, gint y, gint x)
{
  gfloat coord = sqrt (x * x + y * y);
  return exp (-1 * (coord * coord) / (2 * ssim->sigma * ssim->sigma)) /
      (ssim->sigma * sqrt (2 * G_PI));
}

static gboolean
gst_ssim_regenerate_windows (GstSSim * ssim)
{
  gint windowiseven;
  gint y, x, y2, x2;
  GstSSimWeightFunc func;
  gfloat normal_summ = 0;
  gint normal_count = 0;

  g_free (ssim->weights);

  ssim->weights = g_new (gfloat, ssim->windowsize * ssim->windowsize);

  windowiseven = ((gint) ssim->windowsize / 2) * 2 == ssim->windowsize ? 1 : 0;

  g_free (ssim->windows);

  ssim->windows = g_new (GstSSimWindowCache, ssim->height * ssim->width);

  switch (ssim->windowtype) {
    case 0:
      func = gst_ssim_weight_func_none;
      break;
    case 1:
      func = gst_ssim_weight_func_gauss;
      break;
    default:
      GST_WARNING_OBJECT (ssim, "unknown window type - %d. Defaulting to %d",
          ssim->windowtype, 1);
      ssim->windowtype = 1;
      func = gst_ssim_weight_func_gauss;
  }

  for (y = 0; y < ssim->windowsize; y++) {
    gint yoffset = y * ssim->windowsize;
    for (x = 0; x < ssim->windowsize; x++) {
      ssim->weights[yoffset + x] = func (ssim, x - ssim->windowsize / 2 +
          windowiseven, y - ssim->windowsize / 2 + windowiseven);
      normal_summ += ssim->weights[yoffset + x];
      normal_count++;
    }
  }

  for (y = 0; y < ssim->height; y++) {
    for (x = 0; x < ssim->width; x++) {
      GstSSimWindowCache win;
      gint element_count = 0;

      win.x_window_start = x - ssim->windowsize / 2 + windowiseven;
      win.x_weight_start = 0;
      if (win.x_window_start < 0) {
        win.x_weight_start = -win.x_window_start;
        win.x_window_start = 0;
      }

      win.x_window_end = x + ssim->windowsize / 2;
      if (win.x_window_end >= ssim->width)
        win.x_window_end = ssim->width - 1;

      win.y_window_start = y - ssim->windowsize / 2 + windowiseven;
      win.y_weight_start = 0;
      if (win.y_window_start < 0) {
        win.y_weight_start = -win.y_window_start;
        win.y_window_start = 0;
      }

      win.y_window_end = y + ssim->windowsize / 2;
      if (win.y_window_end >= ssim->height)
        win.y_window_end = ssim->height - 1;

      win.element_summ = 0;
      element_count = (win.y_window_end - win.y_window_start + 1) *
          (win.x_window_end - win.x_window_start + 1);
      if (element_count == normal_count)
        win.element_summ = normal_summ;
      else {
        for (y2 = win.y_weight_start; y2 < ssim->windowsize; y2++) {
          for (x2 = win.x_weight_start; x2 < ssim->windowsize; x2++) {
            win.element_summ += ssim->weights[y2 * ssim->windowsize + x2];
          }
        }
      }
      ssim->windows[(y * ssim->width + x)] = win;
    }
  }

  /* FIXME: while 0.01 and 0.03 are pretty much static, the 255 implies that
   * we're working with 8-bit-per-color-component format, which may not be true
   */
  ssim->const1 = 0.01 * 255 * 0.01 * 255;
  ssim->const2 = 0.03 * 255 * 0.03 * 255;
  return TRUE;
}

/* Element registration */
static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_ssim_debug, "ssim", 0, "SSim");

  return gst_element_register (plugin, "ssim", GST_RANK_PRIMARY, GST_TYPE_SSIM);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    ssim,
    "ssim", plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
