#include "os.hpp"

#define FDP_MODULE "os_linux"
#include "core.hpp"
#include "log.hpp"
#include "reader.hpp"
#include "utils/fnview.hpp"
#include "utils/utils.hpp"

#include "map.hpp"
#include "mbedtls/sha1.h"
#include "sym.hpp"

#include <array>
#include <regex>
#include <sstream>
#include <unordered_set>

#ifdef _MSC_VER
#    define search                  std::search
#    define boyer_moore_searcher    std::boyer_moore_searcher
#else
#    define search                  std::experimental::search
#    define boyer_moore_searcher    std::experimental::make_boyer_moore_searcher
#endif

namespace
{
    class version
    {
        std::vector<int> nums;

      public:
        version(const std::string&);
        std::string get();

        bool operator==(const version&);
        bool operator<(const version&);

        bool operator<=(const version&);
        bool operator>(const version&);
        bool operator>=(const version&);
        bool operator!=(const version&);
    };

    version::version(const std::string& vers)
    {
        nums.clear();
        size_t start = 0, end;
        while((end = vers.find('.', start)) != std::string::npos)
        {
            nums.push_back(std::stoi(vers.substr(start, end - start)));
            start = end + 1;
        }
        nums.push_back(std::stoi(vers.substr(start)));

        if(nums.empty())
            throw std::invalid_argument("empty version string");
    }

    std::string version::get()
    {
        std::string str;
        for(const int& num : nums)
            str.append(std::to_string(num)).push_back('.');

        str.pop_back();
        return str;
    }

    bool version::operator==(const version& other)
    {
        return nums == other.nums;
    }

    bool version::operator<(const version& other)
    {
        if(*this == other)
            return false;

        for(int i = 0; i < std::min(nums.size(), other.nums.size()); i++)
        {
            if(nums[i] == other.nums[i])
                continue;
            return nums[i] < other.nums[i];
        }
        return nums.size() < other.nums.size();
    }

    bool version::operator>(const version& other)
    {
        return !(*this < other);
    }

    bool version::operator>=(const version& other)
    {
        return (*this > other) | (*this == other);
    }

    bool version::operator<=(const version& other)
    {
        return (*this < other) | (*this == other);
    }

    bool version::operator!=(const version& other)
    {
        return !(*this == other);
    }
}

namespace
{
    enum class cat_e
    {
        REQUIRED,
        OPTIONAL,
    };

    enum offset_e
    {
        TASKSTRUCT_THREADINFO,
        TASKSTRUCT_COMM,
        TASKSTRUCT_PID,
        TASKSTRUCT_REALPARENT,
        TASKSTRUCT_GROUPLEADER,
        TASKSTRUCT_THREADGROUP,
        TASKSTRUCT_TASKS,
        TASKSTRUCT_MM,
        TASKSTRUCT_ACTIVEMM,
        MMSTRUCT_PGD,
        TASKSTRUCT_STACK,
        PTREGS_IP,
        OFFSET_COUNT,
    };

    struct LinuxOffset
    {
        cat_e      e_cat;
        offset_e   e_id;
        const char module[16];
        const char struc[32];
        const char member[32];
    };
    // clang-format off
    const LinuxOffset g_offsets[] =
    {
			{cat_e::REQUIRED,	TASKSTRUCT_THREADINFO,		"kernel_struct",	"task_struct",		"thread_info"	},
			{cat_e::REQUIRED,	TASKSTRUCT_COMM,			"kernel_struct",	"task_struct",		"comm"			},
			{cat_e::REQUIRED,	TASKSTRUCT_PID,				"kernel_struct",	"task_struct",		"pid"			},
			{cat_e::REQUIRED,	TASKSTRUCT_REALPARENT,		"kernel_struct",	"task_struct",		"real_parent"	},
            {cat_e::REQUIRED,	TASKSTRUCT_GROUPLEADER,		"kernel_struct",	"task_struct",		"group_leader"	},
			{cat_e::REQUIRED,	TASKSTRUCT_THREADGROUP,		"kernel_struct",	"task_struct",		"thread_group"	},
            {cat_e::REQUIRED,	TASKSTRUCT_TASKS,			"kernel_struct",	"task_struct",		"tasks"			},
            {cat_e::REQUIRED,	TASKSTRUCT_MM,				"kernel_struct",	"task_struct",		"mm"			},
            {cat_e::REQUIRED,	TASKSTRUCT_ACTIVEMM,		"kernel_struct",	"task_struct",		"active_mm"		},
            {cat_e::REQUIRED,	MMSTRUCT_PGD,				"kernel_struct",	"mm_struct",		"pgd"			},
			{cat_e::REQUIRED,	TASKSTRUCT_STACK,			"kernel_struct",	"task_struct",		"stack"			},
			{cat_e::REQUIRED,	PTREGS_IP,					"kernel_struct",	"pt_regs",			"ip"			},
    };
    // clang-format on
    static_assert(COUNT_OF(g_offsets) == OFFSET_COUNT, "invalid offsets");

