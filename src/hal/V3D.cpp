/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "V3D.h"

#include "hal.h"
#include "userland.h"

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sys/mman.h>
#include <system_error>
#include <thread>
#include <unistd.h>

using namespace vc4cl;

static const uint32_t V3D_BASE_OFFSET = 0x00c00000;

// The offsets are byte-offsets, but we need to convert them to 32-bit word offsets
static constexpr uint32_t V3D_IDENT0 = 0x0000 / sizeof(uint32_t);
static constexpr uint32_t V3D_IDENT1 = 0x0004 / sizeof(uint32_t);
static constexpr uint32_t V3D_L2CACTL = 0x00020 / sizeof(uint32_t);
static constexpr uint32_t V3D_SLCACTL = 0x00024 / sizeof(uint32_t);
// constexpr uint32_t V3D_IDENT2 = 0x0008 / sizeof(uint32_t);
static constexpr uint32_t V3D_QPU_RESERVATIONS0 = 0x0410 / sizeof(uint32_t);
// constexpr uint32_t V3D_QPU_RESERVATIONS1 = 0x0414 / sizeof(uint32_t);
static constexpr uint32_t V3D_SRQPC = 0x00430 / sizeof(uint32_t);
static constexpr uint32_t V3D_SRQUA = 0x00434 / sizeof(uint32_t);
static constexpr uint32_t V3D_SRQCS = 0x0043c / sizeof(uint32_t);
static constexpr uint32_t V3D_VPMBASE = 0x00504 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_CLEAR = 0x0670 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_ENABLE = 0x0674 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_VALUE_BASE = 0x0680 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_MAPPING_BASE = 0x0684 / sizeof(uint32_t);
static constexpr uint32_t V3D_ERRORS = 0x0F20 / sizeof(uint32_t);
// offset between two counter-enable, two counter-value or two counter-mapping registers
static constexpr uint32_t V3D_COUNTER_INCREMENT = 0x0008 / sizeof(uint32_t);
static constexpr uint32_t V3D_LENGTH = ((V3D_ERRORS - V3D_IDENT0) + 16) * sizeof(uint32_t);

// The lower 24 bits of V3D_IDENT0 contain the characters "V3D"
static constexpr uint32_t V3D_IDENT0_MAGIC = 0x443356;
// The VPM memory to reserve for user programs, in multiples of 256 bytes. 16 * 256 = 4 KB is the most VC4C uses (see
// VPM_DEFAULT_SIZE).
static constexpr uint32_t VPM_USER_RESERVATION = 16;

static constexpr std::chrono::milliseconds POWER_ON_TIMEOUT{1000};
// Needs to be longer than the runtime PM autosuspend delay (40ms for vc4)
static constexpr std::chrono::milliseconds POWER_OFF_TIMEOUT{2000};

/*
 * If V3D is managed by the Linux vc4 DRM driver (the KMS graphics stack), returns the runtime PM sysfs directory of the
 * V3D device. Returns an empty string if V3D is managed by the VideoCore firmware.
 */
static std::string findV3DPowerDirectory()
{
    static const std::string driverDirectory = "/sys/bus/platform/drivers/vc4_v3d/";
    std::unique_ptr<DIR, decltype(&closedir)> dir{opendir(driverDirectory.data()), closedir};
    if(!dir)
        return "";
    while(auto entry = readdir(dir.get()))
    {
        // the bound device is named after its address, e.g. "3fc00000.v3d"
        std::string name = entry->d_name;
        if(name.find(".v3d") != std::string::npos)
            return driverDirectory + name + "/power/";
    }
    return "";
}

static std::string readSysfs(const std::string& path)
{
    std::ifstream file{path};
    std::string value;
    std::getline(file, value);
    return value;
}

static bool writeSysfs(const std::string& path, const std::string& value)
{
    std::ofstream file{path};
    file << value;
    file.flush();
    return file.good();
}

