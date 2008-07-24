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
#ifndef PRINT_TARGET_H
#define PRINT_TARGET_H

#include "subcommand.h"

//{{{ ReadVmcoreinfo -----------------------------------------------------------

/**
 * Subcommand to print the target URL (parsed).
 */
class PrintTarget : public Subcommand {

    public:
        /**
         * Creates a new PrintTarget object.
         */
        PrintTarget()
        throw ();

    public:
        /**
         * Returns the name of the subcommand (print_target).
         */
        const char *getName() const
        throw ();

        /**
         * Returns the list of options supported by this subcommand.
         */
        OptionList getOptions() const
        throw ();

        /**
         * Parses the command line options.
         */
        void parseCommandline(OptionParser *optionparser)
        throw (KError);

        /**
         * Executes the function.
         *
         * @throw KError on any error. No exception indicates success.
         */
        void execute()
        throw (KError);

    private:
        std::string m_rootdir;
};

//}}}

#endif /* PRINT_TARGET_H */

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
