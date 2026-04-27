# Optimized Haiku Build Script
SHELL := /bin/bash

PACKAGE_DIR := build/package
NAME = JAMin
name = jamin
VERSION = 0.98.9

.PHONY: config build clean deepclean package release deps help

UNAME_M := $(shell uname -p)
ifeq ($(UNAME_M), x86)
CXX = g++-x86 
CC = gcc-x86
MAKE := setarch x86 $(MAKE)
ARCH = x86_gcc2
SIMD_FLAGS := -O2
INCLUDE = -L/boot/system/lib/x86 
LDFLAGS = -L/boot/system/develop/lib/x86 -L/boot/system/lib/x86 
CPPFLAGS = -I/boot/system/develop/headers/x86 -I$(PWD)
PKG_CONFIG_PATH = /boot/system/develop/lib/x86/pkgconfig
CFLAGS += $(PACKAGE_CFLAGS)
REQUIRED_RUNTIME_PKGS = fftw gtk3_x86 libxml2_x86 glib2_x86
REQUIRED_BUILD_PKGS = fftw_devel gtk3_x86_devel libxml2_x86_devel gettext make automake autoconf libtool_x86 intltool pkgconfig glib2_x86_devel
packageDir = /lib/x86
tpl = 32.tpl
else ifeq ($(UNAME_M), x86_64)
CXX = g++
CC = gcc
ARCH = x86_64
SIMD_FLAGS := -O3
INCLUDE = -L/boot/system/lib
LDFLAGS = -L/boot/system/develop/lib/ 
CPPFLAGS = -I$(PWD)
PKG_CONFIG_PATH = /boot/system/develop/lib/pkgconfig
REQUIRED_RUNTIME_PKGS = fftw gtk3 libxml2 glib2
REQUIRED_BUILD_PKGS = fftw_devel gtk3_devel libxml2_devel gettext make automake autoconf libtool intltool pkgconfig glib2_devel
packageDir = /lib
tpl = 64.tpl
endif



# CPU Features - Use native if building for personal use and maximum local speed	
# SIMD_FLAGS :=  -O3 -march=native
SIMD_FLAGS ?= -O3 -mtune=generic # Public Build Default



BUILD_FLAGS = $(SIMD_FLAGS) 
LD_OPTIMIZE = -Wl,--gc-sections


EXTRA_LIBS = -ljack
HAIKU_LIBS = -lmedia -lbe -lroot


config:
	./configure JACK_CFLAGS="-I." JACK_LIBS="-ljack" PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" \
	--datarootdir=/boot/system/apps/$(NAME)


all: build

build:
	@echo "=============================================="
	@echo "    Building $(NAME) for Haiku $(ARCH) $(SIMD_FLAGS)"
	@echo "=============================================="
	$(CXX) -shared -fPIC jack_stubs.cpp -o libjack.so $(HAIKU_LIBS)
	mkdir -p /boot/home/config/non-packaged/lib
	cp libjack.so /boot/home/config/non-packaged/lib
	$(MAKE) -f Makefile CFLAGS="-fcommon -I." \
	LIBS="-L/boot/home/config/non-packaged/lib $(INCLUDE) -ljack $(HAIKU_LIBS)"	


release: config build package

