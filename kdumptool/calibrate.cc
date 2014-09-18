/*
 * (c) 2014, Petr Tesarik <ptesarik@suse.de>, SUSE LINUX Products GmbH
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
#include <fstream>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>

#include "subcommand.h"
#include "debug.h"
#include "calibrate.h"
#include "configuration.h"
#include "util.h"
#include "fileutil.h"

// All calculations are in KiB

// This macro converts MiB to KiB
#define MB(x)	((x)*1024)

// Shift right by an amount, rounding the result up
#define shr_round_up(n,amt)	(((n) + (1UL << (amt)) - 1) >> (amt))

// Default reservation size depends on architecture
//
// The following macros are defined:
//
//    DEF_RESERVE_KB	default reservation size
//    KERNEL_KB		kernel image text + data + bss
//    INIT_KB		basic initramfs size (unpacked)
//    INIT_NET_KB	initramfs size increment when network is included
//    SIZE_STRUCT_PAGE	sizeof(struct page)
//    KDUMP_PHYS_LOAD   assumed physical load address of the kdump kernel;
//                      if pages between 0 and the load address are not
//                      counted into total memory, set this to ZERO
//    PERCPU_KB         additional kernel memory for each online CPU
//    CAN_REDUCE_CPUS   non-zero if the architecture can reduce kernel
//                      memory requirements with nr_cpus=
//

#if defined(__x86_64__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(16)
# define INIT_KB		MB(34)
# define INIT_NET_KB		MB(3)
# define SIZE_STRUCT_PAGE	56
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		108

#elif defined(__i386__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(14)
# define INIT_KB		MB(29)
# define INIT_NET_KB		MB(2)
# define SIZE_STRUCT_PAGE	32
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		56

#elif defined(__powerpc64__)
# define DEF_RESERVE_KB		MB(256)
# define KERNEL_KB		MB(16)
# define INIT_KB		MB(58)
# define INIT_NET_KB		MB(4)
# define SIZE_STRUCT_PAGE	64
# define KDUMP_PHYS_LOAD	MB(128)
# define CAN_REDUCE_CPUS	0
# define PERCPU_KB		172	// FIXME: is it non-linear?

#elif defined(__powerpc__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(12)
# define INIT_KB		MB(34)
# define INIT_NET_KB		MB(2)
# define SIZE_STRUCT_PAGE	32
# define KDUMP_PHYS_LOAD	MB(128)
# define CAN_REDUCE_CPUS	0
# define PERCPU_KB		0	// TODO !!!

#elif defined(__s390x__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(13)
# define INIT_KB		MB(34)
# define INIT_NET_KB		MB(2)
# define SIZE_STRUCT_PAGE	56
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		48

# define align_memmap		s390x_align_memmap

#elif defined(__s390__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(12)
# define INIT_KB		MB(29)
# define INIT_NET_KB		MB(2)
# define SIZE_STRUCT_PAGE	32
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		0	// TODO !!!

# define align_memmap		s390_align_memmap

#elif defined(__ia64__)
# define DEF_RESERVE_KB		MB(512)
# define KERNEL_KB		MB(32)
# define INIT_KB		MB(44)
# define INIT_NET_KB		MB(4)
# define SIZE_STRUCT_PAGE	56
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		0	// TODO !!!

#elif defined(__aarch64__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(10)
# define INIT_KB		MB(29)
# define INIT_NET_KB		MB(2)
# define SIZE_STRUCT_PAGE	56
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		0	// TODO !!!

#elif defined(__arm__)
# define DEF_RESERVE_KB		MB(128)
# define KERNEL_KB		MB(12)
# define INIT_KB		MB(29)
# define INIT_NET_KB		MB(2)
# define SIZE_STRUCT_PAGE	32
# define KDUMP_PHYS_LOAD	0
# define CAN_REDUCE_CPUS	1
# define PERCPU_KB		0	// TODO !!!

#else
# error "No default crashkernel reservation for your architecture!"
#endif

#ifndef align_memmap
# define align_memmap(maxpfn)	((unsigned long) (maxpfn))
#endif

static inline unsigned long s390_align_memmap(unsigned long maxpfn)
{
    // SECTION_SIZE_BITS: 25, PAGE_SHIFT: 12, KiB: 10
    return ((maxpfn - 1) | ((1UL << (25 - 12 - 10)) - 1)) + 1;
}

static inline unsigned long s390x_align_memmap(unsigned long maxpfn)
{
    // SECTION_SIZE_BITS: 28, PAGE_SHIFT: 12, KiB: 10
    return ((maxpfn - 1) | ((1UL << (28 - 12 - 10)) - 1)) + 1;
}

// (Pessimistic) estimate of the initrd compression ratio (percents)
#define INITRD_COMPRESS	50

// Estimated non-changing dynamic data (sysfs, procfs, etc.)
#define KERNEL_DYNAMIC_KB	MB(4)

// Default framebuffer size: 1024x768 @ 32bpp, except on mainframe
#if defined(__s390__) || defined(__s390x__)
# define DEF_FRAMEBUFFER_KB	0
#else
# define DEF_FRAMEBUFFER_KB	(768UL*4)
#endif

// large hashes, default settings:			     -> per MiB
//   PID: sizeof(void*) for each 256 KiB			  4
//   Dentry cache: sizeof(void*) for each  8 KiB		128
//   Inode cache:  sizeof(void*) for each 16 KiB		 64
//   TCP established: 2*sizeof(void*) for each 128 KiB		 16
//   TCP bind: 2*sizeof(void*) for each 128 KiB			 16
//   UDP: 2*sizeof(void*) + 2*sizeof(long) for each 2 MiB	  1 + 1
//   UDP-Lite: 2*sizeof(void*) + 2*sizeof(long) for each 2 MiB	  1 + 1
//								-------
//								230 + 2
// Assuming that sizeof(void*) == sizeof(long):
#define KERNEL_HASH_PER_MB	(232*sizeof(long))

// Estimated buffer metadata and filesystem in KiB per dirty MiB
#define BUF_PER_DIRTY_MB	64

// Default vm dirty ratio is 20%
#define DIRTY_RATIO		20

// Userspace base requirements:
//   systemd (PID 1)	 3 M
//   journald		 2 M
//   the journal itself	 4 M
//   10 * udevd		12 M
//   kdumptool		 4 M
//   makedumpfile	 1 M
// -------------------------
// TOTAL:		26 M
#define USER_BASE_KB	MB(26)

// Additional requirements when network is configured
//   dhclient		 7 M
#define USER_NET_KB	MB(7)

// Maximum size of the page bitmap
// 32 MiB is 32*1024*1024*8 = 268435456 bits
// makedumpfile uses two bitmaps, so each has 134217728 bits
// with 4-KiB pages this covers 0.5 TiB of RAM in one cycle
#define MAX_BITMAP_KB	MB(32)

using std::cout;
using std::endl;
using std::ifstream;

//{{{ SystemCPU ----------------------------------------------------------------

class SystemCPU {

    public:
        /**
	 * Initialize a new SystemCPU object.
	 *
	 * @param[in] sysdir Mount point for sysfs
	 */
	SystemCPU(const char *sysdir = "/sys")
	throw ()
	: m_cpudir(FilePath(sysdir).appendPath("devices/system/cpu"))
	{}

    protected:
        /**
	 * Path to the cpu system devices base directory
	 */
	const FilePath m_cpudir;

        /**
	 * Count the number of CPUs in a cpuset
	 *
	 * @param[in] name Name of the cpuset ("possible", "present", "online")
	 *
	 * @exception KError if the file cannot be opened or parsed
	 */
	unsigned long count(const char *name);

    public:
        /**
	 * Count the number of online CPUs
	 *
	 * @exception KError see @c count()
	 */
	unsigned long numOnline(void)
	{ return count("online"); }

        /**
	 * Count the number of offline CPUs
	 *
	 * @exception KError see @c count()
	 */
	unsigned long numOffline(void)
	{ return count("offline"); }

};

