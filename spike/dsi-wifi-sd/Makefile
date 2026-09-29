# SPDX-License-Identifier: Apache-2.0
# Throwaway spike (see docs/research/): confirms dswifi (BSD sockets, server side: listen/accept,
# not just the client GETs every published BlocksDS example does) and libfat (SD card) both work,
# together, in a sustained session, in DSi-enhanced mode, on real hardware. Not part of the product.

BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core

NAME	:= dsi-wifi-sd-spike

GAME_TITLE	:= NSD Bridge spike
GAME_SUBTITLE	:= wifi + SD smoke test

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS		:= -ldswifi9 -lnds9

LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
			   $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
