#include "heaps.gen.hpp"

#define FDP_MODULE "heaps"
#include "log.hpp"
#include "os.hpp"

#include <map>

namespace
{
	constexpr bool g_debug = false;

	static const tracer::callcfg_t g_callcfgs[] =
	{
        {"RtlpAllocateHeapInternal", 2, {{"PVOID", "HeapHandle", sizeof(nt::PVOID)}, {"SIZE_T", "Size", sizeof(nt::SIZE_T)}}},
        {"RtlFreeHeap", 3, {{"PVOID", "HeapHandle", sizeof(nt::PVOID)}, {"ULONG", "Flags", sizeof(nt::ULONG)}, {"PVOID", "BaseAddress", sizeof(nt::PVOID)}}},
        {"RtlpReAllocateHeapInternal", 4, {{"PVOID", "HeapHandle", sizeof(nt::PVOID)}, {"ULONG", "Flags", sizeof(nt::ULONG)}, {"PVOID", "BaseAddress", sizeof(nt::PVOID)}, {"ULONG", "Size", sizeof(nt::ULONG)}}},
        {"RtlSizeHeap", 3, {{"PVOID", "HeapHandle", sizeof(nt::PVOID)}, {"ULONG", "Flags", sizeof(nt::ULONG)}, {"PVOID", "BaseAddress", sizeof(nt::PVOID)}}},
        {"RtlSetUserValueHeap", 4, {{"PVOID", "HeapHandle", sizeof(nt::PVOID)}, {"ULONG", "Flags", sizeof(nt::ULONG)}, {"PVOID", "BaseAddress", sizeof(nt::PVOID)}, {"PVOID", "UserValue", sizeof(nt::PVOID)}}},
        {"RtlGetUserInfoHeap", 5, {{"PVOID", "HeapHandle", sizeof(nt::PVOID)}, {"ULONG", "Flags", sizeof(nt::ULONG)}, {"PVOID", "BaseAddress", sizeof(nt::PVOID)}, {"PVOID", "UserValue", sizeof(nt::PVOID)}, {"PULONG", "UserFlags", sizeof(nt::PULONG)}}},
	};

    using bpid_t    = nt::heaps::bpid_t;
    using Listeners = std::multimap<bpid_t, core::Breakpoint>;
}

struct nt::heaps::Data
{
    Data(core::Core& core, sym::Symbols& syms, std::string_view module);

    core::Core&   core;
    sym::Symbols& syms;
    std::string   module;
    Listeners     listeners;
    bpid_t        last_id;
};

nt::heaps::Data::Data(core::Core& core, sym::Symbols& syms, std::string_view module)
    : core(core)
    , syms(syms)
    , module(module)
    , last_id(0)
{
}

nt::heaps::heaps(core::Core& core, sym::Symbols& syms, std::string_view module)
    : d_(std::make_unique<Data>(core, syms, module))
{
}

nt::heaps::~heaps() = default;

namespace
{
    static opt<bpid_t> register_callback(nt::heaps::Data& d, bpid_t id, proc_t proc, const char* name, const core::Task& on_call)
    {
        const auto addr = d.syms.symbol(d.module, name);
        if(!addr)
            return FAIL(ext::nullopt, "unable to find symbole {}!{}", d.module, name);

        const auto bp = d.core.state.set_breakpoint(name, *addr, proc, on_call);
        if(!bp)
            return FAIL(ext::nullopt, "unable to set breakpoint");

        d.listeners.emplace(id, bp);
        return id;
    }

    template <typename T>
    static T arg(core::Core& core, size_t i)
    {
        const auto arg = core.os->read_arg(i);
        if(!arg)
            return {};

        T value = {};
        static_assert(sizeof value <= sizeof arg->val, "invalid size");
        memcpy(&value, &arg->val, sizeof value);
        return value;
    }
}

opt<bpid_t> nt::heaps::register_RtlpAllocateHeapInternal(proc_t proc, const on_RtlpAllocateHeapInternal_fn& on_func)
{
    return register_callback(*d_, ++d_->last_id, proc, "RtlpAllocateHeapInternal", [=]
    {
        auto& core = d_->core;
        
        const auto HeapHandle = arg<nt::PVOID>(core, 0);
        const auto Size       = arg<nt::SIZE_T>(core, 1);

        if constexpr(g_debug)
            tracer::log_call(core, g_callcfgs[0]);

        on_func(HeapHandle, Size);
    });
}

