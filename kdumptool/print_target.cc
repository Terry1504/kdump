/*
 * (c) 2008, Bernhard Walle <bwalle@suse.de>, SUSE LINUX Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#include <iostream>
#include <string>

#include "fileutil.h"
#include "subcommand.h"
#include "debug.h"
#include "print_target.h"
#include "util.h"
#include "configuration.h"
#include "rootdirurl.h"
#include "stringutil.h"

using std::string;
using std::cout;
using std::cerr;
using std::endl;

//{{{ PrintTarget --------------------------------------------------------------------

/**
 * Global instance, automatically registered in the global subcommand list.
 */
static PrintTarget globalInstance;

// -----------------------------------------------------------------------------
PrintTarget::PrintTarget()
    throw ()
    : m_rootdir()
{
    m_options.push_back(new StringOption("root", 'R', &m_rootdir,
        "Use the specified root directory instead of /."));
}

// -----------------------------------------------------------------------------
const char *PrintTarget::getName() const
    throw ()
{
    return "print_target";
}

// -----------------------------------------------------------------------------
void PrintTarget::execute()
    throw (KError)
{
    Debug::debug()->trace("PrintTarget::execute()");
    Debug::debug()->dbg("root: %s", m_rootdir.c_str());

    Configuration *config = Configuration::config();

    RootDirURLVector urlv(
	config->getStringValue(Configuration::KDUMP_SAVEDIR), m_rootdir);
    RootDirURLVector::iterator it;
    for (it = urlv.begin(); it != urlv.end(); ++it) {
	if (it != urlv.begin())
	    cout << endl;
	print_one(*it);
    }
}

// -----------------------------------------------------------------------------
void PrintTarget::print_one(RootDirURL &parser)
    throw (KError)
{
    string port;

    if (parser.getPort() != -1)
        port = Stringutil::number2string(parser.getPort());

    cout << "Protocol:   " << parser.getProtocolAsString() << endl;
    cout << "URL:        " << parser.getURL() << endl;
    cout << "Username:   " << parser.getUsername() << endl;
    cout << "Password:   " << parser.getPassword() << endl;
    cout << "Host:       " << parser.getHostname() << endl;
    cout << "Port:       " << port << endl;
    cout << "Path:       " << parser.getPath() << endl;
    cout << "Realpath:   " << parser.getRealPath() << endl;
}

//}}}

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
