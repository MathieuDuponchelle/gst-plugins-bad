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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvapoursynth.h"

#include <string.h>
#include <gmodule.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY (vapoursynth_debug);
#define GST_CAT_DEFAULT vapoursynth_debug

#define DEFAULT_RING_SIZE 6
#define DEFAULT_CACHE_SIZE 3

/* Class-specific data used at type registration time */
typedef struct
{
  gchar *func_name;
  gchar *func_args;
  gchar *plugin_ns;
  gchar *plugin_desc;
} GstVapourSynthClassData;

static void gst_vapoursynth_class_init (GstVapourSynthClass * klass,
    GstVapourSynthClassData * klass_data);
static void gst_vapoursynth_init (GstVapourSynth * object);

static void debug_vs_map (VSMap * map);

static const VSAPI *vsapi = NULL;
static VSCore *vscore = NULL;

static void
gst_vapoursynth_input_filter_init (VSMap * in, VSMap * out, void **instanceData,
    VSNode * node, VSCore * core, const VSAPI * vsapi)
{
  GstVapourSynth *self = (GstVapourSynth *) * instanceData;

  GST_DEBUG_OBJECT (self, "in:%p, out:%p, node:%p, core:%p, vsapi:%p",
      in, out, node, core, vsapi);
  GST_DEBUG_OBJECT (self, "in map has %d keys", vsapi->propNumKeys (in));
  debug_vs_map (in);
  GST_DEBUG_OBJECT (self, "out map has %d keys", vsapi->propNumKeys (out));
  debug_vs_map (out);

  vsapi->setVideoInfo (&self->vi, 1, node);
}

static const VSFrameRef *
gst_vapoursynth_input_filter_get_frame (int n, int activationReason,
    void **instanceData, void **frameData, VSFrameContext * frameCtx,
    VSCore * core, const VSAPI * vsapi)
{
  VSFrameRef *retframe = NULL;
  GstVapourSynth *self = (GstVapourSynth *) * instanceData;
  GstVideoFrame vframe;
  GstBuffer *input;
  guint i;
  VSMap *frameprops;

  GST_DEBUG_OBJECT (self,
      "n:%d, activationReason:%d, frameData:%p, frameCtx:%p", n,
      activationReason, frameData, frameCtx);

  g_mutex_lock (&self->ring_mutex);
  /* Error out if filter is requesting a very old frame */
  if (n < self->tail) {
    GST_ERROR_OBJECT (self,
        "Filter is asking for a frame we no longer have (n:%d < tail:%d)", n,
        self->tail);
    g_mutex_unlock (&self->ring_mutex);
    return NULL;
  }

  /* Do we need to drop old frames ? */
  while (n > self->cache_size && (n - self->cache_size > self->tail)) {
    GST_DEBUG_OBJECT (self, "Popping off buffer at tail:%d", self->tail);
    gst_buffer_unref (self->ringbuffer[self->tail % self->ring_size]);
    self->ringbuffer[self->tail % self->ring_size] = NULL;
    self->tail++;
    g_cond_signal (&self->write_cond);
  }

  /* While no data is available, or the requested frame still hasn't arrive, wait */
  while (self->head == self->tail || n > self->head) {
    GST_DEBUG_OBJECT (self, "Waiting for new buffer (n:%d , head:%d)",
        n, self->head);
    g_cond_wait (&self->read_cond, &self->ring_mutex);
  }
  /* FIXME : IMPLEMENT FLUSHING/UNLOCK ! */
  {
    guint i;
    GST_DEBUG_OBJECT (self, "head:%d, tail:%d", self->head, self->tail);
    for (i = 0; i < self->ring_size; i++)
      GST_DEBUG_OBJECT (self, "#%d : %p", i, self->ringbuffer[i]);
  }
  GST_DEBUG_OBJECT (self, "Getting buffer %d", n);
  input = self->ringbuffer[n % self->ring_size];
  g_mutex_unlock (&self->ring_mutex);

  if (input == NULL) {
    GST_ERROR_OBJECT (self, "Buffer is NULL !");
    return NULL;
  }

  retframe =
      vsapi->newVideoFrame (self->vi.format, self->vi.width, self->vi.height,
      NULL, core);
  if (retframe == NULL) {
    GST_ERROR_OBJECT (self, "Failed to create a VSFrame");
    goto beach;
  }

  frameprops = vsapi->getFramePropsRW (retframe);
  /* Set the buffer properties ! */
  vsapi->propSetFloat (frameprops, "_AbsoluteTime",
      GST_BUFFER_PTS (input) / (double) GST_SECOND, 0);
  vsapi->propSetInt (frameprops, "_SARNum", self->gstvideoinfo.par_n, 0);
  vsapi->propSetInt (frameprops, "_SARDen", self->gstvideoinfo.par_d, 0);

  if (!gst_video_frame_map (&vframe, &self->gstvideoinfo, input, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map input buffer");
    vsapi->freeFrame (retframe);
    retframe = NULL;
    goto beach;
  }

  /* Copy input data into retframe */
  for (i = 0; i < self->vi.format->numPlanes; i++) {
    guint8 *writeptr = vsapi->getWritePtr (retframe, i);
    gint writestride = vsapi->getStride (retframe, i);
    vs_bitblt (writeptr, writestride, GST_VIDEO_FRAME_PLANE_DATA (&vframe, i),
        GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, i),
        GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, i),
        vsapi->getFrameHeight (retframe, i));
  }
  gst_video_frame_unmap (&vframe);
  GST_DEBUG_OBJECT (self, "Done");

