#define FDP_MODULE "vm_resume"
#include <icebox/core.hpp>
#include <icebox/log.hpp>

int main(int argc, char** argv)
{
    logg::init(argc, argv);
    if(argc != 2)
        return FAIL(-1, "usage: vm_resume <name>");

    const auto name = std::string{argv[1]};
    LOG(INFO, "starting on {}", name.data());

    core::Core core;
    const auto ok = core.setup(name);
    if(!ok)
        return FAIL(-1, "unable to start core at {}", name.data());

    core.state.pause();
    core.state.resume();
    return 0;
}
