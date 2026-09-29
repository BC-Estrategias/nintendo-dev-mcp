# NSD Bridge — DSi build (BlocksDS). Real NDP protocol over wifi+SD, pairing (code + number
# comparison), the embedded web page, and the on-console folder editor -- full feature parity with
# the 3DS agent.
# Build:  export BLOCKSDS=/opt/wonderful/thirdparty/blocksds/core PATH=/opt/wonderful/bin:$PATH && make

BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core

NAME		:= nsd-bridge-dsi

GAME_TITLE	:= NSD Bridge
GAME_SUBTITLE	:= wireless SD card bridge
GAME_AUTHOR	:= BC Labs # same as the 3DS agent's APP_AUTHOR; BlocksDS's own default (its project URL) otherwise ends up in the banner shown by TWiLight Menu++/flashcart loaders
GAME_ICON	:= icon.png # downscaled from the 3DS agent's icon.png (48x48 -> 32x32), same branding

# platform code + the platform-neutral core + the shared BSD-socket server + the embedded web page
SOURCEDIRS	:= source ../common/src ../posix/src ../web
INCLUDEDIRS	:= ../common/include ../posix/include ../web

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS		:= -ldswifi9 -lnds9

LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
			   $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