    enum symbol_e
    {
        PER_CPU_START,
        CURRENT_TASK,
        KASAN_INIT,
        SYMBOL_COUNT,
    };

    struct LinuxSymbol
    {
        cat_e      e_cat;
        symbol_e   e_id;
        const char module[16];
        const char name[32];
    };
    // clang-format off
    const LinuxSymbol g_symbols[] =
    {
            {cat_e::REQUIRED,	PER_CPU_START,				"kernel_sym",	"__per_cpu_start"	},
			{cat_e::REQUIRED,	CURRENT_TASK,				"kernel_sym",	"current_task"		},
			{cat_e::OPTIONAL,	KASAN_INIT,					"kernel_sym",	"kasan_init"		},
    };
    // clang-format on
    static_assert(COUNT_OF(g_symbols) == SYMBOL_COUNT, "invalid symbols");

    using LinuxOffsets = std::array<uint64_t, OFFSET_COUNT>;
    using LinuxSymbols = std::array<uint64_t, SYMBOL_COUNT>;

    struct OsLinux
        : public os::IModule
    {
        OsLinux(core::Core& core);

        // os::IModule
        bool            setup               () override;
        bool            is_kernel_address   (uint64_t ptr) override;
        bool            can_inject_fault    (uint64_t ptr) override;
        bool            reader_setup        (reader::Reader& reader, opt<proc_t> proc) override;
        sym::Symbols&   kernel_symbols      () override;

        bool                proc_list       (on_proc_fn on_process) override;
        opt<proc_t>         proc_current    () override;
        opt<proc_t>         proc_find       (std::string_view name, flags_e flags) override;
        opt<proc_t>         proc_find       (uint64_t pid) override;
        opt<std::string>    proc_name       (proc_t proc) override;
        bool                proc_is_valid   (proc_t proc) override;
        uint64_t            proc_id         (proc_t proc) override;
        flags_e             proc_flags      (proc_t proc) override;
        void                proc_join       (proc_t proc, os::join_e join) override;
        opt<phy_t>          proc_resolve    (proc_t proc, uint64_t ptr) override;
        opt<proc_t>         proc_select     (proc_t proc, uint64_t ptr) override;
        opt<proc_t>         proc_parent     (proc_t proc) override;

        bool            thread_list     (proc_t proc, on_thread_fn on_thread) override;
        opt<thread_t>   thread_current  () override;
        opt<proc_t>     thread_proc     (thread_t thread) override;
        opt<uint64_t>   thread_pc       (proc_t proc, thread_t thread) override;
        uint64_t        thread_id       (proc_t proc, thread_t thread) override;

        bool                mod_list(proc_t proc, on_mod_fn on_module) override;
        opt<std::string>    mod_name(proc_t proc, mod_t mod) override;
        opt<span_t>         mod_span(proc_t proc, mod_t mod) override;
        opt<mod_t>          mod_find(proc_t proc, uint64_t addr) override;

        bool                vm_area_list    (proc_t proc, on_vm_area_fn on_vm_area) override;
        opt<vm_area_t>      vm_area_find    (proc_t proc, uint64_t addr) override;
        opt<span_t>         vm_area_span    (proc_t proc, vm_area_t vm_area) override;
        vma_access_e        vm_area_access  (proc_t proc, vm_area_t vm_area) override;
        vma_type_e          vm_area_type    (proc_t proc, vm_area_t vm_area) override;
        opt<std::string>    vm_area_name    (proc_t proc, vm_area_t vm_area) override;

        bool                driver_list (on_driver_fn on_driver) override;
        opt<driver_t>       driver_find (uint64_t addr) override;
        opt<std::string>    driver_name (driver_t drv) override;
        opt<span_t>         driver_span (driver_t drv) override;

        opt<bpid_t> listen_proc_create  (const on_proc_event_fn& on_create) override;
        opt<bpid_t> listen_proc_delete  (const on_proc_event_fn& on_delete) override;
        opt<bpid_t> listen_thread_create(const on_thread_event_fn& on_create) override;
        opt<bpid_t> listen_thread_delete(const on_thread_event_fn& on_delete) override;
        opt<bpid_t> listen_mod_create   (const on_mod_event_fn& on_create) override;
        opt<bpid_t> listen_drv_create   (const on_drv_event_fn& on_drv) override;
        size_t      unlisten            (bpid_t bpid) override;

        opt<arg_t>  read_stack  (size_t index) override;
        opt<arg_t>  read_arg    (size_t index) override;
        bool        write_arg   (size_t index, arg_t arg) override;

        void debug_print() override;

        // members
        core::Core&    core_;
        sym::Symbols   syms_;
        reader::Reader reader_;
        LinuxOffsets   offsets_;
        LinuxSymbols   symbols_;
        uint64_t per_cpu = 0;
        uint64_t kpgd    = 0;
        version kversion = "0";
        uint64_t pt_regs_size;
    };
}

