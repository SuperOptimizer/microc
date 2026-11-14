#!/bin/sh
# mkdesk.sh MCC DIR: build mdesk, the desktop for the Linux guest (desk/), against mcc's Linux C library;
# it lands in DIR/linux/mdesk, a static arm64 Linux program.
set -e
MCC=$1; D=$2
$MCC --target=aarch64-linux-gnu -o $D/linux/mdesk desk/desk.c
