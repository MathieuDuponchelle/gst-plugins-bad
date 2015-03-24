plugin_LTLIBRARIES = libgstvapoursynth.la

libgstvapoursynth_la_SOURCES = \
	gstvapoursynth.c 

libgstvapoursynth_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_BASE_CFLAGS) $(GST_CFLAGS) $(VAPOURSYNTH_CFLAGS)
libgstvapoursynth_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) -lgstvideo-@GST_API_VERSION@ \
	$(GST_BASE_LIBS) $(GST_LIBS) $(GMODULE_NO_EXPORT_LIBS) $(VAPOURSYNTH_LIBS)
libgstvapoursynth_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstvapoursynth_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = gstvapoursynth.h

Android.mk: Makefile.am $(BUILT_SOURCES)
	androgenizer \
	-:PROJECT libgstvapoursynth -:SHARED libgstvapoursynth \
	 -:TAGS eng debug \
         -:REL_TOP $(top_srcdir) -:ABS_TOP $(abs_top_srcdir) \
	 -:SOURCES $(libgstvapoursynth_la_SOURCES) \
	 -:CFLAGS $(DEFS) $(DEFAULT_INCLUDES) $(libgstvapoursynth_la_CFLAGS) \
	 -:LDFLAGS $(libgstvapoursynth_la_LDFLAGS) \
	           $(libgstvapoursynth_la_LIBADD) \
	           -ldl \
	 -:PASSTHROUGH LOCAL_ARM_MODE:=arm \
		       LOCAL_MODULE_PATH:='$$(TARGET_OUT)/lib/gstreamer-0.10' \
	> $@
