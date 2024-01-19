#!/bin/sh
P="NR7101_Generic"
echo "P=$P"
echo "copy $TOPDIR/configs/$P.defconfig to .config!"
cp $TOPDIR/configs/$P.defconfig $TOPDIR/.config
