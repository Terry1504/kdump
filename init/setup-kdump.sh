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
#%stage: boot
#%provides: kdump

#
# check if we are called with the -f kdump parameter
#
if ! (( $use_kdump )) ; then
    return 0
fi

#
# copy /etc/sysconfig/kdump
#
CONFIG=/etc/sysconfig/kdump

if [ ! -f "$CONFIG" ] ; then
    echo "kdump configuration not installed"
    return 1
fi

#
# read in the configuration
source "$CONFIG"

#
# replace the KDUMP_SAVEDIR with the resolved path
if [ "$kdump_protocol" = "file" ] ; then
    KDUMP_SAVEDIR="file://$kdump_path"
fi

#
# get the host key, if needed
if [ "$kdump_protocol" = "sftp" -a -z "$KDUMP_HOST_KEY" ] ; then
    KDUMP_HOST_KEY=$(ssh-keygen -F "$kdump_host" 2>/dev/null | \
                     awk '/^[^#]/ { if (NF==3) { print $3; exit } }')
    if [ -z "$KDUMP_HOST_KEY" ] ; then
        echo "WARNING: target SSH host key not found. " \
             "Man-in-the-middle attack is possible." >&2
    fi
fi

#
# copy the configuration file, modifying:
#   KDUMP_SAVEDIR  -> resolved path
#   KDUMP_HOST_KEY -> target host public key
mkdir -p ${tmp_mnt}/etc/sysconfig/
sed -e 's#^[ 	]*\(KDUMP_SAVEDIR\)=.*#\1="'"$KDUMP_SAVEDIR"'"#g' \
    -e 's#^[ 	]*\(KDUMP_HOST_KEY\)=.*#\1="'"$KDUMP_HOST_KEY"'"#g' \
    "$CONFIG" > "${tmp_mnt}${CONFIG}"

#
# add the host key explicitly if the option was missing
grep '^[ 	]*KDUMP_HOST_KEY=' ${tmp_mnt}${CONFIG} >/dev/null 2>&1 \
    || echo 'KDUMP_HOST_KEY="'"$KDUMP_HOST_KEY"'"' >> "${tmp_mnt}${CONFIG}"

#
# remember the host name
#
hostname >> ${tmp_mnt}/etc/hostname.kdump

#
# copy public and private key if needed
#
if [ "$kdump_protocol" = "sftp" ] ; then
    if [ -f /root/.ssh/id_dsa ] && [ -f /root/.ssh/id_dsa.pub ] ; then
        mkdir -p ${tmp_mnt}/.ssh
        cp /root/.ssh/id_dsa ${tmp_mnt}/.ssh
        cp /root/.ssh/id_dsa.pub ${tmp_mnt}/.ssh
    fi
    if [ -f /root/.ssh/id_rsa ] && [ -f /root/.ssh/id_rsa.pub ] ; then
        mkdir -p ${tmp_mnt}/.ssh
        cp /root/.ssh/id_rsa ${tmp_mnt}/.ssh
        cp /root/.ssh/id_rsa.pub ${tmp_mnt}/.ssh
    fi
fi

#
# copy required programs
#
for program in $KDUMP_REQUIRED_PROGRAMS ; do
    if [ ! -f "$program" ] ; then
        echo >&2 ">>> $program does not exist. Skipping!"
        continue
    fi

    dir=$(dirname "$program")
    mkdir -p "${tmp_mnt}/${dir}"
    cp_bin "$program" "${tmp_mnt}/${dir}"
done

save_var use_kdump
save_var kdump_fsmod

# vim: set sw=4 ts=4 et:
