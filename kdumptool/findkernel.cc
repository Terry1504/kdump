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
#include <zlib.h>
#include <libelf.h>
#include <gelf.h>
#include <cerrno>
#include <fcntl.h>

#include "subcommand.h"
#include "debug.h"
#include "findkernel.h"
#include "util.h"
#include "kerneltool.h"
#include "configuration.h"
#include "fileutil.h"
#include "stringutil.h"
#include "kconfig.h"

using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::list;

#define BOOTDIR "/boot"

//{{{ FindKernel ---------------------------------------------------------------

// -----------------------------------------------------------------------------
FindKernel::FindKernel()
    throw ()
{}

// -----------------------------------------------------------------------------
const char *FindKernel::getName() const
    throw ()
{
    return "find_kernel";
}

// -----------------------------------------------------------------------------
void FindKernel::execute()
    throw (KError)
{
    Debug::debug()->trace("FindKernel::execute()");

    string kernelver = Configuration::config()->getKernelVersion();

    // user has specified a specific kernel, check that first
    string kernelimage;
    if (kernelver.size() > 0) {
        kernelimage = findForVersion(kernelver);
        if (kernelimage.size() == 0) {
            cerr << "KDUMP_KERNELVER set to '" << kernelver << "' "
                 << "but no such kernel exists." << endl;
            setErrorCode(-1);
            return;
        }
        
        // suitable?
        if (!suitableForKdump(kernelimage, false)) {
            cerr << "Kernel '" << kernelimage << "' is not suitable for kdump."
                 << endl;
            cerr << "Please change KDUMP_KERNELVER in /etc/sysconfig/kdump."
                 << endl;
            setErrorCode(-1);
            return;
        }
    } else {
        kernelimage = findKernelAuto();
        if (kernelimage.size() == 0) {
            cerr << "No suitable kdump kernel found." << endl;
            setErrorCode(-1);
            return;
        }
    }

    // found!
    string initrd = findInitrd(kernelimage);

    cout << "Kernel:\t" << kernelimage << endl;
    cout << "Initrd:\t" << initrd << endl;
}

// -----------------------------------------------------------------------------
bool FindKernel::suitableForKdump(const string &kernelImage, bool strict)
    throw (KError)
{
    KernelTool kt(kernelImage);

    // if that's not a special kdump kernel, it must be relocatable
    // TODO: check about start address, don't trust the naming
    if (isKdumpKernel(kernelImage)) {
        Debug::debug()->dbg("%s is kdump kernel, no need for relocatable check",
            kernelImage.c_str());
    } else {
        bool relocatable = kt.isRelocatable();
        Debug::debug()->dbg("%s is %s", kernelImage.c_str(),
            relocatable ? "relocatable" : "not relocatable");
        if (!relocatable) {
            return false;
        }
    }

    if (strict) {
        string arch = Util::getArch();
        Kconfig *kconfig = kt.retrieveKernelConfig();

        // avoid large number of CPUs on x86 since that increases
        // memory size constraints of the capture kernel
        if (arch == "i386" || arch == "x86_64") {
            KconfigValue kv = kconfig->get("CONFIG_NR_CPUS");
            if (kv.getType() == KconfigValue::T_STRING) {
                int nr_cpus = kv.getIntValue();
                if (nr_cpus >= 1024) {
                    Debug::debug()->dbg("NR_CPUS of %s is %d >= 1024. Avoid.",
                        kernelImage.c_str(), nr_cpus);
                    delete kconfig;
                    return false;
                }
            }
        }

        // avoid realtime kernels
        KconfigValue kv = kconfig->get("CONFIG_PREEMPT_RT");
        if (kv.getType() != KconfigValue::T_INVALID) {
            Debug::debug()->dbg("%s is realtime kernel. Avoid.",
                kernelImage.c_str());
            delete kconfig;
            return false;
        }

        delete kconfig;
    }

    return true;
}

