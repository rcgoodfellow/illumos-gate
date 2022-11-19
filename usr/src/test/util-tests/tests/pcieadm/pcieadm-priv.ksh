#!/usr/bin/ksh
#
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2022 Oxide Computer Company
#

#
# Additional testing for pcieadm that requires us to actually have
# privileges rather than relying on existing pieces.
#

unalias -a
set -o pipefail

pcieadm_arg0="$(basename $0)"
pcieadm_prog="/usr/lib/pci/pcieadm"
pcieadm_tmp="/tmp/pcieadm-priv.$$"
pcieadm_bdf=""
pcieadm_dev=""
pcieadm_path=""

warn()
{
	typeset msg="$*"
	[[ -z "$msg" ]] && msg="failed"
	echo "TEST FAILED: $pcieadm_arg0: $msg" >&2
	pcieadm_exit=1
}

pcieadm_validate_filter()
{
	typeset filter="$1"

	if ! $pcieadm_prog show-devs $filter >/dev/null; then
		warn "failed to show-devs with filter $filter"
	else
		printf "TEST PASSED: show-devs $filter\n"
	fi

	if $pcieadm_prog show-devs $filter 9000/9000/9000; then
		warn "show-devs $filter 9000/9000/9000, should have failed"
	else
		printf "TEST PASSED: show-devs $filter 9000/9000/9000\n"
	fi

	if ! $pcieadm_prog show-cfgspace -d $filter >/dev/null; then
		warn "failed to show-cfgspace with filter $filter"
	else
		printf "TEST PASSED: show-cfgspace -d $filter\n"
	fi

	if ! $pcieadm_prog save-cfgspace -d $filter "$pcieadm_tmp/out.bin"; then
		warn "failed to use save-cfgspace -d $filter"
	else
		printf "TEST PASSED: save-cfgspace -d $filter\n"
	fi
}

#
# Before we begin execution, set up the environment such that we have a
# standard locale and that umem will help us catch mistakes.
#
export LC_ALL=C.UTF-8
export LD_PRELOAD=libumem.so
export UMEM_DEBUG=default

if [[ -n $PCIEADM ]]; then
	pcieadm_prog=$PCIEADM
fi

if ! $pcieadm_prog show-devs >/dev/null; then
	warn "failed to show devices"
else
	printf "successfully listed devices\n"
fi

if ! mkdir "$pcieadm_tmp"; then
	warn "failed to create temporary directory"
	exit $pcieadm_exit
fi

#
# Verify that we can grab things based on bdf
#
pcieadm_bdf=$($pcieadm_prog show-devs -p -o bdf | \
    awk '{ print $1; exit 0 }')
if [[ -z "$pcieadm_bdf" ]]; then
	warn "failed to obtain bdf based filter"
else
	pcieadm_validate_filter "$pcieadm_bdf"
fi

#
# Do the same based on the device instance.
#
pcieadm_dev=$($pcieadm_prog show-devs -p -o instance | \
    awk '{ if ($1 != "--") { print $1; exit 0 } }')
if [[ -z "$pcieadm_dev" ]]; then
	warn "failed to obtain driver based filter"
else
	pcieadm_validate_filter "$pcieadm_dev"
fi

#
# Verify based on the /devices path. Note, we use the device name to
# seed this as if there is no device driver attached, the path may
# overlap with another device on a PCI-only (non-express) based system.
#
pcieadm_path=$($pcieadm_prog show-devs -p -o path $pcieadm_dev | \
    awk '{ print $1; exit 0 }')
if [[ -z "$pcieadm_path" ]]; then
	warn "failed to obtain path based filter"
else
	pcieadm_validate_filter "$pcieadm_path"
fi

#
# Verify a bad filter doesn't work and results in an error.
#
if $pcieadm_prog show-devs /enoent >/dev/null; then
	warn "pcieadm succeeded with bad filter '/enoent'"
else
	printf "TEST PASSED: show-devs /enoent\n"
fi

if ! $pcieadm_prog save-cfgspace -a "$pcieadm_tmp" > /dev/null; then
	warn "failed to save all devices"
else
	printf "TEST PASSED: save-cfgspace -a\n"
fi

if (( pcieadm_exit == 0 )); then
	printf "All tests passed successfully!\n"
fi
rm -rf "$pcieadm_tmp"
exit $pcieadm_exit
