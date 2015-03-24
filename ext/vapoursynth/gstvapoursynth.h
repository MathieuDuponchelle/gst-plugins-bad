/* GStreamer
 * Copyright (C) 2015 Edward Hervey <bilboed@bilboed.com>
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

#ifndef __GST_VAPOURSYNTH_H__
#define __GST_VAPOURSYNTH_H__

#include <gst/gst.h>

#include <VapourSynth.h>

G_BEGIN_DECLS
#define GST_VAPOURSYNTH(obj) \
  ((GstVapourSynth *) obj)
#define GST_VAPOURSYNTH_CLASS(klass) \
  ((GstVapourSynthClass *) klass)
#define GST_VAPOURSYNTH_GET_CLASS(obj) \
  ((GstVapourSynthClass *) g_type_class_peek (G_TYPE_FROM_INSTANCE (obj)))

typedef struct _GstVapourSynth GstVapourSynth;
typedef struct _GstVapourSynthClass GstVapourSynthClass;

typedef struct _GstVapourSynthPropertyDef GstVapourSynthPropertyDef;
typedef enum {
  VS_PROP_CLIP	= 0,
  VS_PROP_INT   = 1,
  VS_PROP_FLOAT = 2,
  VS_PROP_DATA  = 3,
  VS_PROP_UNKNOWN = 0xff
} GstVapourSynthPropertyType;

/* FIXME : How to differentiate values that are actually set
 * by the user, from cases where we just use optional (unset) values ?
 *   => structure ? */
struct _GstVapourSynthPropertyDef{
  GstVapourSynthPropertyType type;
  gboolean optional;
  /* GStreamer public name */
  gchar *name;
  /* GStreamer description */
  gchar *desc;
  /* key to use */
  gchar *prop_key;
  /* index to use (for arrayed properties) */
  guint prop_index;
  /* For actual GObject properties */
  gint registered_type;
};


struct _GstVapourSynth {
  GstElement parent;

  VSMap *invokeres;
  VSNodeRef *inputfilter;
  VSVideoInfo vi;
  GstStructure *properties;
};

struct _GstVapourSynthClass {
  GstElementClass parent;

  /* The plugin whose function we wrap */
  VSPlugin *vsplugin;
  gchar *func_name;
  gchar *plugin_ns;

  /* NULL terminated */
  GstVapourSynthPropertyDef **properties;
};

G_END_DECLS

#endif /* __GST_VAPOURSYNTH_H__ */