OsLinux::OsLinux(core::Core& core)
    : core_(core)
    , reader_(reader::make(core)) // kernel page directory is setted up later during setup()
{
}

namespace
{
    // dmesg -t | grep -i "Linux version" | sha1sum | cut -c1-40
    static opt<std::string> read_str(reader::Reader& reader, const uint64_t& addr, const unsigned int& buffer_size)
    {
        std::string ret;
        std::vector<char> buffer(buffer_size + 1);
        uint64_t offset = 0;

        do
        {
            const auto ok = reader.read(&buffer[0], addr + offset, buffer_size);
            if(!ok)
                return FAIL(ext::nullopt, "unable to read {} bytes at address ({:#x})", buffer_size, addr + offset);

            buffer.back() = 0;
            ret.append(&buffer[0]);
            offset += buffer_size;

        } while(strlen(&buffer[0]) >= buffer_size);

        return ret;
    }
}

namespace
{
    static bool set_per_cpu(OsLinux& p)
    {
        auto per_cpu = p.core_.regs.read(MSR_GS_BASE);
        if(!p.is_kernel_address(per_cpu))
        {
            per_cpu = p.core_.regs.read(MSR_KERNEL_GS_BASE);
            if(!p.is_kernel_address(per_cpu))
                return FAIL(false, "unable to find per_cpu address");
        }

        p.per_cpu = per_cpu;
        return true;
    }

    static bool set_kernel_page_dir(OsLinux& p, std::function<bool(uint64_t)> check)
    {
        auto kpgd = p.core_.regs.read(FDP_CR3_REGISTER);
        kpgd &= ~0x1fffull; // clear 12th bits due to meltdown patch
        if(!check(kpgd))
        {
            kpgd |= 0x1000ull; // try with orginal CR3 in case system is not meltdown patched
            if(!check(kpgd))
                return FAIL(false, "unable to find a valid kernel page directory");
        }

        p.kpgd              = kpgd;
        p.reader_.kdtb_.val = kpgd;
        return true;
    }

    static bool find_linux_banner(OsLinux& p, fn::view<walk_e(uint64_t)> on_candidate)
    {
        if(!p.kpgd)
            return FAIL(false, "finding linux_banner requires a kernel page directory");

        const char target[] = {'L', 'i', 'n', 'u', 'x', ' ', 'v', 'e', 'r', 's', 'i', 'o', 'n'};
        // compability was checked for kernel from 2.6.12 (2005) to 5.1.2 (2019)
        const auto pattern = boyer_moore_searcher(std::begin(target), std::end(target));

        std::vector<char> buffer(PAGE_SIZE + sizeof target);

        const uint64_t START_KERNEL = 0xffffffff80000000, END_KERNEL = 0xfffffffffff00000;
        // compability was checked for kernel from 2.6.27 (2008) to 5.1.2 (2019)

        const auto buffer_begin    = &buffer[0];
        const auto buffer_afterend = buffer.data() + buffer.size();

        uint64_t offset = START_KERNEL;
        while(offset <= END_KERNEL)
        {
            if(p.reader_.read(&buffer[sizeof target], offset, PAGE_SIZE))
            {
                const auto match = search(buffer_begin, buffer_afterend, pattern);
                if(match != buffer_afterend)
                    if(on_candidate((offset + (match - buffer_begin)) - sizeof target) == WALK_STOP)
                        return true;
                std::memcpy(buffer_begin, &buffer[buffer.size() - sizeof target], sizeof target);
            }
            else
                buffer[sizeof target - 1] = 0;
            offset += PAGE_SIZE;
        }

        return false;
    }

