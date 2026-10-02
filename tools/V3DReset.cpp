/*
 * See the file "LICENSE" for the full license governing this code.
 */

#include <iostream>
#include <string>

#include "common.h"
#include "hal/hal.h"

using namespace vc4cl;

/*
 * Resets the V3D hardware by power-cycling it, which stops all QPU programs still running, e.g. after a kernel
 * execution timed out.
 *
 * Only works if V3D is managed by the Linux vc4 DRM driver (KMS). Needs to be run as root.
 */

static void printProgramStatus(const V3D& v3d)
{
    std::cout << "QPU user programs (requests/completed/in queue): "
              << v3d.getSystemInfo(SystemInfo::USER_REQUESTS_COUNT) << "/"
              << v3d.getSystemInfo(SystemInfo::USER_PROGRAMS_COMPLETED_COUNT) << "/"
              << v3d.getSystemInfo(SystemInfo::PROGRAM_QUEUE_LENGTH) << std::endl;
}

int main(int argc, char** argv)
{
    bool statusOnly = argc > 1 && std::string{argv[1]} == "--status";
    if(argc > 1 && !statusOnly)
    {
        std::cout << "Usage: " << argv[0] << " [--status]" << std::endl;
        std::cout << "Power-cycles the V3D hardware to stop any running QPU programs." << std::endl;
        std::cout << "With --status, only shows the QPU user program status." << std::endl;
        return 2;
    }

    if(system()->getDRMIfAvailable())
    {
        // The vc4 driver resets V3D itself when a kernel times out
        std::cout << "V3D is managed by the vc4 DRM driver with compute job support, which resets it itself. Use "
                     "VC4CL_NO_DRM=1 to access V3D directly anyway."
                  << std::endl;
        return 1;
    }
    auto v3d = system()->getV3DIfAvailable();
    if(!v3d)
    {
        std::cout << "V3D is not accessible, this program needs to be run as root" << std::endl;
        return 1;
    }

    printProgramStatus(*v3d);
    if(statusOnly)
        return 0;

    if(!v3d->resetByPowerCycle())
    {
        std::cout << "Failed to reset V3D" << std::endl;
        return 1;
    }
    std::cout << "V3D was reset" << std::endl;
    printProgramStatus(*v3d);
    return 0;
}
