/* Generic video aggregator plugin
 * Copyright (C) 2008 Wim Taymans <wim@fluendo.com>
 * Copyright (C) 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
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
 
#ifndef __GST_VIDEO_AGGREGATOR_H__
#define __GST_VIDEO_AGGREGATOR_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstaggregator.h>

#include "gstvideoaggregatorpad.h"

G_BEGIN_DECLS

#define GST_TYPE_VIDEO_AGGREGATOR (gst_videoaggregator_get_type())
#define GST_VIDEO_AGGREGATOR(obj) \
        (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VIDEO_AGGREGATOR, GstVideoAggregator))
#define GST_VIDEO_AGGREGATOR_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VIDEO_AGGREGATOR, GstVideoAggregatorClass))
#define GST_IS_VIDEO_AGGREGATOR(obj) \
        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VIDEO_AGGREGATOR))
#define GST_IS_VIDEO_AGGREGATOR_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VIDEO_AGGREGATOR))
#define GST_VIDEO_AGGREGATOR_GET_CLASS(obj) \
        (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_VIDEO_AGGREGATOR,GstVideoAggregatorClass))

typedef struct _GstVideoAggregator GstVideoAggregator;
typedef struct _GstVideoAggregatorClass GstVideoAggregatorClass;

/**
 * GstVideoAggregator:
 *
 * The opaque #GstVideoAggregator structure.
 */
struct _GstVideoAggregator
{
  GstAggregator aggregator;

  /* < private > */

  /* pad */
  GstPad *srcpad;

  /* Lock to prevent the state to change while aggregating */
  GMutex lock;

  /* Lock to prevent two src setcaps from happening at the same time  */
  GMutex setcaps_lock;

  /* Output caps */
  GstVideoInfo info;

  /* current caps */
  GstCaps *current_caps;
  gboolean send_caps;

  /* Current downstream segment */
  GstSegment segment;
  GstClockTime ts_offset;
  guint64 nframes;

  /* QoS stuff */
  gdouble proportion;
  GstClockTime earliest_time;
  guint64 qos_processed, qos_dropped;

  gboolean send_stream_start;
};

struct _GstVideoAggregatorClass
{
  GstAggregatorClass parent_class;

  /* Disable the frame colorspace conversion feature,
   * making pad template management responsability
   * of subclasses */
  gboolean disable_frame_convertion;

  gboolean			 (*modify_src_pad_info) (GstVideoAggregator *videoaggregator, GstVideoInfo *info);
  GstFlowReturn      (*aggregate_frames)          (GstVideoAggregator *videoaggregator, GstBuffer *outbuffer);
  GstCaps *          (*get_preferred_input_caps) (GstVideoAggregator *videoaggregator);
  GstFlowReturn      (*get_output_buffer) (GstVideoAggregator *videoaggregator, GstBuffer **outbuffer);
  gboolean           (*update_src_caps) (GstVideoAggregator *videoaggregator);
};

GType gst_videoaggregator_get_type (void);
gboolean gst_videoaggregator_src_setcaps (GstVideoAggregator * vagg, GstCaps * caps);

G_END_DECLS
#endif /* __GST_VIDEO_AGGREGATOR_H__ */
