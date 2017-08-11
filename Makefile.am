SUBDIRS = src
DIST_SUBDIRS = src doc

install-data-local:
	@echo ""
	@echo ""
	@echo ""
	@echo "   Installation of the MEDIACTRL Prototype complete"
	@echo "------------------------------------------------------"
	@echo "(C) COMICS Research Group, http://www.comics.unina.it/"
	@echo ""
	@echo "Type 'make docs' if you want to build the doxygen documentation."
	@echo ""
	@echo "Don't forget to edit the configuration file before starting:"
	@echo "      $(sysconfdir)/configuration.xml"
	@echo ""
	@echo ""
	@echo ""

docs:
	$(MAKE) -C doc
