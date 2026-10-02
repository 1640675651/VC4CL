/*
 * See the file "LICENSE" for the full license governing this code.
 */

#include "DRM.h"

#include "Mailbox.h"
// copy of the uAPI header of the patched vc4 driver (vc4-compute), keep in sync
#include "vc4_compute_drm.h"

#include <drm/drm.h>
#include <drm/vc4_drm.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace vc4cl;

// The maximum compute job timeout accepted by the driver (VC4_COMPUTE_MAX_TIMEOUT_MS)
static constexpr std::chrono::milliseconds MAX_JOB_TIMEOUT{10 * 60 * 1000};

// Like libdrm's drmIoctl(): retries interrupted calls. The wait ioctls update their timeout for that.
static int drmIoctl(int fd, unsigned long request, void* arg)
{
    int status;
    do
        status = ioctl(fd, request, arg);
    while(status == -1 && (errno == EINTR || errno == EAGAIN));
    return status;
}

static uint32_t alignToPage(uint32_t size)
{
    return (size + PAGE_ALIGNMENT - 1) / PAGE_ALIGNMENT * PAGE_ALIGNMENT;
}

static void closeBufferObject(int fd, uint32_t handle)
{
    drm_gem_close close{};
    close.handle = handle;
    if(drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close) != 0)
        DEBUG_LOG(DebugLevel::SYSCALL,
            std::cout << "[VC4CL] Failed to close vc4 buffer " << handle << ": " << strerror(errno) << std::endl)
}

static const std::string NOT_PERMITTED = "not permitted";

/*
 * Returns an empty string if the render node is a vc4 device whose driver supports compute jobs which this process may
 * use, the reason why not otherwise.
 */
static std::string checkDevice(int fd)
{
    char name[16] = {0};
    drm_version version{};
    version.name = name;
    version.name_len = sizeof(name) - 1;
    if(drmIoctl(fd, DRM_IOCTL_VERSION, &version) != 0 || std::string{name} != "vc4")
        return "not a vc4 device";

    drm_vc4_get_param param{};
    param.param = DRM_VC4_PARAM_SUPPORTS_QPU_COMPUTE;
    if(drmIoctl(fd, DRM_IOCTL_VC4_GET_PARAM, &param) != 0 || !param.value)
        return "the vc4 driver does not support compute jobs";

    // The compute ioctls need CAP_SYS_RAWIO or membership in the compute_gid group, getting a buffer address checks this
    drm_vc4_create_bo create{};
    create.size = PAGE_ALIGNMENT;
    if(drmIoctl(fd, DRM_IOCTL_VC4_CREATE_BO, &create) != 0)
        return std::string{"failed to create a buffer: "} + strerror(errno);
    drm_vc4_qpu_bo_addr address{};
    address.handle = create.handle;
    int status = drmIoctl(fd, DRM_IOCTL_VC4_QPU_BO_ADDR, &address);
    int error = errno;
    closeBufferObject(fd, create.handle);
    if(status != 0)
        return error == EPERM ? NOT_PERMITTED : std::string{"failed to get a buffer address: "} + strerror(error);
    return "";
}

std::unique_ptr<DRM> DRM::create()
{
    for(unsigned minor = 128; minor < 192; ++minor)
    {
        std::string path = "/dev/dri/renderD" + std::to_string(minor);
        int fd = open(path.data(), O_RDWR | O_CLOEXEC);
        if(fd < 0)
            continue;
        auto reason = checkDevice(fd);
        if(reason.empty())
            return std::unique_ptr<DRM>{new DRM(fd, path)};
        close(fd);

        DEBUG_LOG(DebugLevel::SYSTEM_ACCESS,
            std::cout << "[VC4CL] Not using " << path << " for compute jobs: " << reason << std::endl)
        if(reason == NOT_PERMITTED)
            // Otherwise we silently fall back to direct hardware access, which does not work as non-root on arm64 and
            // conflicts with the driver.
            std::cout << "[VC4CL] The vc4 driver supports compute jobs, but this process may not use them. They need "
                         "root rights or membership in the group set by the vc4 module parameter compute_gid."
                      << std::endl;
    }
    return nullptr;
}

DRM::DRM(int fd, std::string path) : devicePath(std::move(path)), fd(fd)
{
    DEBUG_LOG(DebugLevel::SYSTEM_ACCESS, std::cout << "[VC4CL] Using vc4 DRM device " << devicePath << std::endl)
}

DRM::~DRM()
{
    close(fd);
}

