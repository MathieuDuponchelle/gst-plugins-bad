/* GStreamer
 * Copyright (C) 2014 Mathieu Duponchelle <mathieu.duponchelle@oencreed.com>
 * Copyright (C) 2014 Thibault Saunier <tsaunier@opencreed.com>
 *
 * gstcollectpads.c:
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

#ifndef __GST_AGGREGATOR_H__
#define __GST_AGGREGATOR_H__

#include <gst/gst.h>

G_BEGIN_DECLS

/**************************
 * GstAggregator Structs  *
 *************************/

typedef struct _GstAggregator GstAggregator;
typedef struct _GstAggregatorPrivate GstAggregatorPrivate;
typedef struct _GstAggregatorClass GstAggregatorClass;

/************************
 * GstAggregatorPad API *
 ***********************/

#define GST_TYPE_AGGREGATOR_PAD            (gst_aggregator_pad_get_type())
#define GST_AGGREGATOR_PAD(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AGGREGATOR_PAD, GstAggregatorPad))
#define GST_AGGREGATOR_PAD_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AGGREGATOR_PAD, GstAggregatorPadClass))
#define GST_AGGREGATOR_PAD_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),GST_TYPE_AGGREGATOR_PAD, GstAggregatorPadClass))
#define GST_IS_AGGREGATOR_PAD(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AGGREGATOR_PAD))
#define GST_IS_AGGREGATOR_PAD_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AGGREGATOR_PAD))

/****************************
 * GstAggregatorPad Structs *
 ***************************/

typedef struct _GstAggregatorPad GstAggregatorPad;
typedef struct _GstAggregatorPadClass GstAggregatorPadClass;
typedef struct _GstAggregatorPadPrivate GstAggregatorPadPrivate;

/**
 * GstAggregatorPad:
 * @buffer: currently queued buffer.
 * @segment: last segment received.
 *
 * The implementation the GstPad to use with #GstAggregator
 */
struct _GstAggregatorPad
{
  GstPad        parent;

  GstBuffer     *buffer;
  GstSegment    segment;
  gboolean      eos;

  /* < Private > */
  GstAggregatorPadPrivate *priv;

  gpointer _gst_reserved[GST_PADDING];
};

/*
 * GstAggregatorPadClass:
 * @flush:    Optional
 *            Called when the pad has received a flush stop, this is the place
 *            to flush any information specific to the pad, it allows for individual
 *            pads to be flushed while others might not be.
 * 
 */           
struct _GstAggregatorPadClass
{
  GstPadClass parent_class;

  GstFlowReturn (*flush)     (GstAggregatorPad * aggpad, GstAggregator * aggregator);

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING];
};

GType gst_aggregator_pad_get_type           (void);

/****************************
 * GstAggregatorPad methods *
 ***************************/

GstBuffer * gst_aggregator_pad_steal_buffer (GstAggregatorPad *  pad);
GstBuffer * gst_aggregator_pad_get_buffer   (GstAggregatorPad *  pad);

GstAggregatorPad * gst_aggregator_pad_new   (void);

/*********************
 * GstAggregator API *
 ********************/

#define GST_TYPE_AGGREGATOR            (gst_aggregator_get_type())
#define GST_AGGREGATOR(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AGGREGATOR,GstAggregator))
#define GST_AGGREGATOR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AGGREGATOR,GstAggregatorClass))
#define GST_AGGREGATOR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),GST_TYPE_AGGREGATOR,GstAggregatorClass))
#define GST_IS_AGGREGATOR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AGGREGATOR))
#define GST_IS_AGGREGATOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AGGREGATOR))

#define GST_FLOW_CUSTOM_SUCCESS GST_FLOW_NOT_HANDLED

/**
 * GstAggregator:
 * @aggregator_pads: #GList of #GstAggregatorPad managed by this #GstAggregator.
 *
 * Collectpads object.
 */
struct _GstAggregator {
  GstElement      parent;

  GstPad *srcpad;
  GstSegment segment;

  /*< private >*/
  GstAggregatorPrivate *priv;


  gpointer _gst_reserved[GST_PADDING];
};

struct _GstAggregatorClass {
  GstElementClass parent_class;

  GType sinkpads_type;

  GstFlowReturn (*flush)          (GstAggregator    *  aggregator);

  GstFlowReturn (*clip)           (GstAggregator    *  agg,
                                   GstAggregatorPad *  bpad,
                                   GstBuffer        *  buf,
                                   GstBuffer        ** outbuf);

  /* sinkpads virtual methods */
  gboolean      (*sink_event)     (GstAggregator    *  aggregate,
                                   GstAggregatorPad *  bpad,
                                   GstEvent         *  event);

  gboolean      (*sink_query)     (GstAggregator    *  aggregate,
                                   GstAggregatorPad *  bpad,
                                   GstQuery         *  query);

  /* srcpad virtual methods */
  gboolean      (*src_event)      (GstAggregator    *  aggregate,
                                   GstEvent         *  event);

  gboolean      (*src_query)      (GstAggregator    *  aggregate,
                                   GstQuery         *  query);

  gboolean      (*src_activate)   (GstAggregator    *  aggregator,
                                   GstPadMode          mode,
                                   gboolean            active);

  GstFlowReturn (*aggregate)      (GstAggregator    *  aggregator);

  /* Should be linked up first */
  gboolean      (*stop)           (GstAggregator    *  aggregator);

  gboolean      (*start)          (GstAggregator    *  aggregator);

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING];
};

/*************************
 * GstAggregator methods *
 ************************/

GstFlowReturn  gst_aggregator_finish_buffer         (GstAggregator                *  agg,
                                                     GstBuffer                    *  buf);
void           gst_aggregator_set_src_caps          (GstAggregator                *  agg,
                                                     GstCaps                      *  caps);

GType gst_aggregator_get_type(void);

/* API that should eventually land in GstElement itself*/
typedef gboolean (*GstAggregatorPadForeachFunc)    (GstAggregator                 *  self,
                                                    GstPad                        *  pad,
                                                    gpointer                         user_data);
gboolean gst_aggregator_iterate_sinkpads           (GstAggregator                 *  self,
                                                    GstAggregatorPadForeachFunc      func,
                                                    gpointer                         user_data);


G_END_DECLS

#endif /* __GST_AGGREGATOR_H__ */