// -----------------------------------------------------------------------------
unsigned long SystemCPU::count(const char *name)
{
    FilePath path(m_cpudir);
    path.appendPath(name);
    unsigned long ret = 0UL;

    ifstream fin(path.c_str());
    if (!fin)
	throw KSystemError("Cannot open " + path, errno);

    try {
	unsigned long n1, n2;
	char delim;

	fin.exceptions(ifstream::badbit);
	while (fin >> n1) {
	    fin >> delim;
	    if (fin && delim == '-') {
		if (! (fin >> n2) )
		    throw KError(path + ": wrong number format");
		ret += n2 - n1;
		fin >> delim;
	    }
	    if (!fin.eof() && delim != ',')
		throw KError(path + ": wrong delimiter: " + delim);
	    ++ret;
	}
	if (!fin.eof())
	    throw KError(path + ": wrong number format");
	fin.close();
    } catch (ifstream::failure e) {
	throw KSystemError("Cannot read " + path, errno);
    }

    return ret;
}

//}}}
//{{{ Framebuffer --------------------------------------------------------------

class Framebuffer {

    public:
        /**
	 * Initialize a new Framebuffer object.
	 *
	 * @param[in] fbpath Framebuffer sysfs directory path
	 */
	Framebuffer(const char *fbpath)
	throw ()
	: m_dir(fbpath)
	{}