beach:
  return retframe;
}

static void
gst_vapoursynth_input_filter_free (void *instanceData,
    VSCore * core, const VSAPI * vsapi)
{
  /* GstVapourSynth *self = (GstVapourSynth*) *instanceData; */

}

static void
debug_vs_map (VSMap * map)
{
  guint i, nb;

  nb = vsapi->propNumKeys (map);
  for (i = 0; i < nb; i++) {
    const gchar *key = vsapi->propGetKey (map, i);
    GST_DEBUG ("key #%d : '%s'", i, key);
  }
}

static GstVapourSynthPropertyDef *
get_property_def_for_id (GstVapourSynth * self, guint prop_id)
{
  GstVapourSynthClass *klass = GST_VAPOURSYNTH_GET_CLASS (self);
  guint i;

  for (i = 0; klass->properties[i]; i++) {
    if (klass->properties[i]->registered_type == prop_id)
      return klass->properties[i];
  }
  GST_ERROR_OBJECT (self, "Couldn't find property definition for property #%d",
      prop_id);
  return NULL;
}

static GstVapourSynthPropertyDef *
get_property_def_for_quark (GstVapourSynth * self, GQuark quark)
{
  GstVapourSynthClass *klass = GST_VAPOURSYNTH_GET_CLASS (self);
  guint i;

  for (i = 0; klass->properties[i]; i++) {
    if (klass->properties[i]->quark == quark)
      return klass->properties[i];
  }
  GST_ERROR_OBJECT (self, "Couldn't find property definition for quark '%s'",
      g_quark_to_string (quark));
  return NULL;
}

typedef struct
{
  VSMap *map;
  GstVapourSynth *object;
} PropMapData;

static gboolean
set_map_argument (GQuark field_id, const GValue * value, PropMapData * data)
{
  GstVapourSynthPropertyDef *pdef =
      get_property_def_for_quark (data->object, field_id);

  if (pdef == NULL)
    return FALSE;
  switch (pdef->type) {
    case VS_PROP_INT:
    {
      int val = g_value_get_int (value);
      GST_DEBUG ("Setting property '%s' to %d", pdef->prop_key, val);
      vsapi->propSetInt (data->map, pdef->prop_key, val, 0);
      break;
    }
    default:
      GST_FIXME_OBJECT (data->object, "Handle property '%s' yet",
          g_quark_to_string (field_id));
      break;
  }
  return TRUE;
}

static gboolean
fill_function_arguments (GstVapourSynth * self, VSMap * map)
{
  PropMapData data;
  data.map = map;
  data.object = self;

  gst_structure_foreach (self->properties,
      (GstStructureForeachFunc) set_map_argument, &data);
  return TRUE;
}

static void
gst_vapoursynth_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
}