std::unique_ptr<DeviceBuffer> DRM::allocateBuffer(
    const std::shared_ptr<SystemAccess>& system, unsigned sizeInBytes, const std::string& name)
{
    drm_vc4_create_bo create{};
    create.size = alignToPage(sizeInBytes);
    if(drmIoctl(fd, DRM_IOCTL_VC4_CREATE_BO, &create) != 0)
    {
        DEBUG_LOG(DebugLevel::MEMORY,
            std::cout << "[VC4CL] Failed to create vc4 buffer of " << create.size << " bytes: " << strerror(errno)
                      << std::endl)
        return nullptr;
    }

    // shows up in the driver's buffer statistics (/sys/kernel/debug/dri/0/bo_stats)
    if(!name.empty())
    {
        drm_vc4_label_bo label{};
        label.handle = create.handle;
        label.len = static_cast<uint32_t>(name.size());
        label.name = reinterpret_cast<uintptr_t>(name.data());
        drmIoctl(fd, DRM_IOCTL_VC4_LABEL_BO, &label);
    }

    drm_vc4_mmap_bo map{};
    map.handle = create.handle;
    void* hostPointer = MAP_FAILED;
    if(drmIoctl(fd, DRM_IOCTL_VC4_MMAP_BO, &map) == 0)
        hostPointer =
            mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(map.offset));
    drm_vc4_qpu_bo_addr address{};
    address.handle = create.handle;
    if(hostPointer == MAP_FAILED || drmIoctl(fd, DRM_IOCTL_VC4_QPU_BO_ADDR, &address) != 0)
    {
        DEBUG_LOG(DebugLevel::MEMORY,
            std::cout << "[VC4CL] Failed to map vc4 buffer " << create.handle << ": " << strerror(errno) << std::endl)
        if(hostPointer != MAP_FAILED)
            munmap(hostPointer, create.size);
        closeBufferObject(fd, create.handle);
        return nullptr;
    }

    DevicePointer qpuPointer{address.addr};
    DEBUG_LOG(DebugLevel::MEMORY,
        std::cout << "Allocated " << sizeInBytes << " bytes of buffer: handle " << create.handle << ", device address "
                  << std::hex << "0x" << qpuPointer << ", host address " << hostPointer << std::dec << std::endl)
    return std::unique_ptr<DeviceBuffer>{new DeviceBuffer(system, create.handle, qpuPointer, hostPointer, sizeInBytes)};
}

bool DRM::deallocateBuffer(const DeviceBuffer* buffer)
{
    // The driver keeps the memory alive until all compute jobs using it finished
    if(buffer->hostPointer)
        munmap(buffer->hostPointer, alignToPage(buffer->size));
    closeBufferObject(fd, buffer->memHandle);
    DEBUG_LOG(DebugLevel::MEMORY,
        std::cout << "Deallocated " << buffer->size << " bytes of buffer: handle " << buffer->memHandle
                  << ", device address " << std::hex << "0x" << buffer->qpuPointer << std::dec << std::endl)
    return true;
}

ExecutionHandle DRM::executeQPU(unsigned numQPUs, std::pair<uint32_t*, uint32_t> controlAddress,
    const std::vector<const DeviceBuffer*>& buffers, std::chrono::milliseconds timeout)
{
    // The launch messages contain the UNIFORMs and code address for every QPU
    std::vector<drm_vc4_qpu_program> programs(numQPUs);
    for(unsigned i = 0; i < numQPUs; ++i)
    {
        programs[i].uniforms = controlAddress.first[2 * i];
        programs[i].code = controlAddress.first[2 * i + 1];
    }

    // The driver locks every buffer once, so there must not be duplicates, e.g. for a buffer passed as multiple kernel
    // arguments
    std::vector<uint32_t> handles;
    handles.reserve(buffers.size());
    for(const auto* buffer : buffers)
    {
        if(buffer && buffer->memHandle != 0)
            handles.push_back(buffer->memHandle);
    }
    std::sort(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());

    drm_vc4_submit_qpu submit{};
    submit.bo_handles = reinterpret_cast<uintptr_t>(handles.data());
    submit.programs = reinterpret_cast<uintptr_t>(programs.data());
    submit.bo_handle_count = static_cast<uint32_t>(handles.size());
    submit.program_count = numQPUs;
    submit.timeout_ms =
        static_cast<uint32_t>(std::min(std::max(timeout, std::chrono::milliseconds{1}), MAX_JOB_TIMEOUT).count());
    if(drmIoctl(fd, DRM_IOCTL_VC4_SUBMIT_QPU, &submit) != 0)
    {
        std::cout << "[VC4CL] Failed to submit compute job: " << strerror(errno) << std::endl;
        return ExecutionHandle{false};
    }
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
        std::cout << "[VC4CL] Submitted compute job " << submit.seqno << " with " << numQPUs << " QPU programs, "
                  << handles.size() << " buffers and a timeout of " << submit.timeout_ms << " ms" << std::endl)

    int fd = this->fd;
    uint64_t seqno = submit.seqno;
    return ExecutionHandle{[fd, seqno]() -> bool {
        drm_vc4_wait_qpu wait{};
        wait.seqno = seqno;
        wait.timeout_ns = ~0ull;
        if(drmIoctl(fd, DRM_IOCTL_VC4_WAIT_QPU, &wait) != 0)
        {
            std::cout << "[VC4CL] Failed to wait for compute job " << seqno << ": " << strerror(errno) << std::endl;
            return false;
        }
        if(wait.status == -ETIMEDOUT)
            std::cout << "[VC4CL] Kernel execution timed out, the GPU was reset" << std::endl;
        else if(wait.status == -EIO)
            std::cout << "[VC4CL] Kernel execution timed out and the GPU could not be reset, kernel execution is "
                         "disabled until reboot"
                      << std::endl;
        else if(wait.status != 0)
            std::cout << "[VC4CL] Kernel execution failed: " << strerror(-wait.status) << std::endl;
        return wait.status == 0;
    }};
}