// -----------------------------------------------------------------------------
string FindKernel::findForVersion(const string &kernelver)
    throw (KError)
{
    Debug::debug()->trace("FindKernel::findForVersion(%s)", kernelver.c_str());

    list<string> imageNames = KernelTool::imageNames(Util::getArch());

    for (list<string>::const_iterator it = imageNames.begin();
            it != imageNames.end(); ++it) {
        string imagename = *it;
        string kernel;
        if (kernelver.size() == 0) {
            kernel = FileUtil::pathconcat(BOOTDIR, imagename);
        } else {
            kernel = FileUtil::pathconcat(BOOTDIR, imagename+"-"+kernelver);
        }

        Debug::debug()->dbg("Trying %s", kernel.c_str());
        if (FileUtil::exists(kernel)) {
            Debug::debug()->dbg("%s exists", kernel.c_str());
            return kernel;
        }
    }

    return "";
}

// -----------------------------------------------------------------------------
string FindKernel::findKernelAuto()
    throw (KError)
{
    Debug::debug()->trace("FindKernel::findKernelAuto()");

    string runningkernel = Util::getKernelRelease();
    Debug::debug()->trace("Running kernel: %s", runningkernel.c_str());

    // 1. Use $(uname -r)-kdump
    StringVector elements = Stringutil::split(runningkernel, '-');
    elements[elements.size()-1] = "kdump";
    string testkernel = Stringutil::join(elements, '-');
    Debug::debug()->dbg("Trying %s", testkernel.c_str());
    string testkernelimage = findForVersion(testkernel);
    if (testkernelimage.size() > 0 && suitableForKdump(testkernelimage, true)) {
        return testkernelimage;
    }

    // 2. Use kdump
    testkernel = "kdump";
    Debug::debug()->dbg("Trying %s", testkernel.c_str());
    testkernelimage = findForVersion(testkernel);
    if (testkernelimage.size() > 0 && suitableForKdump(testkernelimage, true)) {
        return testkernelimage;
    }

    // 3. Use $(uname -r)
    testkernel = runningkernel;
    Debug::debug()->dbg("Trying %s", testkernel.c_str());
    testkernelimage = findForVersion(testkernel);
    if (testkernelimage.size() > 0 && suitableForKdump(testkernelimage, true)) {
        return testkernelimage;
    }

    // 4. Use ""
    testkernel = "";
    Debug::debug()->dbg("Trying %s", testkernel.c_str());
    testkernelimage = findForVersion(testkernel);
    if (testkernelimage.size() > 0 && suitableForKdump(testkernelimage, true)) {
        return testkernelimage;
    }

    // 5. Use $(uname -r) unstrict
    testkernel = runningkernel;
    Debug::debug()->dbg("Trying %s", testkernel.c_str());
    testkernelimage = findForVersion(testkernel);
    if (testkernelimage.size() > 0 && suitableForKdump(testkernelimage, false)) {
        return testkernelimage;
    }

    // 6. Use "" unstrict
    testkernel = "";
    Debug::debug()->dbg("Trying %s", testkernel.c_str());
    testkernelimage = findForVersion(testkernel);
    if (testkernelimage.size() > 0 && suitableForKdump(testkernelimage, false)) {
        return testkernelimage;
    }

    return "";
}

// -----------------------------------------------------------------------------
bool FindKernel::isKdumpKernel(const string &kernelimage)
    throw (KError)
{
    Debug::debug()->trace("FindKernel::isKdumpKernel(%s)", kernelimage.c_str());
    return Stringutil::endsWith(kernelimage, "-kdump");
}

// -----------------------------------------------------------------------------
string FindKernel::findInitrd(const string &kernelPath)
    throw ()
{
    Debug::debug()->trace("FindKernel::findInitrd(%s)", kernelPath.c_str());

    string dir, stripped;
    KernelTool::stripImageName(kernelPath, dir, stripped);
    
    if (isKdumpKernel(stripped)) {
        return FileUtil::pathconcat(dir, "initrd-" + stripped);
    } else {
        return FileUtil::pathconcat(dir, "initrd-" + stripped + "-kdump");
    }
}


//}}}

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
