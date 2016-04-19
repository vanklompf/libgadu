#!/usr/bin/make -f

.PHONY: all clean

# Deliberately not adding coverity here.
all:
	$(MAKE) -C check-style
	$(MAKE) -C debian
	$(MAKE) -C fedora
	$(MAKE) -C opensuse
	$(MAKE) -C scan-build
	$(MAKE) -C ubuntu
	$(MAKE) -C windows

clean:
	$(MAKE) -C check-style clean
	$(MAKE) -C coverity clean
	$(MAKE) -C debian clean
	$(MAKE) -C fedora clean
	$(MAKE) -C opensuse clean
	$(MAKE) -C scan-build clean
	$(MAKE) -C ubuntu clean
	$(MAKE) -C windows clean