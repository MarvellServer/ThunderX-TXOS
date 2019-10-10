#!/usr/bin/env bash

set -e

export PS4='\[\033[0;33m\]+ ${BASH_SOURCE##*/}:${LINENO}:(${FUNCNAME[0]:-"?"}): \[\033[0;37m\]'
SCRIPTS_TOP=${SCRIPTS_TOP:-"$(cd "${BASH_SOURCE%/*}" && pwd)"}
script_name="${0##*/}"

versions="
	4.12
	4.13
	4.14
	4.15
	4.16
	4.17
	4.18
	4.19
	4.20
	5.0
	5.1
	5.1-refactor
	5.2-legacy
	5.2
	5.3
	5.4
"

for v in ${versions}; do
	echo "${script_name} ===> ${v}"
	rm -rf "${SCRIPTS_TOP}"/../ilp32--patches/ilp32-${v}-patches

	"${SCRIPTS_TOP}"/make-patches.sh --ver="${v}" --remote="origin" \
		-o "${SCRIPTS_TOP}"/../ilp32--patches/ilp32-${v}-patches -v

	#git -C "${SCRIPTS_TOP}"/../ilp32--patches add ilp32-${v}-patches
	#git -C "${SCRIPTS_TOP}"/../ilp32--patches commit -m "fu: ${v}"
done

