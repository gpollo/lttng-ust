AM_CPPFLAGS = -I$(top_srcdir)/include
AM_CFLAGS += -fno-strict-aliasing

lib_LTLIBRARIES = liblttng-ust-setuid.la
liblttng_ust_setuid_la_SOURCES = ustsetuid.c
liblttng_ust_setuid_la_LIBADD = \
	$(top_builddir)/liblttng-ust/liblttng-ust.la \
	$(DL_LIBS)

liblttng_ust_setuid_la_CFLAGS = -DUST_COMPONENT=liblttng-ust-setuid $(AM_CFLAGS)