    static opt<std::string> get_linux_banner(reader::Reader reader, uint64_t addr)
    {
        auto str = read_str(reader, addr, 256); // for recent ubuntu, linux_banner length is about 180 bytes
        if(!str)
            return {};

        if(!str->empty() && str->back() == '\n')
            str->pop_back();

        return str;
    }

    static bool check_setup(OsLinux& p)
    {
        if(!p.kpgd | !p.per_cpu)
            return false;

        opt<proc_t> init_task = {};
        p.proc_list([&](proc_t proc)
        {
            if(p.proc_id(proc) != 0)
                return WALK_NEXT;

            init_task = proc;
            return WALK_STOP;
        });
        if(!init_task)
            return false;

        const auto name = p.proc_name(*init_task);
        if(!name)
            return false;

        return (name->substr(0, 7) == "swapper");
    }
}

namespace
{
    static std::string bytesToStr(const std::vector<unsigned char>& in)
    {
        auto from = in.cbegin();
        auto to   = in.cend();
        std::ostringstream oss;
        for(; from != to; ++from)
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(*from);
        return oss.str();
    }

    static std::string guid(const std::string str) // todo - simplify
    {
        std::vector<unsigned char> vstr(str.data(), str.data() + str.length());
        unsigned char hash[20]; // sha1 length
        mbedtls_sha1(vstr.data(), vstr.size(), hash);

        std::vector<unsigned char>  vhash       (hash, hash + 20);
        return bytesToStr  (vhash);
    }
}

namespace
{
    static opt<uint64_t> make_symbols(sym::Symbols& syms, const std::string& guid, const std::string& strSymbol, const uint64_t& addrSymbol)
    {
        syms.remove("kernel_struct");
        syms.remove("kernel_sym");

        auto dwarf = sym::make_dwarf({}, "kernel", guid);
        if(!dwarf || !syms.insert("kernel_struct", dwarf))
            return FAIL(ext::nullopt, "unable to read _LINUX_SYMBOL_PATH/kernel/{}/elf", guid);

        auto sysmap = sym::make_map({}, "kernel", guid);
        if(!sysmap || !sysmap->set_aslr(strSymbol, addrSymbol))
            return FAIL(ext::nullopt, "unable to read _LINUX_SYMBOL_PATH/kernel/{}/System.map file", guid);

        const auto kaslr = sysmap->get_aslr();

        std::unique_ptr<sym::IMod> sysmap_imod = std::move(sysmap);
        if(!syms.insert("kernel_sym", sysmap_imod))
            return FAIL(ext::nullopt, "unable to read System.map file");

        return kaslr;
    }

    static bool loadOffsets(sym::Symbols& syms, LinuxOffsets& offsets)
    {
        bool fail = false;
        int i     = -1;
        memset(&offsets[0], 0, sizeof offsets);
        for(const auto& off : g_offsets)
        {
            fail |= off.e_id != ++i;
            const auto offset = syms.struc_offset(off.module, off.struc, off.member);
            if(!offset)
            {
                fail |= off.e_cat == cat_e::REQUIRED;
                if(off.e_cat == cat_e::REQUIRED)
                    LOG(ERROR, "unable to read {}!{}.{} member offset", off.module, off.struc, off.member);
                continue;
            }
            offsets[i] = *offset;
        }
        return !fail;
    }

    static bool loadSymbols(sym::Symbols& syms, LinuxSymbols& symbols)
    {
        bool fail = false;
        int i     = -1;
        memset(&symbols[0], 0, sizeof symbols);
        for(const auto& sym : g_symbols)
        {
            fail |= sym.e_id != ++i;
            const auto addr = syms.symbol(sym.module, sym.name);
            if(!addr)
            {
                fail |= sym.e_cat == cat_e::REQUIRED;
                if(sym.e_cat == cat_e::REQUIRED)
                    LOG(ERROR, "unable to read {}!{} symbol offset", sym.module, sym.name);
                continue;
            }
            symbols[i] = *addr;
        }
        return !fail;
    }
}

