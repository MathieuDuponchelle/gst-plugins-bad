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
  /* Create the filter */
}

static const VSFrameRef *
gst_vapoursynth_input_filter_get_frame (int n, int activationReason,
    void **instanceData, void **frameData, VSFrameContext * frameCtx,
    VSCore * core, const VSAPI * vsapi)
{
  GstVapourSynth *self = (GstVapourSynth *) * instanceData;
  GST_DEBUG_OBJECT (self,
      "n:%d, activationReason:%d, frameData:%p, frameCtx:%p", n,
      activationReason, frameData, frameCtx);
  return NULL;
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

static void
gst_vapoursynth_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
}

static void
gst_vapoursynth_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
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
  /* To figure out the pad templates, we need to probe the
   * filter with a whole range of input formats to see
   * which ones are accepted
   * The problem is going to arise with filters that will also
   * check other property values when creating them :( */
}

static void
gst_vapoursynth_init (GstVapourSynth * object)
{
  GstVapourSynthClass *klass = GST_VAPOURSYNTH_GET_CLASS (object);
  VSMap *map = vsapi->createMap ();
  guint i, nb;
  const gchar *errstr = NULL;
  gint errnum;
  VSMap *in = vsapi->createMap ();
  VSMap *out = vsapi->createMap ();

  /* FIXME : Create the input filter when new caps are set on a pad
   * so that we have the video info */
  /* Create the fake input node */
  vsapi->createFilter (in, out, "BOGUSINPUT",
      gst_vapoursynth_input_filter_init,
      gst_vapoursynth_input_filter_get_frame,
      gst_vapoursynth_input_filter_free, fmUnordered, 0, object, vscore);

  object->inputfilter = vsapi->propGetNode (out, "clip", 0, &errnum);
  g_assert (object->inputfilter);

  vsapi->propSetNode (map, "clip", object->inputfilter, 0);

  object->invokeres = vsapi->invoke (klass->vsplugin, klass->func_name, map);
  errstr = vsapi->getError (object->invokeres);
  if (errstr) {
    GST_ERROR_OBJECT (object, "Error when invoking function : %s", errstr);
  }
  nb = vsapi->propNumKeys (object->invokeres);
  GST_DEBUG ("result %d", nb);
  for (i = 0; i < nb; i++) {
    const gchar *key = vsapi->propGetKey (object->invokeres, i);
    GST_DEBUG ("key #%d : %s", i, key);
  }
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

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (vapoursynth_debug, "vapoursynth", 0, "vapoursynth");

  vsapi = getVapourSynthAPI (VAPOURSYNTH_API_VERSION);
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
