pkglib_LTLIBRARIES = ommongodb.la
ommongodb_la_SOURCES = ommongodb.c
ommongodb_la_CPPFLAGS =  $(RSRT_CFLAGS) $(PTHREADS_CFLAGS) $(LIBMONGO_CLIENT_CFLAGS)
ommongodb_la_LDFLAGS = -module -avoid-version
ommongodb_la_LIBADD = $(LIBMONGO_CLIENT_LIBS)

EXTRA_DIST = 
