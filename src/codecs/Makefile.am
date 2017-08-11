libdir = $(pkgdatadir)/codecs

if HAVE_GC
DEFS += -DUSE_GC
endif

if FFMPEG_ALTDIR
DEFS += -DFFMPEG_ALTDIR
AM_CPPFLAGS = -I/usr/include/ffmpeg
endif

INCLUDES = -I../
lib_LTLIBRARIES = libUlawCodec.la libAlawCodec.la libGsmCodec.la
libUlawCodec_la_SOURCES = UlawCodec.cxx ../MediaCtrlCodec.cxx
libAlawCodec_la_SOURCES = AlawCodec.cxx ../MediaCtrlCodec.cxx
libGsmCodec_la_SOURCES = GsmCodec.cxx ../MediaCtrlCodec.cxx
libUlawCodec_la_LDFLAGS = -version-info 4:0:0
libAlawCodec_la_LDFLAGS = -version-info 4:0:0
libGsmCodec_la_LDFLAGS = -version-info 4:0:0

uninstall-local:
	$(RM) -r $(pkgdatadir)/codecs/*