    protected:
        /**
	 * Path to the framebuffer device base directory
	 */
	const FilePath m_dir;

    public:
	/**
	 * Get length of the framebuffer [in bytes].
	 */
	unsigned long size(void) const;
};

// -----------------------------------------------------------------------------
unsigned long Framebuffer::size(void) const
{
    FilePath fp;
    std::ifstream f;
    unsigned long width, height, stride;
    char sep;

    fp.assign(m_dir);
    fp.appendPath("virtual_size");
    f.open(fp.c_str());
    if (!f)
	throw KError(fp + ": Open failed");
    f >> width >> sep >> height;
    f.close();
    if (f.bad())
	throw KError(fp + ": Read failed");
    else if (!f || sep != ',')
	throw KError(fp + ": Invalid content!");
    Debug::debug()->dbg("Framebuffer virtual size: %lux%lu", width, height);

    fp.assign(m_dir);
    fp.appendPath("stride");
    f.open(fp.c_str());
    if (!f)
	throw KError(fp + ": Open failed");
    f >> stride;
    f.close();
    if (f.bad())
	throw KError(fp + ": Read failed");
    else if (!f || sep != ',')
	throw KError(fp + ": Invalid content!");
    Debug::debug()->dbg("Framebuffer stride: %lu bytes", stride);

    return stride * height;
}

//}}}
//{{{ Framebuffers -------------------------------------------------------------

class Framebuffers {

    public:
        /**
	 * Initialize a new Framebuffer object.
	 *
	 * @param[in] sysdir Mount point for sysfs
	 */
	Framebuffers(const char *sysdir = "/sys")
	throw ()
	: m_fbdir(FilePath(sysdir).appendPath("class/graphics"))
	{}

    protected:
        /**
	 * Path to the base directory with links to framebuffer devices
	 */
	const FilePath m_fbdir;

        /**
	 * Filter only valid framebuffer device name
	 */
	class DirFilter : public ListDirFilter {

	    public:
		virtual ~DirFilter()
		{}

		bool test(const struct dirent *d) const
		{
		    char *end;
		    if (strncmp(d->d_name, "fb", 2))
			return false;
		    strtoul(d->d_name + 2, &end, 10);
		    return (*end == '\0' && end != d->d_name + 2);
		}
	};