package: all
	@[ -n "$(PACKAGE_DIR)" ] || { echo "PACKAGE_DIR is undefined"; exit 1; }
	rm -rf "./$(PACKAGE_DIR)"
	mkdir -p $(PACKAGE_DIR)
	sed -e 's/$$(NAME)/$(NAME)/g' -e 's/$$(VERSION)/$(VERSION)/g' -e 's/$$(ARCH)/$(ARCH)/' -e 's/$$(YEAR)/$(shell date +%Y)/' $(name)$(tpl) > $(PACKAGE_DIR)/.PackageInfo 
	mkdir -p $(PACKAGE_DIR)/apps/$(NAME)/data
	mkdir -p $(PACKAGE_DIR)/$(packageDir) 
	mkdir -p $(PACKAGE_DIR)/apps/$(NAME)/jamin/examples
	mkdir -p $(PACKAGE_DIR)/apps/$(NAME)/pixmaps
	mkdir -p $(PACKAGE_DIR)/apps/$(NAME)/controller
	mkdir -p $(PACKAGE_DIR)/apps/$(NAME)/license
	mkdir -p $(PACKAGE_DIR)/lib/ladspa
	mkdir -p $(PACKAGE_DIR)/bin  
	mkdir -p $(PACKAGE_DIR)/data/deskbar/menu/Applications
	rc -o $(name).rsrc $(name).rdef 
	xres -o src/$(name) $(name).rsrc  
	mimeset -f src/$(name)
	copyattr -n BEOS:ICON src/jamin JAMin_Launcher
	copyattr -n BEOS:M:STD_ICON src/jamin JAMin_Launcher
	copyattr -n BEOS:L:STD_ICON src/jamin JAMin_Launcher
	mimeset -f JAMin_Launcher
	chmod +x JAMin_Launcher
	cp JAMin_Launcher $(PACKAGE_DIR)/apps/$(NAME)/
	cp COPYING $(PACKAGE_DIR)/apps/$(NAME)/license/license			
	cp examples/*.jam $(PACKAGE_DIR)/apps/$(NAME)/jamin/examples
	cp controller/*.{dtd,xml} $(PACKAGE_DIR)/apps/$(NAME)/controller
	cp data/*.xml $(PACKAGE_DIR)/apps/$(NAME)/data
	cp pixmaps/*.{png,xpm,jpg} $(PACKAGE_DIR)/apps/$(NAME)/pixmaps
	cp libjack.so $(PACKAGE_DIR)/$(packageDir) 
	#[[ -e /boot/home/config/non-packaged/lib/ladspa ]] && ln -s /boot/home/config/non-packaged/lib/ladspa $(PACKAGE_DIR)/lib/ladspa
	cp -r /boot/home/config/non-packaged/lib/ladspa $(PACKAGE_DIR)/lib/
	cp src/$(name) $(PACKAGE_DIR)/apps/$(NAME)/$(NAME)
	ln -s ../apps/$(NAME)/JAMin_Launcher $(PACKAGE_DIR)/bin/$(name)
	ln -s ../../../../apps/$(NAME)/JAMin_Launcher $(PACKAGE_DIR)/data/deskbar/menu/Applications/$(NAME)
	package create -C $(PACKAGE_DIR) $(NAME)-$(VERSION)-1-$(ARCH).hpkg
	# Just need this to build so will remove it now
	rm /boot/home/config/non-packaged/lib/libjack.so

clean:
	make clean
	@rm -f $(NAME) libjack.so src/libjack.so $(NAME).rsrc $(NAME)-$(VERSION)-1-$(ARCH).hpkg
	@rm -rf build
	@echo "Cleanup complete."

deepclean:
	make clean
	@rm -f $(NAME) libjack.so src/libjack.so $(NAME).rsrc $(NAME)-$(VERSION)-1-$(ARCH).hpkg
	@rm -rf build
	autoreconf -vif
	@echo "Deep cleanup complete."
	
deps:
	@echo "Install these via pkgman to build source:"
	@echo "pkgman install $(REQUIRED_BUILD_PKGS)"     	

help:
	@echo "============================================================================"
	@echo " Building $(NAME) for Haiku $(ARCH) $(SIMD_FLAGS)"
	@echo ""
	@echo ""
	@echo " 1. Default Build: make -f haiku.makefile release"
	@echo ""
	@echo " 2. Custom Build:"
	@echo "     make -fhaiku.makefile clean "
	@echo "     make -f haiku.makefile config"
	@echo "     make -f haiku.makefile release SIMD_FLAGS=\"-O3 -march=native\""
	@echo ""
	@echo " 3. Build: make -f haiku.makefile"
	@echo ""
	@echo " 4. Package: make -f haiku.makefile package"
	@echo ""
	@echo " 5. Custom Package: make -f haiku.makefile release SIMD_FLAGS=\"-O3 -march=native\""
	@echo ""
	@echo " 6. Clean: make -f haiku.makefile clean"
	@echo ""
	@echo " 7. List Required Libs: make -f haiku.makefile deps"
	@echo ""
	@echo "============================================================================"	