static void
gst_vapoursynth_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVapourSynth *self = (GstVapourSynth *) object;
  GstVapourSynthPropertyDef *pdef;

  GST_DEBUG_OBJECT (self, "property #%d", prop_id);
  /* Get the GstVapourSynthPropertyDef */
  pdef = get_property_def_for_id (self, prop_id);
  if (pdef == NULL)
    return;
  gst_structure_id_set_value (self->properties, pdef->quark, value);
  GST_DEBUG_OBJECT (self, "properties are now %" GST_PTR_FORMAT,
      self->properties);
}


/* Extract property definition from the property triplet */
static gboolean
extract_property_type (gchar * orig, gchar ** prop_key, gboolean * is_array,
    GstVapourSynthPropertyType * prop_type, gboolean * is_optional)
{
  gchar **sub = g_strsplit (orig, ":", -1);
  gboolean ret = FALSE;

  GST_LOG ("Extracting property information from '%s'", orig);

  if (g_strv_length (sub) < 2)
    goto beach;

  /* property name */
  *prop_key = g_strdup (sub[0]);

  /* property type */
  *is_array = strstr (sub[1], "[]") != NULL;

  if (!g_ascii_strncasecmp (sub[1], "clip", 4))
    *prop_type = VS_PROP_CLIP;
  else if (!g_ascii_strncasecmp (sub[1], "int", 3))
    *prop_type = VS_PROP_INT;
  else if (!g_ascii_strncasecmp (sub[1], "float", 5))
    *prop_type = VS_PROP_FLOAT;
  else {
    GST_ERROR ("Unknown type '%s'", sub[1]);
    *prop_type = VS_PROP_UNKNOWN;
  }

  /* optional ? */
  *is_optional = sub[2] && !strcmp (sub[2], "opt");

  GST_LOG ("Property key:'%s', is_array:%d, prop_type:%d, is_optional:%d",
      *prop_key, *is_array, *prop_type, *is_optional);

  ret = TRUE;

beach:
  g_strfreev (sub);

  return ret;
}

static void
extract_property_information (GstVapourSynthClass * klass)
{
  VSMap *funcs;
  const gchar *props;
  gchar **splitprops;
  guint i;
  guint count = 1;
  gint errcode;
  GList *plist = NULL;
  GList *tmp;

  funcs = vsapi->getFunctions (klass->vsplugin);
  g_assert (funcs);

  props = vsapi->propGetData (funcs, klass->func_name, 0, &errcode);
  g_assert (props);

  /* Extract property definition */
  splitprops = g_strsplit (props, ";", -1);

  for (i = 1; splitprops[i] && splitprops[i][0]; i++) {
    GstVapourSynthPropertyDef *tofill;
    gboolean is_array, is_optional;
    GstVapourSynthPropertyType prop_type;
    gchar *prop_key;

    if (!extract_property_type (splitprops[i], &prop_key, &is_array, &prop_type,
            &is_optional))
      continue;
    if (is_array) {
      gint i;
      for (i = 0; i < 4; i++) {
        tofill = g_new0 (GstVapourSynthPropertyDef, 1);
        tofill->type = prop_type;
        tofill->optional = is_optional;
        tofill->name = g_strdup_printf ("%s-%d", prop_key, i);
        tofill->quark = g_quark_from_string (tofill->name);
        tofill->desc =
            g_strdup_printf ("%s #%d%s", prop_key, i,
            is_optional ? " (optional)" : "");
        tofill->prop_key = prop_key;
        tofill->prop_index = i;
        if (prop_type != VS_PROP_CLIP)
          tofill->registered_type = count++;
        else
          tofill->registered_type = -1;
        plist = g_list_append (plist, tofill);
      }
    } else {
      tofill = g_new0 (GstVapourSynthPropertyDef, 1);
      tofill->type = prop_type;
      tofill->optional = is_optional;
      tofill->name = g_strdup (prop_key);
      tofill->quark = g_quark_from_string (tofill->name);
      tofill->desc =
          g_strdup_printf ("%s%s", prop_key, is_optional ? " (optional)" : "");
      tofill->prop_key = prop_key;
      tofill->prop_index = 0;
      if (prop_type != VS_PROP_CLIP)
        tofill->registered_type = count++;
      else
        tofill->registered_type = -1;
      plist = g_list_append (plist, tofill);
    }
  }

  g_strfreev (splitprops);
  vsapi->freeMap (funcs);

  /* Convert to an array */
  klass->properties =
      g_new0 (GstVapourSynthPropertyDef *, g_list_length (plist) + 1);
  for (count = 0, tmp = plist; tmp; tmp = tmp->next, count++) {
    GstVapourSynthPropertyDef *src = (GstVapourSynthPropertyDef *) tmp->data;
    klass->properties[count] = src;
  }
  g_list_free (plist);
}

