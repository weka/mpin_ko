#!/bin/bash
#
# This is a make helper script that finds the proper LINUX_HEADERS_PREFIX
# Makefile usage: makehelp.sh $(LINUXINCLUDE)
#	   $(LINUXINCLUDE) as defined by make
# Output on stdout the correct LINUX_HEADERS_PREFIX

_extract_dir_names()
{
	for p in $(echo "$@" ) ; do
		if [[ $p == \-I*  ]]; then
			echo ${p#*-I};
		fi ;
	done
}

find_linux_headers()
{
	linux_includes="$@"
	ret_code=17 # error code not found

	for f in $( _extract_dir_names $linux_includes ); do
		bn="$(basename $f)" ;
		if [ -e $f/linux/fs.h ] && [ $bn != "uapi" ] && [ $bn != "drm-backport" ]; then
			echo $f;
			ret_code=0; #good
		fi ;
	done

	return "$ret_code";
}

__LINUXINCLUDE="$@"

LINUX_HEADERS_PREFIX="$(find_linux_headers $__LINUXINCLUDE)"
if [ -z $LINUX_HEADERS_PREFIX ]; then
	echo "makehelp failed [$__LINUXINCLUDE]"
	exit 17
fi

echo $LINUX_HEADERS_PREFIX
exit 0
