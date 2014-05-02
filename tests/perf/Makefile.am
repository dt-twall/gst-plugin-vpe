plugin_LTLIBRARIES = libgstperf.la

# headers we need but don't want installed
noinst_HEADERS = \
	gstperf.h

# sources used to compile this plug-in
libgstperf_la_SOURCES = \
	gstperf.c
	$(noinst_HEADERS)

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstperf_la_CFLAGS = \
	$(GST_CFLAGS)

libgstperf_la_LIBADD = \
	$(GST_LIBS)

libgstperf_la_LDFLAGS = \
	$(GST_PLUGIN_LDFLAGS) \
	$(GST_ALL_LDFLAGS) \
	--no-undefined

libgstperf_la_LIBTOOLFLAGS = --tag=disable-static

