#
# Copyright (C) 2009 OpenWrt.org
#

SUBTARGET:=nr7101
BOARDNAME:=NR7101 Generic Outdoor 5G CPE
ARCH_PACKAGES:=ramips_24kec
FEATURES+=usb ubifs nand
CPU_TYPE:=24kec
CPU_SUBTYPE:=dsp

DEFAULT_PACKAGES +=

define Target/Description
	Build firmware images for NR7101 Generic Outdoor LTE CPE.
endef