bool OsLinux::setup()
{
    if(!set_per_cpu(*this))
        return false;

    auto ok = set_kernel_page_dir(*this, [&](uint64_t kpgd)
    {
        auto reader      = reader::make(core_);
        reader.kdtb_.val = kpgd;
        return (!!reader.read(per_cpu));
    });

    std::regex pattern("^Linux version ((\?:\\.\?\\d+)+)");
    std::smatch match;
    bool firstattempt = true;
    ok                = find_linux_banner(*this, [&](uint64_t candidate)
    {
        if(firstattempt)
            firstattempt = false;
        else
            LOG(INFO, "try with next linux banner...");

        const auto linux_banner = get_linux_banner(reader_, candidate);
        if(!linux_banner)
            return WALK_NEXT;
        LOG(INFO, "linux banner found '{}'", *linux_banner);

        const auto hash  = guid(*linux_banner);
        const auto kaslr = make_symbols(syms_, hash, "linux_banner", candidate);
        if(!kaslr)
            return WALK_NEXT;

        if(!loadOffsets(syms_, offsets_) | !loadSymbols(syms_, symbols_))
            return WALK_NEXT;

        if(!check_setup(*this))
            return WALK_NEXT;

        if(!std::regex_search(*linux_banner, match, pattern))
            return FAIL(WALK_NEXT, "unable to parse kernel version in this linux banner");
        kversion = match[1].str();

        const auto opt_pt_regs_size = syms_.struc_size("kernel_struct", "pt_regs");
        if(!opt_pt_regs_size)
            return FAIL(WALK_NEXT, "unable to read the size of pt_regs structure");
        pt_regs_size = *opt_pt_regs_size;

        LOG(INFO, "kernel {} loaded with kaslr {:#x}", kversion.get(), *kaslr);
        return WALK_STOP;
    });

    if(!ok)
        return FAIL(false, "unable to find kernel");

    return true;
}

std::unique_ptr<os::IModule> os::make_linux(core::Core& core)
{
    return std::make_unique<OsLinux>(core);
}

bool OsLinux::proc_list(on_proc_fn on_process)
{
    const auto current = proc_current();
    if(!current)
        return false;

    const auto head    = current->id + offsets_[TASKSTRUCT_TASKS];
    opt<uint64_t> link = head;
    do
    {
        const auto thread = thread_t{*link - offsets_[TASKSTRUCT_TASKS]};
        const auto proc   = thread_proc(thread);
        if(proc)
            if(on_process(*proc) == WALK_STOP)
                return true;

        link = reader_.read(*link);
        if(!link)
            return FAIL(false, "unable to read next process address");
    } while(link != head);

    return true;
}

opt<proc_t> OsLinux::proc_current()
{
    const auto thread = thread_current();
    if(!thread)
        return {};

    return thread_proc(*thread);
}

opt<proc_t> OsLinux::proc_find(std::string_view name, flags_e /*flags*/)
{
    opt<proc_t> found;
    proc_list([&](proc_t proc)
    {
        const auto got = proc_name(proc);
        if(*got != name)
            return WALK_NEXT;

        found = proc;
        return WALK_STOP;
    });
    return found;
}

opt<proc_t> OsLinux::proc_find(uint64_t pid)
{
    opt<proc_t> ret;
    proc_list([&](proc_t proc)
    {
        const auto got = proc_id(proc);
        if(got > 4194304) // PID <= 4194304 for linux
            return FAIL(WALK_NEXT, "unable to find the pid of proc {:#x}", proc.id);

        if(got != pid)
            return WALK_NEXT;

        ret = proc;
        return WALK_STOP;
    });
    return ret;
}

opt<std::string> OsLinux::proc_name(proc_t proc)
{
    return read_str(reader_, proc.id + offsets_[TASKSTRUCT_COMM], 16);
    // 16 is COMM member length
    // todo
    //   -> create a member_size() in sym::IMod
    //   -> or set the buffer length to 1 (read_str will iterate until \x00)
    //   -> or set a bigger buffer to avoid multiple FDP calls
}

uint64_t OsLinux::proc_id(proc_t proc)
{
    return thread_id({}, thread_t{proc.id});
}

bool OsLinux::proc_is_valid(proc_t /*proc*/)
{
    return true;
}

bool OsLinux::is_kernel_address(uint64_t ptr)
{
    return (ptr > 0x7fffffffffffffff); // middle of 64bits address space, under -> user space, upper -> kernel space
}

bool OsLinux::can_inject_fault(uint64_t /*ptr*/)
{
    return false;
}

