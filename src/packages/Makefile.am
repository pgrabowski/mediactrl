libdir = $(pkgdatadir)/packages

if HAVE_GC
DEFS += -DUSE_GC
endif

if FFMPEG_ALTDIR
DEFS += -DFFMPEG_ALTDIR
AM_CPPFLAGS = -I/usr/include/ffmpeg
endif

INCLUDES = -I../
lib_LTLIBRARIES = libExamplePackage.la libIvrPackage.la libMixerPackage.la
libExamplePackage_la_SOURCES = ExamplePackage.cxx ../ControlPackage.cxx ../MediaCtrlCodec.cxx
libIvrPackage_la_SOURCES = IvrPackage.cxx ../ControlPackage.cxx ../MediaCtrlCodec.cxx
libMixerPackage_la_SOURCES = MixerPackage.cxx ../ControlPackage.cxx ../MediaCtrlCodec.cxx
libExamplePackage_la_LDFLAGS = -version-info 4:0:0
libIvrPackage_la_LDFLAGS = -version-info 4:0:0
libMixerPackage_la_LDFLAGS = -version-info 4:0:0

uninstall-local:
	$(RM) -r $(pkgdatadir)/packages/*
