plugin_LTLIBRARIES = libgstlivesourcewrapper.la

# sources used to compile this plug-in
libgstlivesourcewrapper_la_SOURCES = gstlivesourcewrapper.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstlivesourcewrapper_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_CFLAGS)
libgstlivesourcewrapper_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) $(GST_LIBS)
libgstlivesourcewrapper_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstlivesourcewrapper_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

# headers we need but don't want installed
noinst_HEADERS = gstlivesourcewrapper.h