flags_e OsLinux::proc_flags(proc_t proc) // compatibility checked until v5.2-rc5 (06/2019)
{
    unsigned char TIF_IA32 = 17, TIF_ADDR32 = 29, TIF_X32 = 30; // see /arch/x86/include/asm/thread_info.h

    uint32_t mask = 1ul << TIF_IA32;
    if(kversion >= version("3.4"))
        mask |= (1ul << TIF_ADDR32 | 1ul << TIF_X32);

    const auto thread_info = reader_.le32(proc.id + offsets_[TASKSTRUCT_THREADINFO]);
    if(!thread_info)
        return FAIL(FLAGS_NONE, "unable to read thread_info flags of process {:#x}", proc.id);

    if(!(*thread_info & mask))
        return FLAGS_NONE;

    return FLAGS_32BIT;
}

namespace
{
    opt<uint64_t> pt_regs(OsLinux& p, thread_t thread)
    {
        /*
		Offsets of this method are reliable only for an intel x86_64 CPU
		and a stable version of kernel (e.g it will not work properly with a kernel v4.0-rc3, 3.15-rc5, ...)
		*/

        const auto stack_ptr = p.reader_.read(thread.id + p.offsets_[TASKSTRUCT_STACK]);
        if(!stack_ptr)
            return FAIL(ext::nullopt, "unable to read task_struct->stack of {:#x} thread", thread.id);

        uint8_t THREAD_SIZE_ORDER;
        if(p.kversion < version("3.15"))
            THREAD_SIZE_ORDER = 1;
        else if(p.kversion < version("4"))
            THREAD_SIZE_ORDER = 2;
        else
        {
            const uint8_t KASAN_STACK_ORDER = (p.symbols_[KASAN_INIT] != 0);
            THREAD_SIZE_ORDER               = 2 + KASAN_STACK_ORDER;
        }
        const uint64_t THREAD_SIZE = PAGE_SIZE << THREAD_SIZE_ORDER;

        uint8_t TOP_OF_KERNEL_STACK_PADDING;
        if(p.kversion < version("4"))
            TOP_OF_KERNEL_STACK_PADDING = 8;
        else
            TOP_OF_KERNEL_STACK_PADDING = 0;

        const auto start_stack = *stack_ptr + THREAD_SIZE - TOP_OF_KERNEL_STACK_PADDING;
        return start_stack - p.pt_regs_size;
    }
}

namespace
{
    static uint8_t cpu_ring(OsLinux& p)
    {
        return p.core_.regs.read(FDP_CS_REGISTER) & 0b11ull;
    }

    static void proc_join_any(OsLinux& p, proc_t proc)
    {
        std::unordered_set<uint64_t> ptrs;
        p.thread_list(proc, [&](thread_t thread)
        {
            const auto ptr = p.thread_pc({}, thread);
            if(!ptr)
            {
                ptrs.clear();
                return FAIL(WALK_STOP, "unable to find the return address of thread {:#x}", thread.id);
            }

            ptrs.insert(*ptr);
            return WALK_NEXT;
        });

        if(ptrs.empty())
            LOG(ERROR, "unable to proc_join_any on process {:#x}", proc.id);
        else
        {
            p.core_.state.run_to(std::string_view("proc_join_any"), ptrs, core::BP_CR3_NONE, [&](proc_t bp_proc)
            {
                return (bp_proc.id == proc.id) ? WALK_STOP : WALK_NEXT;
            });
        }
    }

    void run_until_next_cr3(OsLinux& p)
    {
        p.core_.state.run_to(std::string_view("next_cr3"), std::unordered_set<uint64_t>(), core::BP_CR3_ON_WRITINGS, [&](proc_t)
        {
            return WALK_STOP;
        });
    }
}