static void
install_properties (GstVapourSynthClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  guint i;

  for (i = 0; klass->properties[i]; i++) {
    GstVapourSynthPropertyDef *prop = klass->properties[i];
    GST_DEBUG ("Attempting to install property '%s' '%s' type:%d",
        prop->name, prop->desc, prop->registered_type);
    if (prop->registered_type != -1) {
      switch (prop->type) {
        case VS_PROP_INT:
          g_object_class_install_property (gobject_class, prop->registered_type,
              g_param_spec_int (prop->name, prop->desc, prop->desc, G_MININT,
                  G_MAXINT, 0, G_PARAM_READWRITE));
        case VS_PROP_CLIP:
        default:
          break;
      }
    }
  }
}

static void
gst_vapoursynth_class_init (GstVapourSynthClass * klass,
    GstVapourSynthClassData * klass_data)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  gchar *longname;
  GstPadTemplate *templ;

  klass->plugin_ns = klass_data->plugin_ns;
  klass->func_name = klass_data->func_name;

  longname =
      g_strdup_printf ("vapoursynth '%s/%s' filter", klass->plugin_ns,
      klass->func_name);
  gst_element_class_set_metadata (gstelement_class, longname,
      "Video/Filter", klass_data->plugin_desc,
      "Edward Hervey <bilboed@bilboed.com>");
  g_free (longname);

  GST_DEBUG ("Getting plugin '%s' '%s'", klass->plugin_ns,
      klass_data->plugin_desc);
  klass->vsplugin = vsapi->getPluginByNs (klass->plugin_ns, vscore);
  g_assert (klass->vsplugin != NULL);

  gobject_class->set_property = gst_vapoursynth_set_property;
  gobject_class->get_property = gst_vapoursynth_get_property;

  extract_property_information (klass);
  install_properties (klass);

  /* FIXME : Add pad templates */
  /* To figure out the pad templates, we should to probe the
   * filter with a whole range of input formats to see
   * which ones are accepted
   * The problem is going to arise with filters that will also
   * check other property values when creating them :( */
  templ =
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
      gst_caps_from_string ("video/x-raw,format={I420}"));
  gst_element_class_add_pad_template (gstelement_class, templ);
  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
      gst_caps_from_string ("video/x-raw,format={I420}"));
  gst_element_class_add_pad_template (gstelement_class, templ);
}

static GstFlowReturn
gst_vapoursynth_push_buffer (GstVapourSynth * self, const VSFrameRef * frameref)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *outbuf;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  GstVideoFrame vframe;
  gint par_n, par_d;
  const VSMap *frameprops;
  guint i;


  /* Print out frame properties */
  frameprops = vsapi->getFramePropsRO (frameref);
  debug_vs_map ((VSMap *) frameprops);
  pts =
      vsapi->propGetFloat (frameprops, "_AbsoluteTime", 0,
      NULL) * (double) GST_SECOND;
  par_n = vsapi->propGetInt (frameprops, "_SARNum", 0, NULL);
  par_d = vsapi->propGetInt (frameprops, "_SARDen", 0, NULL);
  /* vsapi->freeMap ((VSMap*) frameprops); */
  if (par_n != self->outvideoinfo.par_n || par_d != self->outvideoinfo.par_d) {
    GstCaps *outcaps;
    self->outvideoinfo.par_n = par_n;
    self->outvideoinfo.par_d = par_d;
    outcaps = gst_video_info_to_caps (&self->outvideoinfo);
    GST_DEBUG_OBJECT (self,
        "PAR changed, setting new output caps %" GST_PTR_FORMAT, outcaps);
    gst_pad_set_caps (self->srcpad, outcaps);
    gst_caps_unref (outcaps);
  }

  GST_DEBUG_OBJECT (self, "allocating output buffer of size %" G_GSIZE_FORMAT,
      GST_VIDEO_INFO_SIZE (&self->outvideoinfo));
  outbuf =
      gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (&self->outvideoinfo),
      NULL);
  GST_BUFFER_PTS (outbuf) = pts;
  gst_video_frame_map (&vframe, &self->outvideoinfo, outbuf, GST_MAP_WRITE);
  for (i = 0; i < self->out_vi->format->numPlanes; i++) {
    vs_bitblt (GST_VIDEO_FRAME_PLANE_DATA (&vframe, i),
        GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, i),
        vsapi->getReadPtr (frameref, i),
        vsapi->getStride (frameref, i),
        GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, i),
        vsapi->getFrameHeight (frameref, i));
  }
  gst_video_frame_unmap (&vframe);

  ret = gst_pad_push (self->srcpad, outbuf);

  return ret;
}