    public:
	/**
	 * Get size of all framebuffers [in bytes].
	 */
	unsigned long size(void) const;
};

// -----------------------------------------------------------------------------
unsigned long Framebuffers::size(void) const
{
    Debug::debug()->trace("Framebuffers::size()");

    unsigned long ret = 0UL;

    StringVector v = m_fbdir.listDir(DirFilter());
    for (StringVector::const_iterator it = v.begin(); it != v.end(); ++it) {
        Debug::debug()->dbg("Found framebuffer: %s", it->c_str());

	FilePath fp(m_fbdir);
	fp.appendPath(*it);
	Framebuffer fb(fp.c_str());
	ret += fb.size();
    }

    Debug::debug()->dbg("Total size of all framebuffers: %lu bytes", ret);
    return ret;
}

//}}}
//{{{ SlabInfo -----------------------------------------------------------------

class SlabInfo {

    public:
        /**
	 * Initialize a new SlabInfo object.
	 *
	 * @param[in] line Line from /proc/slabinfo
	 */
	SlabInfo(const KString &line);

    protected:
	bool m_comment;
	KString m_name;
	unsigned long m_active_objs;
	unsigned long m_num_objs;
	unsigned long m_obj_size;
	unsigned long m_obj_per_slab;
	unsigned long m_pages_per_slab;
	unsigned long m_active_slabs;
	unsigned long m_num_slabs;

    public:
	bool isComment(void) const
	throw ()
	{ return m_comment; }

	const KString &name(void) const
	throw ()
	{ return m_name; }

	unsigned long activeObjs(void) const
	throw ()
	{ return m_active_objs; }

	unsigned long numObjs(void) const
	throw ()
	{ return m_num_objs; }

	unsigned long objSize(void) const
	throw ()
	{ return m_obj_size; }

	unsigned long objPerSlab(void) const
	throw ()
	{ return m_obj_per_slab; }

	unsigned long pagesPerSlab(void) const
	throw ()
	{ return m_pages_per_slab; }

	unsigned long activeSlabs(void) const
	throw ()
	{ return m_active_slabs; }

	unsigned long numSlabs(void) const
	throw ()
	{ return m_num_slabs; }
};

// -----------------------------------------------------------------------------
SlabInfo::SlabInfo(const KString &line)
{
    static const char slabdata_mark[] = " : slabdata ";

    std::istringstream ss(line);
    ss >> m_name;
    if (!ss)
	throw KError("Invalid slabinfo line: " + line);

    if (m_name[0] == '#') {
	m_comment = true;
	return;
    }
    m_comment = false;

    ss >> m_active_objs >> m_num_objs >> m_obj_size
       >> m_obj_per_slab >> m_pages_per_slab;
    if (!ss)
	throw KError("Invalid slabinfo line: " + line);

    size_t sdpos = line.find(slabdata_mark, ss.tellg());
    if (sdpos == KString::npos)
	throw KError("Invalid slabinfo line: " + line);

    ss.seekg(sdpos + sizeof(slabdata_mark) - 1, ss.beg);
    ss >> m_active_slabs >> m_num_slabs;
    if (!ss)
	throw KError("Invalid slabinfo line: " + line);
}

//}}}
//{{{ SlabInfos ----------------------------------------------------------------

// Taken from procps:
#define SLABINFO_LINE_LEN	2048
#define SLABINFO_VER_LEN	100

class SlabInfos {

    public:
	typedef std::map<KString, const SlabInfo*> Map;

        /**
	 * Initialize a new SlabInfos object.
	 *
	 * @param[in] procdir Mount point for procfs
	 */
	SlabInfos(const char *procdir = "/proc")
	throw ()
	: m_path(FilePath(procdir).appendPath("slabinfo"))
	{}

	~SlabInfos()
	{ destroyInfo(); }

    protected:
        /**
	 * Path to the slabinfo file
	 */
	const FilePath m_path;