void OsLinux::proc_join(proc_t proc, os::join_e join)
{
    while(true)
    {
        const auto current = proc_current();

        if(current->id == proc.id && ((join == os::JOIN_ANY_MODE) | (join == os::JOIN_USER_MODE && cpu_ring(*this) == 3)))
            return;

        if(current->id != proc.id)
            proc_join_any(*this, proc);

        // here, we are in the targetted process and in kernel mode
        // if it was already the case, we did not move

        if(join == os::JOIN_ANY_MODE)
            return;

        // and we want to join the user mode...
        const auto pt_regs_ptr = pt_regs(*this, thread_t{proc.id});
        if(!pt_regs_ptr)
        {
            LOG(ERROR, "unable to find pt_regs struct of thread {:#x}", proc.id);
            run_until_next_cr3(*this);
            continue;
        }

        const auto user_rip = reader_.read(*pt_regs_ptr + offsets_[PTREGS_IP]);
        if(!user_rip)
            LOG(ERROR, "unable to read rip in pt_regs struct of thread {:#x}", proc.id);
        if(!user_rip || !(*user_rip))
        {
            run_until_next_cr3(*this);
            continue;
        }

        core_.state.run_to(std::string_view("proc_join_user"), std::unordered_set<uint64_t>{*user_rip}, core::BP_CR3_ON_WRITINGS, [&](proc_t bp_proc)
        {
            if((cpu_ring(*this) == 3) | (bp_proc.id != proc.id))
                return WALK_STOP;

            // we are still in the targetted process and in kernel mode
            const auto updated_pt_regs_ptr = pt_regs(*this, thread_t{proc.id});
            if(!updated_pt_regs_ptr)
                return WALK_STOP;

            const auto updated_user_rip = reader_.read(*updated_pt_regs_ptr + offsets_[PTREGS_IP]);
            if(!updated_user_rip || *user_rip != *updated_user_rip)
                return WALK_STOP;

            // nothing changed
            return WALK_NEXT;
        });
    }
}

opt<phy_t> OsLinux::proc_resolve(proc_t proc, uint64_t ptr)
{
    const auto select = proc_select(proc, ptr);
    if(!select)
        return {};

    return core_.mem.virtual_to_physical(ptr, select->dtb);
}

opt<proc_t> OsLinux::proc_select(proc_t proc, uint64_t ptr)
{
    if(!is_kernel_address(ptr))
        return proc;

    return proc_t{proc.id, dtb_t{kpgd}};
}

opt<proc_t> OsLinux::proc_parent(proc_t proc)
{
    const auto thread_parent = reader_.read(proc.id + offsets_[TASKSTRUCT_REALPARENT]);
    if(!thread_parent)
        return FAIL(ext::nullopt, "unable to read pointer to the parent of process {:#x}", proc.id);

    return thread_proc(thread_t{*thread_parent});
}

bool OsLinux::reader_setup(reader::Reader& reader, opt<proc_t> proc)
{
    reader.udtb_ = (proc) ? proc->dtb : dtb_t{0};
    reader.kdtb_ = dtb_t{kpgd};
    return true;
}

sym::Symbols& OsLinux::kernel_symbols()
{
    return syms_;
}

bool OsLinux::thread_list(proc_t proc, on_thread_fn on_thread)
{
    const auto head    = proc.id + offsets_[TASKSTRUCT_THREADGROUP];
    opt<uint64_t> link = head;
    do
    {
        on_thread(thread_t{*link - offsets_[TASKSTRUCT_THREADGROUP]});

        link = reader_.read(*link);
        if(!link)
            return FAIL(false, "unable to read next thread address");
    } while(link != head);

    return true;
}

opt<thread_t> OsLinux::thread_current()
{
    const auto addr = reader_.read(per_cpu + symbols_[CURRENT_TASK] - symbols_[PER_CPU_START]);
    if(!addr)
        return FAIL(ext::nullopt, "unable to read current_task in per_cpu area");

    return thread_t{*addr};
}

namespace
{
    opt<uint64_t> mm_pgd(OsLinux& p, uint64_t mm)
    {
        const auto pgd_t = p.reader_.read(mm + p.offsets_[MMSTRUCT_PGD]);
        if(!pgd_t)
            return FAIL(ext::nullopt, "unable to read pgd_t at {:#x} in mm_struct of process", mm + p.offsets_[MMSTRUCT_PGD]);

        const auto pgd = p.core_.mem.virtual_to_physical(*pgd_t, dtb_t{p.kpgd});
        if(!pgd)
            return FAIL(ext::nullopt, "unable to find the pgd converting virtual addr {:#x} to physical one", *pgd_t);

        return pgd->val;
    }
}

opt<proc_t> OsLinux::thread_proc(thread_t thread)
{
    const auto proc_id = reader_.read(thread.id + offsets_[TASKSTRUCT_GROUPLEADER]);
    if(!proc_id)
        return FAIL(ext::nullopt, "unable to find the leader of thread {:#x}", thread.id);

    const auto mm = reader_.read(*proc_id + offsets_[TASKSTRUCT_MM]);
    if(!mm | !(*mm))
        return proc_t{*proc_id, dtb_t{0}};

    const auto pgd = mm_pgd(*this, *mm);
    if(!pgd)
        return proc_t{*proc_id, dtb_t{0}};

    return proc_t{*proc_id, dtb_t{*pgd}};
}