static GstFlowReturn
gst_vapoursynth_chain (GstPad * pad, GstVapourSynth * self, GstBuffer * buffer)
{
  GstFlowReturn ret = GST_FLOW_OK;
  const VSFrameRef *frameref;
  static char errmsg[40];

  if (G_UNLIKELY (self->actualfilter == NULL))
    goto not_negotiated;

  g_mutex_lock (&self->ring_mutex);
  while (self->head - self->tail >= self->ring_size) {
    GST_DEBUG_OBJECT (self, "Waiting for more room (head:%d, tail:%d)",
        self->head, self->tail);
    g_cond_wait (&self->write_cond, &self->ring_mutex);
  }
  GST_DEBUG_OBJECT (self, "Putting new buffer at head:%d", self->head);
  self->ringbuffer[self->head++ % self->ring_size] = buffer;
  {
    guint i;
    GST_DEBUG_OBJECT (self, "head:%d, tail:%d", self->head, self->tail);
    for (i = 0; i < self->ring_size; i++)
      GST_DEBUG_OBJECT (self, "#%d : %p", i, self->ringbuffer[i]);
  }
  g_cond_signal (&self->read_cond);
  g_mutex_unlock (&self->ring_mutex);

  frameref =
      vsapi->getFrame (self->out_frame_counter++, self->actualfilter, errmsg,
      40);
  if (frameref == NULL) {
    GST_ERROR_OBJECT (self, "Call to getFrame() returned nothing !");
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (self, "Yay, got a frame back \\o/");
  ret = gst_vapoursynth_push_buffer (self, frameref);

  return ret;

not_negotiated:
  {
    GST_ERROR_OBJECT (self, "No filter !");
    gst_buffer_unref (buffer);
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static gboolean
gst_vapoursynth_set_caps (GstVapourSynth * self, GstCaps * incaps)
{
  GstVapourSynthClass *klass = GST_VAPOURSYNTH_GET_CLASS (self);
  gboolean ret = FALSE;
  VSMap *map = vsapi->createMap ();
  const gchar *errstr = NULL;
  gint errnum;
  VSMap *in = vsapi->createMap ();
  VSMap *out = vsapi->createMap ();
  VSMap *invokeres;

  GST_DEBUG_OBJECT (self, "caps %" GST_PTR_FORMAT, incaps);

  /* 1. Convert caps to VSVideoInfo */
  gst_video_info_init (&self->gstvideoinfo);
  if (!gst_video_info_from_caps (&self->gstvideoinfo, incaps)) {
    GST_ERROR_OBJECT (self, "Can't get video info from caps %" GST_PTR_FORMAT,
        incaps);
    goto beach;
  }
  switch (GST_VIDEO_INFO_FORMAT (&self->gstvideoinfo)) {
    case GST_VIDEO_FORMAT_I420:
      self->vi.format = vsapi->getFormatPreset (pfYUV420P8, vscore);
      break;
    default:
      GST_ERROR_OBJECT (self, "UNHANDLED FORMAT '%s'",
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT
              (&self->gstvideoinfo)));
      goto beach;
  }
  self->vi.fpsNum = self->gstvideoinfo.fps_n;
  self->vi.fpsDen = self->gstvideoinfo.fps_d;
  self->vi.width = self->gstvideoinfo.width;
  self->vi.height = self->gstvideoinfo.height;
  self->vi.numFrames = 0;

  /* 2. Create input filter and invoke function on it */
  /* FIXME : Create the input filter when new caps are set on a pad
   * so that we have the video info */
  /* Create the fake input node */
  vsapi->createFilter (in, out, "BOGUSINPUT",
      gst_vapoursynth_input_filter_init,
      gst_vapoursynth_input_filter_get_frame,
      gst_vapoursynth_input_filter_free, fmUnordered, 0, self, vscore);

  GST_DEBUG_OBJECT (self, "Created input filter");
  debug_vs_map (in);
  debug_vs_map (out);
  vsapi->freeMap (in);

  GST_DEBUG_OBJECT (self, "Getting corresponding node");
  self->inputfilter = vsapi->propGetNode (out, "clip", 0, &errnum);
  vsapi->freeMap (out);
  if (self->inputfilter == NULL) {
    GST_ERROR_OBJECT (self, "Failed to create input filter");
    goto beach;
  }

  /* Set our input filter as the input of the filter we want to use */
  vsapi->propSetNode (map, "clip", self->inputfilter, 0);
  fill_function_arguments (self, map);

  invokeres = vsapi->invoke (klass->vsplugin, klass->func_name, map);
  errstr = vsapi->getError (invokeres);
  if (errstr) {
    GST_ERROR_OBJECT (self, "Error when invoking function : %s", errstr);
    goto beach;
  }

  self->actualfilter = vsapi->propGetNode (invokeres, "clip", 0, &errnum);

  GST_DEBUG ("invokeres");
  debug_vs_map (invokeres);
  GST_DEBUG ("map");
  debug_vs_map (map);
  vsapi->freeMap (invokeres);

  /* And now set downstream caps */
  if (self->out_vi == NULL) {
    GstVideoFormat out_format;
    GstCaps *outcaps;
    self->out_vi = vsapi->getVideoInfo (self->actualfilter);
    GST_LOG_OBJECT (self, "Creating output caps");
    gst_video_info_init (&self->outvideoinfo);
    switch (self->out_vi->format->id) {
      case pfYUV420P8:
        out_format = GST_VIDEO_FORMAT_I420;
        break;
      default:
        GST_ERROR_OBJECT (self, "Unknown output vsformat %d",
            self->out_vi->format->id);
        ret = GST_FLOW_NOT_NEGOTIATED;
        goto beach;
    }
    gst_video_info_set_format (&self->outvideoinfo, out_format,
        self->out_vi->width, self->out_vi->height);
    self->outvideoinfo.fps_n = self->out_vi->fpsNum;
    self->outvideoinfo.fps_d = self->out_vi->fpsDen;
    outcaps = gst_video_info_to_caps (&self->outvideoinfo);
    GST_DEBUG_OBJECT (self, "Setting output caps %" GST_PTR_FORMAT, outcaps);
    gst_pad_set_caps (self->srcpad, outcaps);
    gst_caps_unref (outcaps);
  }

  ret = TRUE;

beach:
  GST_DEBUG_OBJECT (self, "Returning %d", ret);
  return ret;
}

static gboolean
gst_vapoursynth_src_event (GstPad * pad, GstVapourSynth * self,
    GstEvent * event)
{
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    default:
      ret = gst_pad_event_default (pad, (GstObject *) self, event);
  }
  return ret;
}

static gboolean
gst_vapoursynth_sink_event (GstPad * pad, GstVapourSynth * self,
    GstEvent * event)
{
  gboolean ret = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      gst_event_parse_caps (event, &caps);
      ret = gst_vapoursynth_set_caps (self, caps);
      gst_event_unref (event);
    }
    default:
      ret = gst_pad_event_default (pad, (GstObject *) self, event);
  }
  return ret;

  return TRUE;
}

