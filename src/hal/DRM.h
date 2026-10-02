/*
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4CL_DRM
#define VC4CL_DRM

#include "../common.h"
#include "../executor.h"
#include "hal.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vc4cl
{
    /*
     * Access to the V3D via the Linux vc4 DRM driver, patched to run QPU user programs as compute jobs (see
     * DRM-DESIGN.md).
     *
     * The driver schedules the kernel executions together with OpenGL jobs and handles V3D power management and resets.
     * Memory is allocated as vc4 buffer objects (from CMA), which the compute jobs need to reference, so this is used
     * for both memory management and kernel execution.
     */
    class DRM
    {
    public:
        /*
         * Returns the DRM backend if a vc4 render node supports compute jobs and this process is allowed to use them,
         * otherwise nullptr.
         */
        static std::unique_ptr<DRM> create();

        DRM(const DRM&) = delete;
        DRM(DRM&&) = delete;
        ~DRM();

        DRM& operator=(const DRM&) = delete;
        DRM& operator=(DRM&&) = delete;

        std::unique_ptr<DeviceBuffer> allocateBuffer(
            const std::shared_ptr<SystemAccess>& system, unsigned sizeInBytes, const std::string& name);
        bool deallocateBuffer(const DeviceBuffer* buffer);

        /*
         * Queues the QPU programs given by the launch messages (UNIFORMs and code address per QPU) as a compute job.
         *
         * All buffers accessed by the programs need to be passed, so the driver keeps them alive while the job runs.
         */
        CHECK_RETURN ExecutionHandle executeQPU(unsigned numQPUs, std::pair<uint32_t*, uint32_t> controlAddress,
            const std::vector<const DeviceBuffer*>& buffers, std::chrono::milliseconds timeout);

        bool readValue(SystemQuery query, uint32_t& output) noexcept;

        const std::string devicePath;

    private:
        DRM(int fd, std::string path);

        int fd;
    };

} /* namespace vc4cl */
#endif /* VC4CL_DRM */