opt<bpid_t> nt::heaps::register_RtlFreeHeap(proc_t proc, const on_RtlFreeHeap_fn& on_func)
{
    return register_callback(*d_, ++d_->last_id, proc, "RtlFreeHeap", [=]
    {
        auto& core = d_->core;
        
        const auto HeapHandle  = arg<nt::PVOID>(core, 0);
        const auto Flags       = arg<nt::ULONG>(core, 1);
        const auto BaseAddress = arg<nt::PVOID>(core, 2);

        if constexpr(g_debug)
            tracer::log_call(core, g_callcfgs[1]);

        on_func(HeapHandle, Flags, BaseAddress);
    });
}

opt<bpid_t> nt::heaps::register_RtlpReAllocateHeapInternal(proc_t proc, const on_RtlpReAllocateHeapInternal_fn& on_func)
{
    return register_callback(*d_, ++d_->last_id, proc, "RtlpReAllocateHeapInternal", [=]
    {
        auto& core = d_->core;
        
        const auto HeapHandle  = arg<nt::PVOID>(core, 0);
        const auto Flags       = arg<nt::ULONG>(core, 1);
        const auto BaseAddress = arg<nt::PVOID>(core, 2);
        const auto Size        = arg<nt::ULONG>(core, 3);

        if constexpr(g_debug)
            tracer::log_call(core, g_callcfgs[2]);

        on_func(HeapHandle, Flags, BaseAddress, Size);
    });
}

opt<bpid_t> nt::heaps::register_RtlSizeHeap(proc_t proc, const on_RtlSizeHeap_fn& on_func)
{
    return register_callback(*d_, ++d_->last_id, proc, "RtlSizeHeap", [=]
    {
        auto& core = d_->core;
        
        const auto HeapHandle  = arg<nt::PVOID>(core, 0);
        const auto Flags       = arg<nt::ULONG>(core, 1);
        const auto BaseAddress = arg<nt::PVOID>(core, 2);

        if constexpr(g_debug)
            tracer::log_call(core, g_callcfgs[3]);

        on_func(HeapHandle, Flags, BaseAddress);
    });
}

opt<bpid_t> nt::heaps::register_RtlSetUserValueHeap(proc_t proc, const on_RtlSetUserValueHeap_fn& on_func)
{
    return register_callback(*d_, ++d_->last_id, proc, "RtlSetUserValueHeap", [=]
    {
        auto& core = d_->core;
        
        const auto HeapHandle  = arg<nt::PVOID>(core, 0);
        const auto Flags       = arg<nt::ULONG>(core, 1);
        const auto BaseAddress = arg<nt::PVOID>(core, 2);
        const auto UserValue   = arg<nt::PVOID>(core, 3);

        if constexpr(g_debug)
            tracer::log_call(core, g_callcfgs[4]);

        on_func(HeapHandle, Flags, BaseAddress, UserValue);
    });
}

opt<bpid_t> nt::heaps::register_RtlGetUserInfoHeap(proc_t proc, const on_RtlGetUserInfoHeap_fn& on_func)
{
    return register_callback(*d_, ++d_->last_id, proc, "RtlGetUserInfoHeap", [=]
    {
        auto& core = d_->core;
        
        const auto HeapHandle  = arg<nt::PVOID>(core, 0);
        const auto Flags       = arg<nt::ULONG>(core, 1);
        const auto BaseAddress = arg<nt::PVOID>(core, 2);
        const auto UserValue   = arg<nt::PVOID>(core, 3);
        const auto UserFlags   = arg<nt::PULONG>(core, 4);

        if constexpr(g_debug)
            tracer::log_call(core, g_callcfgs[5]);

        on_func(HeapHandle, Flags, BaseAddress, UserValue, UserFlags);
    });
}

opt<bpid_t> nt::heaps::register_all(proc_t proc, const nt::heaps::on_call_fn& on_call)
{
    const auto id   = ++d_->last_id;
    const auto size = d_->listeners.size();
    for(const auto cfg : g_callcfgs)
        register_callback(*d_, id, proc, cfg.name, [=]{ on_call(cfg); });

    if(size == d_->listeners.size())
        return {};

    return id;
}

bool nt::heaps::unregister(bpid_t id)
{
    return d_->listeners.erase(id) > 0;
}
