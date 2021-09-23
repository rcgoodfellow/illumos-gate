#! /bin/bash

set -eu
set -o pipefail

typeset -i opt_s rdsize

opt_m=
opt_r=
opt_s=

rdsize=
lofi_dev=
tmp_file=
mnt_dir=
manifest=

basedir="$(cd $(dirname $0) && pwd)"
DEFAULT_MANIFEST="${basedir}/ramdisk.manifest"

function usage()
{
	printf 'Usage: %s [-m manifest] [-r proto] [-s size]\n' "$0" >&2
	printf '\t-m manifest\t<manifest> lists files to include\n' >&2
	printf '\t  Default: %s\n' "${DEFAULT_MANIFEST}" >&2
	printf '\t-r proto\tfind illumos contents in <proto>\n' >&2
	printf '\t  Default: ${ROOT} [%s]\n' "${ROOT:-<unset>}" >&2
	printf '\t-s size\t\tcreate a filesystem of <size> KiB\n' >&2
	printf '\t  Default: ${MKRD_SIZE} [%d]\n' "${MKRD_SIZE:-0}" >&2
	exit 2
}

function cleanup()
{
	[ -n "${mnt_dir}" -a -d "${mnt_dir}" ] && \
	    pfexec umount -- "${mnt_dir}" || true
	[ -n "${mnt_dir}" -a -d "${mnt_dir}" ] && \
	    rmdir -- "${mnt_dir}" || true
	mnt_dir=
	[ -n "${lofi_dev}" ] && pfexec lofiadm -d "${lofi_dev}" || true
	lofi_dev=
}

function on_error()
{
	cleanup || true
	[ -z "${tmp_file}" -a -f "${tmp_file}" ] && rm -- "${tmp_file}" || true
}

while getopts "m:r:s:" opt; do
	case "${opt}" in
	m)
		opt_m="${OPTARG}"
		;;
	r)
		opt_r="${OPTARG}"
		;;
	s)
		opt_s="${OPTARG}"
		;;
	-)
		break
		;;
	\?)
		usage
		;;
	esac
done

shift $(( ${OPTIND} - 1 ))
if [ "$#" != 0 ]; then
	usage
fi

manifest="${opt_m:-${DEFAULT_MANIFEST}}"
if [ -z "${manifest}" -o ! -r "${manifest}" ]; then
	printf 'manifest [%s] not readable\n' "${manifest}" >&2
	usage
fi

proto="${opt_r:-${ROOT:-''}}"
if [ -z "${proto}" -o ! -d "${proto}" ]; then
	printf 'proto area [%s] is not a directory\n' "${proto}" >&2
	usage
fi

rdsize="${opt_s:-${MKRD_SIZE}}"
(( rdsize == 0 )) 2>/dev/null && rdsize=
if [ -z "${rdsize}" -o $(( rdsize )) = 0 ]; then
	printf 'size [%d] must be a nonzero integer\n' "${rdsize}" >&2
	usage
fi

trap cleanup EXIT
trap on_error ERR

#
# All that preamble and faffing about is ancillary to this, which does nothing
# more or less than create an empty UFS filesystem in a file, mount it, fill it
# with the contents of the proto area as specified by the manifest, and unmount
# it.
#

mnt_dir="$(mktemp -td "mnt.XXXXXX")"
tmp_file="$(mktemp -t "ramdisk.ufs.XXXXXX")"
mkfile "${rdsize}k" "${tmp_file}" >/dev/null
lofi_dev="$(pfexec lofiadm -a "${tmp_file}")"
pfexec newfs -o space -m 0 -i 12248 -b 4096 "${lofi_dev}" </dev/null >/dev/null
pfexec mount -F ufs -o nologging "${lofi_dev}" "${mnt_dir}" >/dev/null
if ! (cd "${proto}" && tar cf - -I "${manifest}" | \
    pfexec tar xf - -C "${mnt_dir}"); then
	printf 'population failed; missing file or size too small?\n' >&2
	false
fi
printf '%s\n' "${tmp_file}"
unset tmp_file

# Cleanup is done for us
exit 0