static void
gst_vapoursynth_init (GstVapourSynth * object)
{
  GstVapourSynthClass *klass = GST_VAPOURSYNTH_GET_CLASS (object);
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  object->srcpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template
      (gstelement_class, "src"), "src");
  gst_pad_set_event_function (object->srcpad,
      (GstPadEventFunction) gst_vapoursynth_src_event);
  gst_element_add_pad ((GstElement *) object, object->srcpad);

  object->sinkpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template
      (gstelement_class, "sink"), "sink");
  gst_pad_set_event_function (object->sinkpad,
      (GstPadEventFunction) gst_vapoursynth_sink_event);
  gst_pad_set_chain_function (object->sinkpad,
      (GstPadChainFunction) gst_vapoursynth_chain);
  gst_element_add_pad ((GstElement *) object, object->sinkpad);

  object->properties = gst_structure_new_empty ("vapoursynth-properties");
  g_mutex_init (&object->ring_mutex);
  g_cond_init (&object->read_cond);
  g_cond_init (&object->write_cond);
  object->ring_size = DEFAULT_RING_SIZE;
  object->cache_size = DEFAULT_CACHE_SIZE;
  object->ringbuffer = g_new0 (GstBuffer *, object->ring_size);
}


static gboolean
_load_filters (GstPlugin * gstplugin)
{
  gboolean ret = FALSE;
  const VSCoreInfo *coreinfo;
  VSMap *res;
  guint i, nbplug;
  const gchar *err;
  int errcode;
  GTypeInfo typeinfo = {
    sizeof (GstVapourSynthClass),
    (GBaseInitFunc) NULL,
    NULL,
    (GClassInitFunc) gst_vapoursynth_class_init,
    NULL,
    NULL,
    sizeof (GstVapourSynth),
    0,
    (GInstanceInitFunc) gst_vapoursynth_init,
  };
  GType type;
  gchar *type_name;

  vscore = vsapi->createCore (0);
  if (vscore == NULL) {
    GST_ERROR ("Couldn't get a vapoursynth core !");
    return FALSE;
  }
  coreinfo = vsapi->getCoreInfo (vscore);
  if (coreinfo == NULL) {
    GST_ERROR ("Couldn't get core info!");
    goto beach;
  }
  GST_DEBUG ("VapourSynth version '%s'", coreinfo->versionString);
  GST_DEBUG ("core:%d, api:0x%x, numThreads:%d", coreinfo->core, coreinfo->api,
      coreinfo->numThreads);
  GST_DEBUG ("maxFramebufferSize:%" G_GINT64_FORMAT,
      coreinfo->maxFramebufferSize);
  GST_DEBUG ("usedFramebufferSize:%" G_GINT64_FORMAT,
      coreinfo->usedFramebufferSize);

  res = vsapi->getPlugins (vscore);
  if (res == NULL) {
    GST_ERROR ("Couldn't get available plugins");
    goto beach;
  }

  /* REGISTER A NEW ELEMENT FOR EACH PLUGIN/FUNCTION PAIR */

  nbplug = vsapi->propNumKeys (res);
  err = vsapi->getError (res);
  if (err) {
    GST_ERROR ("Failed to get plugins '%s'", err);
  } else
    GST_DEBUG ("Number of plugins available : %d", nbplug);
  for (i = 0; i < nbplug; i++) {
    const gchar *key = vsapi->propGetKey (res, i);
    const gchar *data = vsapi->propGetData (res, key, 0, &errcode);
    if (key && data) {
      gchar **tripl = g_strsplit (data, ";", -1);
      VSPlugin *plugin;
      GST_DEBUG ("Plugin #%d : %s", i, key);
      GST_DEBUG ("   Namespace:'%s' , Identifier:'%s' , name:'%s'",
          tripl[0], tripl[1], tripl[2]);
      plugin = vsapi->getPluginById (tripl[1], vscore);
      if (plugin) {
        guint j, nbfunc;
        VSMap *funcs;
        GST_DEBUG ("Got plugin");
        funcs = vsapi->getFunctions (plugin);
        if (funcs) {
          nbfunc = vsapi->propNumKeys (funcs);
          for (j = 0; j < nbfunc; j++) {
            const gchar *key = vsapi->propGetKey (funcs, j);
            const gchar *data = vsapi->propGetData (funcs, key, 0, &errcode);
            GstVapourSynthClassData *klass_data;
            gchar **val;
            guint nbprop;
            GST_DEBUG ("  Function #%d : '%s' '%s'", j, key, data);
            val = g_strsplit (data, ";", -1);
            GST_DEBUG ("    Function name : '%s'", val[0]);
            for (nbprop = 1; val[nbprop] && val[nbprop][0]; nbprop++) {
              GST_DEBUG ("     Property #%d : %s", nbprop, val[nbprop]);
            }
            type_name = g_strdup_printf ("vapoursynth_%s_%s", tripl[0], val[0]);
            {
              guint x;
              for (x = 0; type_name[x]; x++)
                if (type_name[x] == '.')
                  type_name[x] = '_';
            }
            klass_data = g_new0 (GstVapourSynthClassData, 1);
            klass_data->func_name = g_strdup (val[0]);
            klass_data->plugin_ns = g_strdup (tripl[0]);
            klass_data->plugin_desc = g_strdup (tripl[2]);
            typeinfo.class_data = klass_data;
            type =
                g_type_register_static (GST_TYPE_ELEMENT, type_name, &typeinfo,
                0);
            gst_element_register (gstplugin, type_name, GST_RANK_NONE, type);
            g_strfreev (val);
          }
          vsapi->freeMap (funcs);
        }
      } else {
        GST_WARNING ("Couldn't get vapoursynth plugin '%s'", tripl[1]);
      }
      g_strfreev (tripl);
    } else
      GST_WARNING ("Failed to load a vapoursynth plugin (%s)", key);
  }

  vsapi->freeMap (res);
  ret = TRUE;
beach:
  return ret;
}

