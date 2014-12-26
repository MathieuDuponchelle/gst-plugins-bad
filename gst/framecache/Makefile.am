plugin_LTLIBRARIES = libgstframecache.la

libgstframecache_la_SOURCES = \
	gstframecache.c

libgstframecache_la_CFLAGS =  \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_BASE_CFLAGS) \
        $(GST_CFLAGS)

libgstframecache_la_LIBADD =  \
	$(GST_PLUGINS_BASE_LIBS) \
	$(GST_BASE_LIBS) $(GST_LIBS)

libgstcompositor_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstcompositor_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = \
		 gstframecache.h
