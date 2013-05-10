#!/bin/bash
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
#%stage: filesystem
#%depends: storage
#%provides: kdump

#
# Read and normalize /etc/fstab and /proc/mounts (if exists).
# The following transformations are done:
#   - initial TABs and SPACEs are removed
#   - empty lines and comments are removed
#   - fields are separated by a single TAB
function read_mounts()
{
    local proc_mounts=/proc/mounts
    test -e $proc_mounts || proc_mounts=
    sed -e 's/[ \t][ \t]*/\t/g;s/^\t//;/^$/d;/^#/d' \
        "$root_dir"/etc/fstab $proc_mounts
}

#
# Find a mount point in /etc/fstab or /proc/mounts, checking the mounted
# device's major and minor numbers.
#
# Parameters: 1) mpoint:    mount point
# Output:
#                mntfstype: file system field from fstab
#                mntopts:   mount options from fstab
#                mntdev:    device name (preferably from fstab, with
#                           a fallback to)
function find_mount()
{
    local mpoint="$1"

    # default is "not found"
    mntdev=

    # get mntdev via stat
    local cpio major minor
    cpio=`echo "$mpoint" | /bin/cpio --quiet -o -H newc`
    major="$(echo $(( 0x${cpio:62:8} )) )"
    minor="$(echo $(( 0x${cpio:70:8} )) )"

    # get opts from fstab and device too if stat failed
    local fstab_device fstab_mountpoint fstab_type fstab_options dummy
    while read fstab_device fstab_mountpoint fstab_type fstab_options dummy
    do
        fstab_mountpoint="${fstab_mountpoint%/}"
        if [ "$fstab_mountpoint" = "$mpoint" ]; then
            # get major and minor
            update_blockdev "$fstab_device"
            # let's see if the stat device is the same as the fstab device
            if [ "$major" -le 0 ] || \
               [ "$blockmajor" -eq "$major" -a "$blockminor" -eq "$minor" ]
            then
                # if both match, use the fstab device so the user can decide
                # how to access the mounted device
                mntdev="$fstab_device"
            fi
            mntfstype="$fstab_type"
            mntopts="$fstab_options"
            break
        fi
    done < <(read_mounts)

    if [ "$major" -gt 0 -a -z "$mntdev" ] ; then
        # don't check for non-device mounts
        mntdev="$(majorminor2blockdev $major $minor)"
        update_blockdev $mntdev
        mntdev="$(beautify_blockdev $mntdev)"
    fi
}

#
# Get the mount modules for a given file system type
# Parameters: 1) fstype:  filesystem type
# Output:
#                mntmod:  modules required by the fs, if any
function get_mntmod()
{
    local fstype="$1"

    # We assume that we always have to load a module for the fs
    mntmod=$fstype

    # Check if we have to load a module for the fs type
    # XXX: This check should happen more generically for all modules
    if [ ! "$(find $root_dir/lib/modules/$kernel_version/ -name $mntmod.ko)" ]
    then
        if grep -q ${mntfstype}_fs_type "$map" ; then
            # No need to load a module, since this is compiled in
            mntmod=
        fi
    fi
}

#
# Add the mount helper to kdump_fsprog if necessary.
# Parameters: 1) fstype: filesystem type
# Global state:
#                kdump_fsprog: updated with the mount helper if needed
function add_mntprog()
{
    local fstype="$1"
    local helper="$root_dir/sbin/mount.${fstype}"
    if [ -e "$helper" ]; then
        kdump_fsprog="$kdump_fsprog $helper"
    fi
}

