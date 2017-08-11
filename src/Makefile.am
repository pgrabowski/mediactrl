dist_sysconf_DATA = stuff/configuration.xml.sample stuff/mycert.pem stuff/mycert.key

AM_CFLAGS = $(RESIP_CFLAGS)
AM_LDFLAGS = $(RESIP_LIBS)

if HAVE_GC
DEFS += -DUSE_GC
endif

if FFMPEG_ALTDIR
DEFS += -DFFMPEG_ALTDIR
AM_CPPFLAGS = -I/usr/include/ffmpeg
endif

SUBDIRS = codecs packages
bin_PROGRAMS = mediactrl
mediactrl_SOURCES = MediaCtrlMemory.h MediaCtrlCodec.h MediaCtrlCodec.cxx RemoteMonitor.cxx RemoteMonitor.h CfwStack.cxx CfwStack.h MediaCtrlClient.cxx MediaCtrlClient.h ControlPackage.cxx ControlPackage.h MediaCtrlEndpoint.cxx MediaCtrlEndpoint.h MediaCtrlSip.cxx MediaCtrlSip.h MediaCtrlRtp.cxx MediaCtrlRtp.h MediaCtrl.cxx MediaCtrl.h prototype.cxx
DEFS += -DDEFAULT_CONF_FILE='"$(sysconfdir)/configuration.xml"'

install-data-local:
	test -e "$(sysconfdir)/configuration.xml" || cat stuff/configuration.xml.sample | sed s/\\/usr\\/share/`echo $(datadir) | sed s:\\/:'\\''\\'\\/:g`/g > $(sysconfdir)/configuration.xml
	$(INSTALL_DATA) stuff/* $(pkgdatadir)/

uninstall-local:
	$(RM) -rf $(pkgdatadir)