        /**
	 * SlabInfo for each slab
	 */
	Map m_info;

    private:
	/**
	 * Destroy SlabInfo objects in m_info.
	 */
	void destroyInfo(void)
	throw();

    public:
        /**
	 * Read the information about each slab.
	 */
	const Map& getInfo(void);
};

// -----------------------------------------------------------------------------
void SlabInfos::destroyInfo(void)
    throw()
{
    Map::iterator it;
    for (it = m_info.begin(); it != m_info.end(); ++it)
	delete it->second;
    m_info.clear();
}

// -----------------------------------------------------------------------------
const SlabInfos::Map& SlabInfos::getInfo(void)
{
    static const char verhdr[] = "slabinfo - version: ";
    char buf[SLABINFO_VER_LEN], *p, *end;
    unsigned long major, minor;

    std::ifstream f(m_path.c_str());
    if (!f)
	throw KError(m_path + ": Open failed");
    f.getline(buf, sizeof buf);
    if (f.bad())
	throw KError(m_path + ": Read failed");
    else if (!f || strncmp(buf, verhdr, sizeof(verhdr)-1))
	throw KError(m_path + ": Invalid version");
    p = buf + sizeof(verhdr) - 1;

    major = strtoul(p, &end, 10);
    if (end == p || *end != '.')
	throw KError(m_path + ": Invalid version");
    p = end + 1;
    minor = strtoul(p, &end, 10);
    if (end == p || *end != '\0')
	throw KError(m_path + ": Invalid version");
    Debug::debug()->dbg("Found slabinfo version %lu.%lu", major, minor);

    if (major != 2)
	throw KError(m_path + ": Unsupported slabinfo version");

    char line[SLABINFO_LINE_LEN];
    while(f.getline(line, SLABINFO_LINE_LEN)) {
	SlabInfo *si = new SlabInfo(line);
	if (si->isComment()) {
	    delete si;
	    continue;
	}
	m_info[si->name()] = si;
    }

    return m_info;
}

//}}}
//{{{ Calibrate ----------------------------------------------------------------

// -----------------------------------------------------------------------------
Calibrate::Calibrate()
    throw ()
{}

// -----------------------------------------------------------------------------
const char *Calibrate::getName() const
    throw ()
{
    return "calibrate";
}

