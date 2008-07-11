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
#ifndef PROGRESS_H
#define PROGRESS_H

#include "global.h"

//{{{ Terminal -----------------------------------------------------------------

/**
 * Retrieve some information about the terminal.
 */
class Terminal {

    public:
        /**
         * Represents a terminal size.
         */
        struct Size {
            /** The width */
            int width;
            /** The height */
            int height;
        };

    public:
        /**
         * Deletes a Terminal object.
         */
        virtual ~Terminal()
        throw () {}

    public:

        /**
         * Retriesves the terminal size.
         *
         * @return a size object that represents the terminal size,
         *         and (0, 0) if the size cannot be retrieved (mostly because
         *         the standard output is no terminal but redirected to a file)
         */
        Size getSize() const
        throw ();
};

//}}}
//{{{ Progress -----------------------------------------------------------------


/**
 * Interface for a progress meter.
 */
class Progress {

    public:
        /**
         * Destroys a Progress object.
         */
        virtual ~Progress() {}

    public:
        /**
         * This method has to be called when the operation starts.
         */
        virtual void start()
        throw () = 0;

        /**
         * This operation has to be called repeadedly.
         *
         * @param[in] current the current progress value
         * @param[in] max the maximum progress value
         */
        virtual void progressed(int current, int max)
        throw () = 0;

        /**
         * This method has to be called when the operation is finished.
         * It displays the final 100 %.
         */
        virtual void stop()
        throw () = 0;
};

//}}}
//{{{ TerminalProgress ---------------------------------------------------------

/**
 * Retrieve some information about the terminal.
 */
class TerminalProgress : public Progress {

    public:
        /**
         * Creates a new object of type TerminalProgress.
         *
         * @param[in] name the name of the action that is taking place
         *            here
         */
        TerminalProgress(const std::string &name)
        throw ();

        /**
         * This method has to be called when the operation starts.
         */
        void start()
        throw ();

        /**
         * This operation has to be called repeadedly.
         *
         * @param[in] current the current progress value
         * @param[in] max the maximum progress value
         */
        void progressed(int current, int max)
        throw ();

        /**
         * This method has to be called when the operation is finished.
         * It displays the final 100 %.
         */
        void stop()
        throw ();

    private:
        std::string m_name;
        int m_linelen;
        int m_progresslen;
};

//}}}

#endif /* PROGRESS_H */

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