static bool waitForRuntimeStatus(
    const std::string& pmDirectory, const std::string& status, std::chrono::milliseconds timeout)
{
    const auto start = std::chrono::steady_clock::now();
    while(readSysfs(pmDirectory + "runtime_status") != status)
    {
        if(std::chrono::steady_clock::now() - start > timeout)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

static inline void memoryBarrier()
{
#if defined(__aarch64__) || (defined(__ARM_ARCH) && __ARM_ARCH >= 7)
    // makes sure all previous memory accesses (e.g. writing UNIFORMs and code) are complete before the QPUs start
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

V3D::V3D() : pmDirectory(findV3DPowerDirectory())
{
    bcm_host_init();
    v3dBasePointer = static_cast<volatile uint32_t*>(
        mapmem(busAddressToPhysicalAddress(bcm_host_get_peripheral_address() + V3D_BASE_OFFSET), V3D_LENGTH));
    DEBUG_LOG(DebugLevel::SYSCALL, std::cout << "[VC4CL] V3D base: " << const_cast<uint32_t*>(v3dBasePointer) << std::endl)
    if(!pmDirectory.empty())
    {
        DEBUG_LOG(DebugLevel::SYSTEM_ACCESS,
            std::cout << "[VC4CL] V3D is managed by the vc4 DRM driver, runtime PM: " << pmDirectory << std::endl)
        // The vc4 DRM driver powers off V3D when it is not used by any DRM client, so we need to keep it powered on
        if(!ensurePoweredOn())
            std::cout << "[VC4CL] Failed to power on V3D, kernel execution via register poking will fail!"
                      << std::endl;
    }
}

V3D::~V3D()
{
    if(!originalPmControl.empty())
    {
        // Allow the vc4 DRM driver to power off V3D again
        if(!writeSysfs(pmDirectory + "control", originalPmControl))
            std::cout << "[VC4CL] Failed to restore V3D runtime PM control: " << originalPmControl << std::endl;
    }
    unmapmem(const_cast<uint32_t*>(v3dBasePointer), V3D_LENGTH);
    bcm_host_deinit();
}

bool V3D::ensurePoweredOn()
{
    auto control = readSysfs(pmDirectory + "control");
    if(control != "on")
    {
        if(!writeSysfs(pmDirectory + "control", "on"))
        {
            std::cout << "[VC4CL] Failed to disable V3D runtime PM via " << pmDirectory << "control (need root)"
                      << std::endl;
            return false;
        }
        if(originalPmControl.empty())
            originalPmControl = control;
        DEBUG_LOG(DebugLevel::SYSTEM_ACCESS,
            std::cout << "[VC4CL] Disabled V3D runtime PM (was: " << control << ")" << std::endl)
    }
    return waitForRuntimeStatus(pmDirectory, "active", POWER_ON_TIMEOUT);
}

bool V3D::isPoweredOn() const
{
    // a powered off V3D returns 0xdeadbeef for all registers
    if((v3dBasePointer[V3D_IDENT0] & 0xFFFFFF) != V3D_IDENT0_MAGIC)
        return false;
    return pmDirectory.empty() || readSysfs(pmDirectory + "runtime_status") == "active";
}

void V3D::recoverFromTimeout()
{
    // The QPUs might still be running and might write to any memory (there is no MMU), so until we know they are
    // stopped, do not execute anything else and do not free any memory they could access.
    hung = true;
    std::cout << "[VC4CL] Kernel execution timed out, the QPUs might still be running!" << std::endl;
    if(pmDirectory.empty())
    {
        std::cout << "[VC4CL] Cannot reset V3D, no more kernels will be executed and GPU memory will not be freed"
                  << std::endl;
        return;
    }

    // Reset V3D the same way the vc4 DRM driver does (see vc4_reset()): Let it runtime suspend, which powers it off,
    // then power it on again. Runtime suspend only happens if no DRM client is using V3D at the moment.
    bool suspended = writeSysfs(pmDirectory + "control", "auto") &&
        waitForRuntimeStatus(pmDirectory, "suspended", POWER_OFF_TIMEOUT);
    // the temporary "auto" is not the value to restore on exit
    auto savedPmControl = originalPmControl;
    bool resumed = ensurePoweredOn();
    originalPmControl = savedPmControl;
    if(suspended && resumed)
    {
        hung = false;
        std::cout << "[VC4CL] V3D was reset by power-cycling it" << std::endl;
    }
    else
        std::cout << "[VC4CL] Failed to reset V3D (is another program using the GPU?), no more kernels will be "
                     "executed and GPU memory will not be freed"
                  << std::endl;
}

uint32_t V3D::getSystemInfo(const SystemInfo key) const
{
    switch(key)
    {
    case SystemInfo::VPM_MEMORY_SIZE:
        if((v3dBasePointer[V3D_IDENT1] >> 28) == 0)
            // 0 => 16K
            return 16 * 1024;
        return (v3dBasePointer[V3D_IDENT1] >> 28) * 1024;
    case SystemInfo::VPM_USER_MEMORY_SIZE:
        //"Contains the amount of VPM memory reserved for all user programs, in multiples of 256 bytes (4x 16-way 32-bit
        // vectors)."
        return (v3dBasePointer[V3D_VPMBASE] & 0x1F) * 256;
    case SystemInfo::SEMAPHORES_COUNT:
        // FIXME (similar to error with QPU count), this returns 0 but should return 16
        // return (v3dBasePointer[V3D_IDENT1) >> 16) & 0xFF;
        return 16;
    case SystemInfo::SLICE_TMU_COUNT:
        return (v3dBasePointer[V3D_IDENT1] >> 12) & 0xF;
    case SystemInfo::SLICE_QPU_COUNT:
        return (v3dBasePointer[V3D_IDENT1] >> 8) & 0xF;
    case SystemInfo::SLICES_COUNT:
        return (v3dBasePointer[V3D_IDENT1] >> 4) & 0xF;
    case SystemInfo::QPU_COUNT:
        return 12;
        // FIXME somehow from time to time (e.g. every first call after reboot??) this returns 196, but why??
        // return ((*(uint32_t*)(mmap_ptr + V3D_IDENT1) >> 4) & 0xF) * ((*(uint32_t*)(mmap_ptr + V3D_IDENT1) >> 8) &
        // 0xF);
    case SystemInfo::HDR_SUPPORT:
        return (v3dBasePointer[V3D_IDENT1] >> 24) & 0x1;
    case SystemInfo::V3D_REVISION:
        return v3dBasePointer[V3D_IDENT1] & 0xF;
    case SystemInfo::USER_PROGRAMS_COMPLETED_COUNT:
        return (v3dBasePointer[V3D_SRQCS] >> 16) & 0xFF;
    case SystemInfo::USER_REQUESTS_COUNT:
        return (v3dBasePointer[V3D_SRQCS] >> 8) & 0xFF;
    case SystemInfo::PROGRAM_QUEUE_FULL:
        return (v3dBasePointer[V3D_SRQCS] >> 7) & 0x1;
    case SystemInfo::PROGRAM_QUEUE_LENGTH:
        return v3dBasePointer[V3D_SRQCS] & 0x3F;
    }
    return 0;
}

bool V3D::setCounter(uint8_t counterIndex, const CounterType type)
{
    if(static_cast<uint8_t>(counterIndex) > 15)
        return false;
    if(static_cast<uint8_t>(counterIndex) > 29)
        return false;

    // 1. enable counter
    // http://maazl.de/project/vc4asm/doc/VideoCoreIV-addendum.html states (section 10):
    //"You need to set bit 31 (allegedly reserved) to enable performance counters at all."
    v3dBasePointer[V3D_COUNTER_ENABLE] |= (1u << 31) | (1u << counterIndex);
    // 2. set mapping
    v3dBasePointer[V3D_COUNTER_MAPPING_BASE + counterIndex * V3D_COUNTER_INCREMENT] =
        static_cast<uint32_t>(type) & 0x1F;
    // 3. reset counter
    // TODO difference between reset here and reset in resetCounterValue??
    v3dBasePointer[V3D_COUNTER_VALUE_BASE + counterIndex * V3D_COUNTER_INCREMENT] = 0;

    return true;
}

void V3D::resetCounterValue(uint8_t counterIndex)
{
    v3dBasePointer[V3D_COUNTER_CLEAR] = (1 << counterIndex);
}

// https://github.com/jonasarrow/vc4top: "Detected by observation, if the GPU is gated, deadbeef is returned"
// Confirmed this detection
static const uint32_t POWEROFF_VALUE = 0xdeadbeef;

int64_t V3D::getCounter(uint8_t counterIndex) const
{
    uint32_t val = static_cast<uint32_t>(v3dBasePointer[V3D_COUNTER_VALUE_BASE + counterIndex * V3D_COUNTER_INCREMENT]);
    if(val == POWEROFF_VALUE)
        return static_cast<int64_t>(-1);
    return static_cast<int64_t>(val);
}

void V3D::disableCounter(uint8_t counterIndex)
{
    resetCounterValue(counterIndex);
    // TODO correct?? the or?
    v3dBasePointer[V3D_COUNTER_ENABLE] |= 0xFFFF ^ (1 << counterIndex);
}

bool V3D::setReservation(const uint8_t qpu, const QPUReservation val)
{
    if(qpu > 16)
        return false;
    // 8 reservation settings are in one 32-bit register (4 bit per setting)
    uint32_t registerOffset = qpu / 8;
    uint32_t bitOffset = (qpu % 8) * 4;
    uint32_t writeVal = (static_cast<uint8_t>(val) & 0xFu) << bitOffset;
    // clear old values
    v3dBasePointer[V3D_QPU_RESERVATIONS0 + registerOffset] &= ~(0xFu << bitOffset);
    // set new values
    v3dBasePointer[V3D_QPU_RESERVATIONS0 + registerOffset] |= writeVal;

    return true;
}

QPUReservation V3D::getReservation(const uint8_t qpu) const
{
    uint32_t registerOffset = qpu / 8;
    uint32_t bitOffset = (qpu % 8) * 4;
    uint32_t rawValue = v3dBasePointer[V3D_QPU_RESERVATIONS0 + registerOffset] >> bitOffset;
    return static_cast<QPUReservation>(rawValue & 0xF);
}

bool V3D::hasError(const ErrorType type) const
{
    // read bit
    uint32_t val = v3dBasePointer[V3D_ERRORS] >> static_cast<uint8_t>(type);
    // reset bit
    v3dBasePointer[V3D_ERRORS] = 1 << static_cast<uint8_t>(type);
    return val == 1;
}

ExecutionHandle V3D::executeQPU(
    unsigned numQPUs, std::pair<uint32_t*, unsigned> addressPairs, bool flushBuffer, std::chrono::milliseconds timeout)
{
    // see
    // https://github.com/raspberrypi/userland/blob/master/host_applications/linux/apps/hello_pi/hello_fft/gpu_fft_base.c,
    // function gpu_fft_base_exec_direct
    // TODO interrupts?? not in Broadcom spec
    // https://vc4-notes.tumblr.com/post/125039428234/v3d-registers-not-on-videocore-iv-3d-architecture
    // see errata: https://elinux.org/VideoCore_IV_3D_Architecture_Reference_Guide_errata

    if(hung)
    {
        std::cout << "[VC4CL] Not executing kernel, since a previous kernel execution did not finish!" << std::endl;
        return ExecutionHandle{false};
    }
    if((!pmDirectory.empty() && !ensurePoweredOn()) || !isPoweredOn())
    {
        std::cout << "[VC4CL] Not executing kernel, since V3D is powered off!" << std::endl;
        return ExecutionHandle{false};
    }
    // The vc4 DRM driver sets the VPM memory reserved for user programs to 0 every time V3D is powered on, since it
    // does not support user programs.
    if((v3dBasePointer[V3D_VPMBASE] & 0x1F) < VPM_USER_RESERVATION)
        v3dBasePointer[V3D_VPMBASE] = VPM_USER_RESERVATION;

    // clear cache (if set)
    // FIXME when the buffer-flush is disabled (for any consecutive execution), the updated UNIFORM-values are not used,
    // but the old ones!
    //-> which results in incorrect executions (except the first one)
    // XXX can this be re-enabled, if we allocate the host-pointer (Mailbox.cpp) with direct access-mode?
    // if(flushBuffer)
    {
        // clear L2 cache
        v3dBasePointer[V3D_L2CACTL] = 1 << 2;
        // clear uniforms and instructions caches (TMU too)
        v3dBasePointer[V3D_SLCACTL] = 0xFFFFFFFF;
    }

    // reset user program states
    v3dBasePointer[V3D_SRQCS] = (1 << 7) | (1 << 8) | (1 << 16);

    memoryBarrier();

    // write uniforms and instructions addresses for all QPUs
    uint32_t* addressBase = addressPairs.first;
    for(unsigned i = 0; i < numQPUs; ++i)
    {
        v3dBasePointer[V3D_SRQUA] = addressBase[0];
        v3dBasePointer[V3D_SRQPC] = addressBase[1];
        addressBase += 2;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    auto checkFunc = [this, numQPUs, start, timeout]() -> bool {
        // wait for completion
        while(true)
        {
            if(((v3dBasePointer[V3D_SRQCS] >> 16) & 0xFF) == numQPUs)
                return true;
            if(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::high_resolution_clock::now() - start) > timeout)
                break;
            // TODO sleep some time?? so CPU is not fully used for waiting
            // e.g. sleep for the theoretical execution time of the kernel (e.g. #instructions / QPU clock) and then
            // begin active waiting
        }
        recoverFromTimeout();
        return false;
    };
    return ExecutionHandle{checkFunc};
}

bool V3D::readValue(SystemQuery query, uint32_t& output) noexcept
{
    switch(query)
    {
    case SystemQuery::CURRENT_QPU_CLOCK_RATE_IN_HZ:
    case SystemQuery::MAXIMUM_QPU_CLOCK_RATE_IN_HZ:
    case SystemQuery::CURRENT_ARM_CLOCK_RATE_IN_HZ:
    case SystemQuery::MAXIMUM_ARM_CLOCK_RATE_IN_HZ:
    case SystemQuery::QPU_TEMPERATURE_IN_MILLI_DEGREES:
    case SystemQuery::TOTAL_ARM_MEMORY_IN_BYTES:
    case SystemQuery::TOTAL_GPU_MEMORY_IN_BYTES:
        return false;
    case SystemQuery::NUM_QPUS:
        output = getSystemInfo(SystemInfo::QPU_COUNT);
        return true;
    case SystemQuery::TOTAL_VPM_MEMORY_IN_BYTES:
        output = getSystemInfo(SystemInfo::VPM_MEMORY_SIZE);
        return true;
    }
    return false;
}

uint32_t V3D::busAddressToPhysicalAddress(uint32_t busAddress)
{
    return busAddress & ~0xC0000000;
}

void* vc4cl::mapmem(unsigned base, unsigned size)
{
    // TODO this is called once to initialize V3D and every time a buffer is mapped.
    // Can't we cache the memory fd?
    int mem_fd;
    unsigned offset = base % V3D::MEMORY_PAGE_SIZE;
    base = base - offset;
    /*
    size = size + offset;
    */
    /* open /dev/mem */
    if((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
    {
        std::cout << "[VC4CL] can't open /dev/mem" << std::endl;
        std::cout << "[VC4CL] This program should be run as root. Try prefixing command with: sudo" << std::endl;
        throw std::system_error(errno, std::system_category(), "Failed to open /dev/mem");
    }
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED /*|MAP_FIXED*/, mem_fd, base);
    DEBUG_LOG(DebugLevel::SYSCALL, printf("[VC4CL] base=0x%x, mem=%p\n", base, mem))
    if(mem == MAP_FAILED)
    {
        std::cout << "[VC4CL] mmap error " << mem << std::endl;
        perror("[VC4CL] Error in mapmem");
        close(mem_fd);
        throw std::system_error(errno, std::system_category(), "Error in mapmem");
    }
    close(mem_fd);
    return reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(mem) + offset);
}

void vc4cl::unmapmem(void* addr, unsigned size)
{
    /*
    const intptr_t offset = (intptr_t)addr % V3D::MEMORY_PAGE_SIZE;
    addr = (char *)addr - offset;
    size = size + offset;
    */
    int s = munmap(addr, size);
    if(s != 0)
    {
        std::cout << "munmap error " << s << std::endl;
        perror("Error in unmapmem");
        throw std::system_error(errno, std::system_category(), "Error in unmapmem");
    }
}
