# SPDX-License-Identifier: Apache-2.0
# Isolation test: same console/SD code as dsi-wifi-sd, but the STANDARD ARM7 (no dswifi), to find
# out whether nds-bootstrap's white screen is caused specifically by the custom dswifi ARM7 binary.

BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core

NAME	:= dsi-sd-only-spike

GAME_TITLE	:= NSD Bridge spike
GAME_SUBTITLE	:= SD only, no wifi

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_minimal.elf
LIBS		:= -lnds9

LIBDIRS		:= $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