static bool readMemInfoBytes(const std::string& key, uint32_t& output)
{
    std::ifstream file{"/proc/meminfo"};
    std::string line;
    while(std::getline(file, line))
    {
        // e.g. "CmaTotal:         262144 kB"
        if(line.compare(0, key.size() + 1, key + ":") != 0)
            continue;
        std::istringstream values{line.substr(key.size() + 1)};
        uint64_t kiloBytes = 0;
        values >> kiloBytes;
        output = static_cast<uint32_t>(std::min<uint64_t>(kiloBytes * 1024, UINT32_MAX));
        return kiloBytes != 0;
    }
    return false;
}

static bool readSysfsNumber(const std::string& path, uint64_t factor, uint32_t& output)
{
    std::ifstream file{path};
    uint64_t value = 0;
    if(!(file >> value))
        return false;
    output = static_cast<uint32_t>(std::min<uint64_t>(value * factor, UINT32_MAX));
    return true;
}

template <MailboxTag Tag>
static bool readFirmwareClock(VC4Clock clock, uint32_t& output)
{
    QueryMessage<Tag> msg({static_cast<unsigned>(clock)});
    if(!firmwarePropertyCall(msg.buffer.data()) || !msg.isSuccessful())
        return false;
    output = msg.getContent(1);
    return true;
}

bool DRM::readValue(SystemQuery query, uint32_t& output) noexcept
{
    try
    {
        drm_vc4_get_param param{};
        param.param = DRM_VC4_PARAM_V3D_IDENT1;
        switch(query)
        {
        case SystemQuery::NUM_QPUS:
            if(drmIoctl(fd, DRM_IOCTL_VC4_GET_PARAM, &param) != 0)
                return false;
            // slices * QPUs per slice
            output = static_cast<uint32_t>(((param.value >> 4) & 0xF) * ((param.value >> 8) & 0xF));
            return output != 0;
        case SystemQuery::TOTAL_VPM_MEMORY_IN_BYTES:
            if(drmIoctl(fd, DRM_IOCTL_VC4_GET_PARAM, &param) != 0)
                return false;
            // in KB, 0 means 16 KB
            output = static_cast<uint32_t>((param.value >> 28) & 0xF);
            output = (output == 0 ? 16 : output) * 1024;
            return true;
        case SystemQuery::TOTAL_GPU_MEMORY_IN_BYTES:
            // all buffers are allocated from CMA
            return readMemInfoBytes("CmaTotal", output);
        case SystemQuery::TOTAL_ARM_MEMORY_IN_BYTES:
            return readMemInfoBytes("MemTotal", output);
        case SystemQuery::CURRENT_QPU_CLOCK_RATE_IN_HZ:
            return readFirmwareClock<MailboxTag::GET_CLOCK_RATE>(VC4Clock::V3D, output);
        case SystemQuery::MAXIMUM_QPU_CLOCK_RATE_IN_HZ:
            return readFirmwareClock<MailboxTag::GET_MAX_CLOCK_RATE>(VC4Clock::V3D, output);
        case SystemQuery::CURRENT_ARM_CLOCK_RATE_IN_HZ:
            return readSysfsNumber("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1000, output);
        case SystemQuery::MAXIMUM_ARM_CLOCK_RATE_IN_HZ:
            return readSysfsNumber("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", 1000, output);
        case SystemQuery::QPU_TEMPERATURE_IN_MILLI_DEGREES:
            return readSysfsNumber("/sys/class/thermal/thermal_zone0/temp", 1, output);
        }
    }
    catch(...)
    {
    }
    return false;
}