// -----------------------------------------------------------------------------
void Calibrate::execute()
    throw (KError)
{
    Debug::debug()->trace("Calibrate::execute()");

    unsigned long required, prev;
    unsigned long pagesize = sysconf(_SC_PAGESIZE);

    try {
	Configuration *config = Configuration::config();
	bool needsnet = config->needsNetwork();

	// Get total RAM size
	unsigned long memtotal = Util::getMemorySize();
        Debug::debug()->dbg("Expected total RAM: %lu KiB", memtotal);

	// Calculate boot requirements
	unsigned long ramfs = INIT_KB;
	if (needsnet)
	    ramfs += INIT_NET_KB;
	unsigned long initrd = (ramfs * INITRD_COMPRESS) / 100;
	unsigned long bootsize = KERNEL_KB + initrd + ramfs;
        Debug::debug()->dbg("Memory needed at boot: %lu KiB", bootsize);

	// Run-time kernel requirements
	required = KERNEL_KB + ramfs + KERNEL_DYNAMIC_KB;

	try {
	    Framebuffers fb;
	    required += fb.size() / 1024UL;
	} catch(KError e) {
	    Debug::debug()->dbg("Cannot get framebuffer size: %s", e.what());
	    required += DEF_FRAMEBUFFER_KB;
	}

	// Add space for constant slabs
	try {
	    SlabInfos slab;
	    SlabInfos::Map info = slab.getInfo();
	    SlabInfos::Map::iterator it;
	    for (it = info.begin(); it != info.end(); ++it) {
		if (it->first.startsWith("Acpi-") ||
		    it->first.startsWith("ftrace_") ) {
		    unsigned long slabsize = it->second->numSlabs() *
			it->second->pagesPerSlab() * pagesize / 1024;
		    required += slabsize;

		    Debug::debug()->dbg("Adding %ld KiB for %s slab cache",
					slabsize, it->second->name().c_str());
		}
	    }
	} catch (KError e) {
	    Debug::debug()->dbg("Cannot get slab sizes: %s", e.what());
	}

	// Add memory based on CPU count
	unsigned long cpus;
	if (CAN_REDUCE_CPUS) {
	    cpus = config->KDUMP_CPUS.value();
	} else {
	    SystemCPU syscpu;
	    unsigned long online = syscpu.numOnline();
	    unsigned long offline = syscpu.numOffline();
	    Debug::debug()->dbg("CPUs online: %lu, offline: %lu",
				online, offline);
	    cpus = online + offline;
	}
	Debug::debug()->dbg("Total assumed CPUs: %lu", cpus);

	// User-space requirements
	unsigned long user = USER_BASE_KB;
	if (needsnet)
	    user += USER_NET_KB;

	if (config->needsMakedumpfile()) {
	    // Estimate bitmap size (1 bit for every RAM page)
	    unsigned long bitmapsz = shr_round_up(memtotal / pagesize, 2);
	    if (bitmapsz > MAX_BITMAP_KB)
		bitmapsz = MAX_BITMAP_KB;
	    Debug::debug()->dbg("Estimated bitmap size: %lu KiB", bitmapsz);
	    user += bitmapsz;

	    // Makedumpfile needs additional 96 B for every 128 MiB of RAM
	    user += 96 * shr_round_up(memtotal, 20 + 7);
	}
        Debug::debug()->dbg("Total userspace: %lu KiB", user);
	required += user;

	// Make room for dirty pages and in-flight I/O:
	//
	//   required = prev + dirty + io
	//      dirty = total * (DIRTY_RATIO / 100)
	//	   io = dirty * (BUF_PER_DIRTY_MB / 1024)
	//
	// solve the above using integer math:
	unsigned long dirty;
	prev = required;
	required = required * MB(100) /
	    (MB(100) - MB(DIRTY_RATIO) - DIRTY_RATIO * BUF_PER_DIRTY_MB);
	dirty = (required - prev) * MB(1) / (MB(1) + BUF_PER_DIRTY_MB);
        Debug::debug()->dbg("Dirty pagecache: %lu KiB", dirty);
        Debug::debug()->dbg("In-flight I/O: %lu KiB", required - prev - dirty);

	// Account for memory between 0 and KDUMP_PHYS_LOAD
	required += KDUMP_PHYS_LOAD;
	Debug::debug()->dbg("Assumed load offset: %lu KiB", KDUMP_PHYS_LOAD);

	// Account for "large hashes"
	prev = required;
	required = required * MB(1024) / (MB(1024) - KERNEL_HASH_PER_MB);
        Debug::debug()->dbg("Large kernel hashes: %lu KiB", required - prev);

	// Add space for memmap
	prev = required;
	required = required * pagesize / (pagesize - SIZE_STRUCT_PAGE);
	unsigned long maxpfn = (required - prev) / SIZE_STRUCT_PAGE;
	required = prev + align_memmap(maxpfn) * SIZE_STRUCT_PAGE;
        Debug::debug()->dbg("Maximum memmap size: %lu KiB", required - prev);

	// Make sure there is enough space at boot
	Debug::debug()->dbg("Total run-time size: %lu KiB", required);
	if (required < bootsize)
	    required = bootsize;

    } catch(KError e) {
	Debug::debug()->info(e.what());
	required = DEF_RESERVE_KB;
    }

    cout << shr_round_up(required, 10) << endl;
}

//}}}

// vim: set sw=4 ts=4 fdm=marker et: :collapseFolds=1:
