#include "fdp.hpp"

#define FDP_MODULE "fdp"
#include "log.hpp"

extern "C"
{
#include <FDP.h>
}

namespace
{
    static fdp::shm* cast(FDP_SHM* shm)
    {
        return reinterpret_cast<fdp::shm*>(shm);
    }

    static FDP_SHM* cast(fdp::shm* shm)
    {
        return reinterpret_cast<FDP_SHM*>(shm);
    }
}

fdp::shm* fdp::open(const char* name)
{
    const auto ptr = FDP_OpenSHM(name);
    return cast(ptr);
}

bool fdp::init(shm& shm)
{
    return FDP_Init(cast(&shm));
}

void fdp::reset(shm& shm)
{
    auto ptr = cast(&shm);
    FDP_Pause(ptr);

    for(int bpid = 0; bpid < FDP_MAX_BREAKPOINT; bpid++)
        FDP_UnsetBreakpoint(ptr, bpid);

    FDP_WriteRegister(ptr, 0, FDP_DR0_REGISTER, 0);
    FDP_WriteRegister(ptr, 0, FDP_DR1_REGISTER, 0);
    FDP_WriteRegister(ptr, 0, FDP_DR2_REGISTER, 0);
    FDP_WriteRegister(ptr, 0, FDP_DR3_REGISTER, 0);
    FDP_WriteRegister(ptr, 0, FDP_DR6_REGISTER, 0);
    FDP_WriteRegister(ptr, 0, FDP_DR7_REGISTER, 0);
}

opt<FDP_State> fdp::state(shm& shm)
{
    FDP_State value = 0;
    const auto ok   = FDP_GetState(cast(&shm), &value);
    if(!ok)
        return {};

    return value;
}

bool fdp::state_changed(shm& shm)
{
    return FDP_GetStateChanged(cast(&shm));
}

bool fdp::pause(shm& shm)
{
    return FDP_Pause(cast(&shm));
}

bool fdp::resume(shm& shm)
{
    return FDP_Resume(cast(&shm));
}

bool fdp::step_once(shm& shm)
{
    return FDP_SingleStep(cast(&shm), 0);
}

bool fdp::unset_breakpoint(shm& shm, int bpid)
{
    return FDP_UnsetBreakpoint(cast(&shm), bpid);
}

int fdp::set_breakpoint(shm& shm, FDP_BreakpointType type, int bpid, FDP_Access access, FDP_AddressType ptrtype, uint64_t ptr, uint64_t len, uint64_t cr3)
{
    return FDP_SetBreakpoint(cast(&shm), 0, type, bpid, access, ptrtype, ptr, len, cr3);
}

bool fdp::read_physical(shm& shm, void* vdst, size_t size, phy_t phy)
{
    const auto dst   = reinterpret_cast<uint8_t*>(vdst);
    const auto usize = static_cast<uint32_t>(size);
    return FDP_ReadPhysicalMemory(cast(&shm), dst, usize, phy.val);
}

namespace
{
    template <typename T>
    static auto switch_dtb(fdp::shm& shm, dtb_t dtb, T operand)
    {
        const auto backup      = fdp::read_register(shm, FDP_CR3_REGISTER);
        const auto need_switch = backup && *backup != dtb.val;
        if(need_switch)
            fdp::write_register(shm, FDP_CR3_REGISTER, dtb.val);
        const auto ret = operand();
        if(need_switch)
            fdp::write_register(shm, FDP_CR3_REGISTER, *backup);
        return ret;
    }
}

bool fdp::read_virtual(shm& shm, void* vdst, size_t size, dtb_t dtb, uint64_t ptr)
{
    const auto dst   = reinterpret_cast<uint8_t*>(vdst);
    const auto usize = static_cast<uint32_t>(size);
    return switch_dtb(shm, dtb, [&]
    {
        return FDP_ReadVirtualMemory(cast(&shm), 0, dst, usize, ptr);
    });
}

opt<phy_t> fdp::virtual_to_physical(shm& shm, dtb_t dtb, uint64_t ptr)
{
    uint64_t phy  = 0;
    const auto ok = switch_dtb(shm, dtb, [&]
    {
        return FDP_VirtualToPhysical(cast(&shm), 0, ptr, &phy);
    });
    if(!ok)
        return {};

    return phy_t{phy};
}

bool fdp::inject_interrupt(shm& shm, uint32_t code, uint32_t error, uint64_t cr2)
{
    return FDP_InjectInterrupt(cast(&shm), 0, code, error, cr2);
}

opt<uint64_t> fdp::read_register(shm& shm, reg_e reg)
{
    uint64_t value = 0;
    const auto ok  = FDP_ReadRegister(cast(&shm), 0, reg, &value);
    if(!ok)
        return {};

    return value;
}

opt<uint64_t> fdp::read_msr_register(shm& shm, msr_e msr)
{
    uint64_t value = 0;
    const auto ok  = FDP_ReadMsr(cast(&shm), 0, msr, &value);
    if(!ok)
        return {};

    return value;
}

bool fdp::write_register(shm& shm, reg_e reg, uint64_t value)
{
    return FDP_WriteRegister(cast(&shm), 0, reg, value);
}

bool fdp::write_msr_register(shm& shm, msr_e msr, uint64_t value)
{
    return FDP_WriteMsr(cast(&shm), 0, msr, value);
}