static void
gst_vapoursynth_message_handler (int msgType, const char *msg, void *user_data)
{
  GstDebugLevel level;

  switch (msgType) {
    case mtDebug:
      level = GST_LEVEL_DEBUG;
      break;
    case mtWarning:
      level = GST_LEVEL_WARNING;
      break;
    case mtCritical:
    case mtFatal:
    default:
      level = GST_LEVEL_ERROR;
      break;
  }
  GST_CAT_LEVEL_LOG (GST_CAT_DEFAULT, level, NULL, "%s", msg);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (vapoursynth_debug, "vapoursynth", 0, "vapoursynth");

  vsapi = getVapourSynthAPI (VAPOURSYNTH_API_VERSION);
  vsapi->setMessageHandler (gst_vapoursynth_message_handler, NULL);
  if (vsapi == NULL) {
    GST_ERROR ("Couldn't get VapourSynth API !");
    return FALSE;
  }
  return _load_filters (plugin);
#if 0
  gst_plugin_add_dependency_simple (plugin,
      "VAPOURSYNTH_PATH:HOME/.vapoursynth/lib",
      LIBDIR "/vapoursynth:"
      "/usr/lib/vapoursynth:/usr/local/lib/vapoursynth:"
      "/usr/lib32/vapoursynth:/usr/local/lib32/vapoursynth:"
      "/usr/lib64/vapoursynth:/usr/local/lib64/vapoursynth",
      NULL, GST_PLUGIN_DEPENDENCY_FLAG_RECURSE);

  plugin_names =
      g_hash_table_new_full ((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal,
      (GDestroyNotify) g_free, NULL);

  vapoursynth_path = g_getenv ("VAPOURSYNTH_PATH");
  if (vapoursynth_path && *vapoursynth_path) {
    gchar **p, **paths = g_strsplit (vapoursynth_path, ":",);

    for (p = paths; *p; p++) {
      register_plugins (plugin, plugin_names, *p, *p);
    }

    g_strfreev (paths);
  } else {
#define register_plugins2(plugin, pn, p) register_plugins(plugin, pn, p, p)
    homedir = g_get_home_dir ();
    path = g_build_filename (homedir, ".vapoursynth", "lib", NULL);
    libdir_path = g_build_filename (LIBDIR, "vapoursynth", NULL);
    register_plugins2 (plugin, plugin_names, path);
    g_free (path);
    register_plugins2 (plugin, plugin_names, libdir_path);
    g_free (libdir_path);
    register_plugins2 (plugin, plugin_names, "/usr/local/lib/vapoursynth");
    register_plugins2 (plugin, plugin_names, "/usr/lib/vapoursynth");
    register_plugins2 (plugin, plugin_names, "/usr/local/lib32/vapoursynth");
    register_plugins2 (plugin, plugin_names, "/usr/lib32/vapoursynth");
    register_plugins2 (plugin, plugin_names, "/usr/local/lib64/vapoursynth");
    register_plugins2 (plugin, plugin_names, "/usr/lib64/vapoursynth");
#undef register_plugins2
  }

  g_hash_table_unref (plugin_names);
#endif
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vapoursynth,
    "VapourSynth wrapper library",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
