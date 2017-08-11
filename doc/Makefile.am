dist_pkgdata_DATA = mediactrl-doxygen.cfg mediactrl-doxygen-footer.html draft-ietf-mediactrl-call-flows-00.txt draft-ietf-mediactrl-sip-control-framework-10.txt draft-boulton-mmusic-sdp-control-package-attribute-03.txt draft-ietf-mediactrl-ivr-control-package-06.txt draft-ietf-mediactrl-mixer-control-package-06.txt

if HAVE_DOXYGEN
pkgdocdir = $(datadir)/doc/$(PACKAGE)-$(VERSION)
htmldocdir = $(pkgdocdir)/html

all-local: force_look
	doxygen mediactrl-doxygen.cfg

install-data-local:
	@echo "Installing documentation..."
	test -z "$(DESTDIR)$(htmldocdir)" || $(MKDIR_P) "$(DESTDIR)$(htmldocdir)"
	$(INSTALL_DATA) html/* $(DESTDIR)$(htmldocdir)

uninstall-local:
	$(RM) -r $(DESTDIR)$(htmldocdir)
	$(RM) -r $(DESTDIR)$(pkgdocdir)

clean-local:
	$(RM) -r html latex man

force_look:
	true
endif