opt<uint64_t> OsLinux::thread_pc(proc_t /*proc*/, thread_t thread)
{
    const auto current = thread_current();
    if(!current)
        return {};

    if(thread.id == current->id)
        return core_.regs.read(FDP_RIP_REGISTER);

    const auto pt_regs_ptr = pt_regs(*this, thread);
    if(!pt_regs_ptr)
        return FAIL(ext::nullopt, "unable to find pt_regs struct of thread {:#x}", thread.id);

    return reader_.read(*pt_regs_ptr - 8);
}

uint64_t OsLinux::thread_id(proc_t /*proc*/, thread_t thread) // return opt ?, remove proc ?
{
    const auto pid = reader_.le32(thread.id + offsets_[TASKSTRUCT_PID]);
    if(!pid)
        return 0xffffffffffffffff; // opt<uint64_t> either ? (0 is a valid pid for an iddle task in linux)

    return *pid;
}

bool OsLinux::mod_list(proc_t /*proc*/, on_mod_fn on_module)
{
    mod_t dummy_mod = {0, FLAGS_NONE};
    on_module(dummy_mod);
    return true;
}

opt<std::string> OsLinux::mod_name(proc_t /*proc*/, mod_t /*mod*/)
{
    return {};
}

opt<span_t> OsLinux::mod_span(proc_t /*proc*/, mod_t /*mod*/)
{
    return {};
}

opt<mod_t> OsLinux::mod_find(proc_t /*proc*/, uint64_t /*addr*/)
{
    return {};
}

bool OsLinux::vm_area_list(proc_t /*proc*/, on_vm_area_fn /*on_vm_area*/)
{
    return false;
}

opt<vm_area_t> OsLinux::vm_area_find(proc_t /*proc*/, uint64_t /*addr*/)
{
    return {};
}

opt<span_t> OsLinux::vm_area_span(proc_t /*proc*/, vm_area_t /*vm_area*/)
{
    return {};
}

vma_access_e OsLinux::vm_area_access(proc_t /*proc*/, vm_area_t /*vm_area*/)
{
    return VMA_ACCESS_NONE;
}

vma_type_e OsLinux::vm_area_type(proc_t /*proc*/, vm_area_t /*vm_area*/)
{
    return vma_type_e::none;
}

opt<std::string> OsLinux::vm_area_name(proc_t /*proc*/, vm_area_t /*vm_area*/)
{
    return {};
}

bool OsLinux::driver_list(on_driver_fn on_driver)
{
    driver_t dummy_driver = {0};
    on_driver(dummy_driver);
    return true;
}

opt<driver_t> OsLinux::driver_find(uint64_t /*addr*/)
{
    return {};
}

opt<std::string> OsLinux::driver_name(driver_t /*drv*/)
{
    return {};
}

opt<span_t> OsLinux::driver_span(driver_t /*drv*/)
{
    return {};
}

opt<arg_t> OsLinux::read_stack(size_t /*index*/)
{
    return {};
}

opt<arg_t> OsLinux::read_arg(size_t /*index*/)
{
    return {};
}

bool OsLinux::write_arg(size_t /*index*/, arg_t /*arg*/)
{
    return false;
}

void OsLinux::debug_print()
{
}

opt<OsLinux::bpid_t> OsLinux::listen_proc_create(const on_proc_event_fn& /*on_create*/)
{
    return {};
}

opt<OsLinux::bpid_t> OsLinux::listen_proc_delete(const on_proc_event_fn& /*on_delete*/)
{
    return {};
}

opt<OsLinux::bpid_t> OsLinux::listen_thread_create(const on_thread_event_fn& /*on_create*/)
{
    return {};
}

opt<OsLinux::bpid_t> OsLinux::listen_thread_delete(const on_thread_event_fn& /*on_remove*/)
{
    return {};
}

opt<OsLinux::bpid_t> OsLinux::listen_mod_create(const on_mod_event_fn& /*on_create*/)
{
    return {};
}

opt<OsLinux::bpid_t> OsLinux::listen_drv_create(const on_drv_event_fn&)
{
    return {};
}

size_t OsLinux::unlisten(bpid_t /*bpid*/)
{
    return {};
}