#
# Resolve which devices are necessary for a mount point
# Parameters: 1) desc:       human-readable description of the mount point
#             2) mntdir:     mount directory
#             3) mntdev:     device specification (optional)
# Output:
#                mntdev:     device specification
#                mntfstype:  file system type
#                mntopts:    mount options
#                mntmod:     modules needed to mount the device
#                mntjournal: journal device (if specified)
# Global state:
#                interface:  if empty on input, set to "default" for
#                            network filesystems
function resolve_mount()
{
    local desc="$1" mntdir="${2%/}"
    mntdev="$3"

    mntfstype=
    mntopts=
    if [ -z "$mntdev" ] ; then
        # mntdev not specified explicitly,
        # get it from the mount point
        find_mount "$mntdir"
    fi

    #if we don't know where the device belongs to
    if [ -z "$mntfstype" ] ; then
        # get type from /etc/fstab or /proc/mounts (actually not needed)
        local fstab_device fstab_mountpoint fstab_type fstab_options dummy
        while read fstab_device fstab_mountpoint fstab_type fstab_options dummy
        do
            if [ "$fstab_device" = "$mntdev" ] ; then
                mntfstype="$fstab_type"
                mntopts="$fstab_options"
                break
            fi
        done < <(read_mounts)
    fi

    # check for journal device
    mntjournal=
    if [ -n "$mntopts" ] ; then
        local jdev=${mntopts#*,jdev=}
        if [ "$jdev" != "$mntopts" ] ; then
            mntjournal=${jdev%%,*}
        fi
        local logdev=${mntopts#*,logdev=}
        if [ "$logdev" != "$mntopts" ] ; then
            mntjournal=${logdev%%,*}
        fi
    fi

    # check for nfs and set the mntfstype accordingly
    case "$mntdev" in
        /dev/nfs)
            mntfstype=nfs
            ;;
        /dev/*)
            if [ ! -e "$mntdev" ]; then
                error 1 "$desc device ($mntdev) not found"
            fi
            ;;
        *://*) # URL type
            mntfstype=${mntdev%%://*}
            interface=${interface:-default}
            ;;
        iscsi:*)
            ;;
        *:*)
            mntfstype=nfs
            interface=${interface:-default}
            ;;
    esac

    if [ -z "$mntfstype" ]; then
        eval $(udevadm info -q env -n $mntdev | sed -n '/ID_FS_TYPE/p' )
        [ $? -eq 0 ] && mntfstype=$ID_FS_TYPE
        [ "$mntfstype" = "unknown" ] && mntfstype=
        ID_FS_TYPE=
    fi

    if [ ! "$mntfstype" ]; then
        error 1 "Could not find the filesystem type for $desc device $mntdev

Currently available -d parameters are:
        Block devices   /dev/<device>
        NFS             <server>:<path>
        URL             <protocol>://<path>"
    fi

    get_mntmod "$mntfstype"
    add_mntprog "$mntfstype"
}

#
# Returns the UUID for a given disk
# Parameters: 1) partition device (e.g. /dev/sda5)
# Returns:    The UUID
function get_uuid_for_disk()                                               # {{{
{
    local uuid=
    local device=$1

    local output=$(/sbin/blkid ${device})
    local uuid=$(echo "$output" | sed -e 's/.*UUID="\([-a-fA-F0-9]*\)".*/\1/')
    echo "$uuid"
}                                                                          # }}}

#
# Adds an entry to a kdump fstab.
# Parameters: 1) fsname: file system name
#             2) mntdir: mount directory
#             3) fstype: file system type
#             4) opts:   mount options
#             5) freq:   dump frequency
#             6) passno: pass number on parallel fsck
function add_fstab()                                                       # {{{
{
    local fsname=$1
    local mntdir=$2
    local fstype=$3
    local opts=$4
    local freq=$5
    local passno=$6

    # translate block devices into UUID
    if [ -b "$fsname" ] ; then
        local uuid=$(get_uuid_for_disk "$fsname")
        if [ -n "$uuid" ] && [ -b "/dev/disk/by-uuid/$uuid" ] ; then
            fsname="/dev/disk/by-uuid/$uuid"
        fi
    fi

    # add "nolock" for NFS
    if [ "$fstype" = "nfs" ]; then
        opts="${opts},nolock"
    fi

    echo "$fsname $mntdir $fstype $opts $freq $passno" \
        >> ${tmp_mnt}/etc/fstab.kdump
}                                                                          # }}}

#######################################################################################

#
# check if we are called with the -f kdump parameter
#
use_kdump=
if use_script kdump ; then
    use_kdump=1
fi

if ! (( $use_kdump )) ; then
    return 0
fi

#
# get the save directory and protocol
#
kdump_max=0
eval "$( kdumptool print_target | \
    egrep '^(Protocol:|Realpath:|$)' | \
    sed -e "s/'/'\\\\''/g" \
	-e 's/^$/max=$((kdump_max+1))'\''/' \
	-e 's/^/kdump_/' \
	-e "s/: */[\$kdump_max]='/" \
	-e "s/\$/\'/" )"
if [ ${#kdump_Protocol[@]} -eq 0 ] ; then
    echo >&2 "kdumptool print_target failed."
    return 1
fi

kdump_fsmod=

#
# Check for network file systems
#
for protocol in "${kdump_Protocol[@]}" ; do
    if [ "$protocol" = "nfs" ]; then
        interface=${interface:-default}
        get_mntmod nfs
        kdump_fsmod="$kdump_fsmod $mntmod"
        add_mntprog nfs
    fi

    if [ "$protocol" = "cifs" -o "$protocol" = "smb" ]; then
        interface=${interface:-default}
        get_mntmod cifs
        kdump_fsmod="$kdump_fsmod $mntmod"
        add_mntprog cifs
    fi
done

#
# add mount points (below /root)
#

touch ${tmp_mnt}/etc/fstab.kdump
mnt_boot=
mnt_kdump=( )
while read device mountpoint filesystem opts dummy ; do
    if [ "$mountpoint" = "/boot" ] ; then
        mnt_boot="$mountpoint"
    fi

    local i=0
    while [ $i -le $kdump_max ] ; do
        protocol="${kdump_Protocol[i]}"
        realpath="${kdump_Realpath[i]}"
        if [ "$protocol" = "file" -a "$mountpoint" != "/" -a \
             "${realpath#$mountpoint}" != "$realpath" -a \
             "${#mountpoint}" -gt "${#mnt_kdump[i]}" ] ; then
            mnt_kdump[i]="$mountpoint"
        fi
        i=$((i+1))
    done
done < <(read_mounts)

# add the boot partition
if [ -n "$mnt_boot" ]
then
    mountpoint="$mnt_boot"
    resolve_mount "Boot directory" "$mountpoint"
    bootdev="$mntdev"
    add_fstab "$mntdev" "/root$mountpoint" "$mntfstype" "$mntopts" 0 0
    blockdev="$blockdev $(resolve_device Boot $mntdev)"
    kdump_fsmod="$kdump_fsmod $mntmod"
fi

# add the target file system
for mountpoint in "${mnt_kdump[@]}"
do
    resolve_mount "Dump directory" "$mountpoint"
    dumpdev="$mntdev"
    add_fstab "$mntdev" "/root$mountpoint" "$mntfstype" "$mntopts" 0 0
    blockdev="$blockdev $(resolve_device Dump $mntdev)"
    kdump_fsmod="$kdump_fsmod $mntmod"
done

# vim: set sw=4 ts=4 et:
