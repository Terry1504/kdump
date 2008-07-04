#!/bin/bash
#
# (c) 2008, Bernhard Walle <bwalle@suse.de>, SUSE LINUX Products GmbH
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.
#

#
# Define the testing results and test kernels here                           {{{
#

KERNEL_IMAGES=("kernel-bzImage-x86_64"
               "kernel-ELFgz-x86_64"
               "kernel-ELF-x86_64"
               )
RELOCATABLE=(  1
               0
               0
               1 )
TYPE=(         "x86"
               "ELF gzip"
               "ELF"
               "ELF gzip" )

                                                                           # }}}

#
# Program                                                                    {{{
#

KDUMPTOOL=$1
DIR=$2

if [ -z "$DIR" ] || [ -z "$KDUMPTOOL" ] ; then
    echo "Usage: $0 kdumptool directory"
    exit 1
fi

errornumber=0
i=0
for kernel in ${KERNEL_IMAGES[@]}; do
    path=$DIR/$kernel
    reloc=${RELOCATABLE[$i]}
    type=${TYPE[$i]}

    if [ ! -f "$path" ] ; then
        echo "$path" does not exist
        errornumber=$[$errornumber+1]
        continue
    fi

    reloc_output=$($KDUMPTOOL identify_kernel -r $path)
    reloc_exit=$?

    type_output=$($KDUMPTOOL identify_kernel -t $path)
    type_exit=$?

    if (( $reloc )) ; then
        if [ "$reloc_output" != "Relocatable" ] ; then
            echo "$kernel is relocatable but kdumptool says it's '$reloc_output'"
            errornumber=$[$errornumber+1]
            continue
        fi
        if [ "$reloc_exit" -ne 0 ] ; then
            echo "$kernel is relocatable but exitstatus is '$reloc_exit'"
            errornumber=$[$errornumber+1]
            continue
        fi
    else
        if [ "$reloc_output" != "Not relocatable" ] ; then
            echo "$kernel is not relocatable but kdumptool says it is '$reloc_output'"
            errornumber=$[$errornumber+1]
            continue
        fi
        if [ "$reloc_exit" -ne 2 ] ; then
            echo "$kernel is relocatable but exitstatus is '$reloc_exit'"
            errornumber=$[$errornumber+1]
            continue
        fi
    fi

    i=$[$i+1]
done

if [ "$errornumber" -eq 0 ] ; then
    echo "PASSED"
else
    echo "$errornumber FAILED"
fi

exit $errornumber

# vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
