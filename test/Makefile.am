AM_CPPFLAGS =  -I$(top_srcdir)/src
LDADD = $(top_srcdir)/src/libckpool.a

bin_PROGRAMS = sha256 gridpool

TESTS = sha256 gridpool

sha256_SOURCES = sha256.c
gridpool_SOURCES = gridpool.c $(top_srcdir)/src/gridpool_adapter.c $(top_srcdir)/src/yyjson.c
#sha256_LDADD = libckpool.a
