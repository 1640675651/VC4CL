/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "executor.h"
#include "Buffer.h"
#include "Event.h"
#include "Kernel.h"
#include "PerformanceCounter.h"
#include "hal/hal.h"

#include <CL/opencl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

using namespace vc4cl;

// timeout in ms
// to allow hanging kernels to time-out, set this to a non-infinite, but high enough value, so no valid kernel takes
// that long (e.g. 1min)
static const std::chrono::milliseconds KERNEL_TIMEOUT{1000};

// get_work_dim, get_local_size, get_local_id, get_num_groups (x, y, z), get_global_offset (x, y, z), global-data,
// uniform address, max_group_id (x, y, z)
static const unsigned MAX_HIDDEN_PARAMETERS = 14;

// The maximum number of UNIFORM words for the work-groups of a single launch of a SIMT kernel (see below). Larger
// NDRanges are split into several launches.
static const std::size_t MAX_SIMT_UNIFORM_WORDS = 64 * 1024;

static unsigned AS_GPU_ADDRESS(const unsigned* ptr, DeviceBuffer* buffer)
{
    const char* tmp = *reinterpret_cast<const char**>(&ptr);
    return static_cast<unsigned>(
        static_cast<uint32_t>(buffer->qpuPointer) + ((tmp) - reinterpret_cast<char*>(buffer->hostPointer)));
}

static size_t get_size(
    uint8_t numQPUS, size_t code_size, size_t num_uniforms, size_t global_data_size, size_t stackFrameSizeInWords)
{
    // we duplicate the UNIFORMs to be able to update one block while the second one is used for execution
    size_t uniformSize = 2 * sizeof(unsigned) * num_uniforms;
    // we need 2 32-bit words (code pointer, uniform pointer) per QPU for a single launch message block.
    // we need 2 launch message blocks, one per UNIFORM block (see above)
    size_t launchMessageSize = 2 * 2 * sizeof(uint32_t) * numQPUS;
    size_t rawSize = code_size + uniformSize + global_data_size +
        numQPUS * stackFrameSizeInWords * sizeof(uint64_t) /* word-size */ + launchMessageSize;
    // round up to next multiple of alignment
    return (rawSize / PAGE_ALIGNMENT + 1) * PAGE_ALIGNMENT;
}

static unsigned* set_work_item_info(unsigned* ptr, cl_uint num_dimensions,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& global_offsets,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& global_sizes,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& local_sizes,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& group_indices,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& local_indices, unsigned global_data,
    unsigned uniformAddress, const KernelUniforms& uniformsUsed, uint8_t workItemMergeFactor)
{
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION, {
        std::cout << "Setting work-item infos:" << std::endl;
        std::cout << "\t" << num_dimensions << " dimensions with offsets: " << global_offsets[0] << ", "
                  << global_offsets[1] << ", " << global_offsets[2] << std::endl;
        std::cout << "\tGlobal IDs (sizes): " << group_indices[0] * local_sizes[0] + local_indices[0] << "("
                  << global_sizes[0] << "), " << group_indices[1] * local_sizes[1] + local_indices[1] << "("
                  << global_sizes[1] << "), " << group_indices[2] * local_sizes[2] + local_indices[2] << "("
                  << global_sizes[2] << ")" << std::endl;
        if (workItemMergeFactor > 1)
            std::cout << "\tLocal IDs (sizes): " << (local_indices[0] * workItemMergeFactor) << "-"
                      << std::min((local_indices[0] + 1) * workItemMergeFactor, local_sizes[0]) << "(" << local_sizes[0] << "), "
                      << local_indices[1] << "(" << local_sizes[1] << "), " << local_indices[2] << "(" << local_sizes[2] << ")" << std::endl;
        else
            std::cout << "\tLocal IDs (sizes): " << local_indices[0] << "(" << local_sizes[0] << "), " << local_indices[1]
                      << "(" << local_sizes[1] << "), " << local_indices[2] << "(" << local_sizes[2] << ")" << std::endl;
        std::cout << "\tGroup IDs (sizes): " << group_indices[0] << "(" << (global_sizes[0] / local_sizes[0]) << "), "
                  << group_indices[1] << "(" << (global_sizes[1] / local_sizes[1]) << "), " << group_indices[2] << "("
                  << (global_sizes[2] / local_sizes[2]) << ")" << std::endl;
    })
    // composes UNIFORMS for the values
    if(uniformsUsed.getWorkDimensionsUsed())
        *ptr++ = num_dimensions; /* get_work_dim() */
    // since locals values top at 255, all 3 dimensions can be unified into one 32-bit UNIFORM
    // when read, the values are shifted by 8 * ndim bits and ANDed with 0xFF
    if(uniformsUsed.getLocalSizesUsed())
        *ptr++ = static_cast<unsigned>(
            local_sizes[2] << 16 | local_sizes[1] << 8 | local_sizes[0]); /* get_local_size(dim) */
    if(uniformsUsed.getLocalIDsUsed())
        *ptr++ = static_cast<unsigned>(
            local_indices[2] << 16 | local_indices[1] << 8 | (local_indices[0] * workItemMergeFactor)); /* get_local_id(dim) */
    if(uniformsUsed.getNumGroupsXUsed())
        *ptr++ = static_cast<unsigned>(global_sizes[0] / local_sizes[0]); /* get_num_groups(0) */
    if(uniformsUsed.getNumGroupsYUsed())
        *ptr++ = static_cast<unsigned>(global_sizes[1] / local_sizes[1]); /* get_num_groups(1) */
    if(uniformsUsed.getNumGroupsZUsed())
        *ptr++ = static_cast<unsigned>(global_sizes[2] / local_sizes[2]); /* get_num_groups(2) */
    if(uniformsUsed.getGroupIDXUsed())
        *ptr++ = static_cast<unsigned>(group_indices[0]); /* get_group_id(0) */
    if(uniformsUsed.getGroupIDYUsed())
        *ptr++ = static_cast<unsigned>(group_indices[1]); /* get_group_id(1) */
    if(uniformsUsed.getGroupIDZUsed())
        *ptr++ = static_cast<unsigned>(group_indices[2]); /* get_group_id(2) */
    if(uniformsUsed.getGlobalOffsetXUsed())
        *ptr++ = static_cast<unsigned>(global_offsets[0]); /* get_global_offset(0) */
    if(uniformsUsed.getGlobalOffsetYUsed())
        *ptr++ = static_cast<unsigned>(global_offsets[1]); /* get_global_offset(1) */
    if(uniformsUsed.getGlobalOffsetZUsed())
        *ptr++ = static_cast<unsigned>(global_offsets[2]); /* get_global_offset(2) */
    if(uniformsUsed.getGlobalDataAddressUsed())
        *ptr++ = global_data; // base address for the global-data block
    return ptr;
}

static bool increment_index(std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& indices,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& limits, const size_t offset)
{
    indices[0] += offset;
    if(indices[0] >= limits[0])
    {
        indices[0] -= limits[0];
        ++indices[1];
        if(indices[1] == limits[1])
        {
            indices[1] = 0;
            ++indices[2];
        }
    }
    return indices[2] < limits[2];
}

// Advances the indices by count positions, returns whether the end was not reached
static bool advance_index(std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& indices,
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS>& limits, std::size_t count)
{
    for(std::size_t i = 0; i < count; ++i)
    {
        if(!increment_index(indices, limits, 1))
            return false;
    }
    return true;
}

static void dumpBuffer(std::ostream& os, const DeviceBuffer* buffer)
{
    if(!buffer || !buffer->hostPointer)
    {
        uint32_t numWords = 1;
        os.write(reinterpret_cast<char*>(&numWords), sizeof(unsigned));
        uint32_t address = 0;
        os.write(reinterpret_cast<char*>(&address), sizeof(unsigned));
    }
    else
    {
        // TODO for sub-buffers, the address and contents of the whole buffer is dumped
        uint32_t numWords = 0x80000000 | static_cast<uint32_t>(buffer->size / sizeof(unsigned));
        os.write(reinterpret_cast<char*>(&numWords), sizeof(unsigned));
        os.write(reinterpret_cast<const char*>(buffer->hostPointer), buffer->size);
    }
}

static void dumpMemoryState(std::ostream& os, const Kernel* kernel, const KernelExecution& args,
    DeviceBuffer& mainBuffer, const uint32_t* qpu_code, uint32_t* firstUniformPointer, bool printHead)
{
    // add additional pointers for the dump-analyzer
    // qpu base-pointer (global-data pointer) | qpu code-pointer | qpu UNIFORM-pointer | num uniforms
    // | implicit uniform bit-field
    if(printHead)
    {
        unsigned tmp = static_cast<unsigned>(mainBuffer.qpuPointer);
        os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
        tmp = AS_GPU_ADDRESS(qpu_code, &mainBuffer);
        os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
        tmp = AS_GPU_ADDRESS(firstUniformPointer, &mainBuffer);
        os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
        tmp = static_cast<uint32_t>(
            kernel->info.uniformsUsed.countUniforms() + 1 /* re-run flag */ + kernel->info.getExplicitUniformCount());
        os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
        tmp = static_cast<unsigned>(kernel->info.uniformsUsed.value);
        os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
        // write buffer contents
        os.write(reinterpret_cast<char*>(mainBuffer.hostPointer), static_cast<uint32_t>(mainBuffer.size));
    }
    // append additionally the kernel parameter for this execution
    // append 0-word as border between sections for initial dump and all bits set for end-of-execution dump
    unsigned tmp = printHead ? 0 : 0xFFFFFFFFu;
    os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
    // parameter have this format: pointer-bit| # words | <data (direct or buffer contents)>
    for(unsigned i = 0; i < args.executionArguments.size(); ++i)
    {
        const auto& arg = args.executionArguments[i];
        auto bufferIt = args.persistentBuffers.find(i);
        auto tmpIt = args.tmpBuffers.find(i);
        if(bufferIt != args.persistentBuffers.end())
        {
            dumpBuffer(os, bufferIt->second.first.get());
        }
        else if(tmpIt != args.tmpBuffers.end())
        {
            dumpBuffer(os, tmpIt->second.get());
        }
        else if(auto scalar = dynamic_cast<const ScalarArgument*>(arg.get()))
        {
            tmp = static_cast<uint32_t>(scalar->scalarValues.size());
            os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
            for(const auto elem : scalar->scalarValues)
            {
                tmp = elem.getUnsigned();
                os.write(reinterpret_cast<char*>(&tmp), sizeof(unsigned));
            }
        }
    }
}

static bool flushHostCache(SystemAccess& system, const std::unique_ptr<DeviceBuffer>& kernelBuffer,
    const std::map<unsigned, std::unique_ptr<DeviceBuffer>>& tmpBuffers,
    const std::map<unsigned, std::pair<std::shared_ptr<DeviceBuffer>, DevicePointer>>& persistentBuffers)
{
    std::vector<const DeviceBuffer*> toBeFlushed;
    toBeFlushed.reserve(1 + tmpBuffers.size() + persistentBuffers.size());
    toBeFlushed.emplace_back(kernelBuffer.get());
    for(auto& buf : tmpBuffers)
    {
        if(buf.second && buf.second->hostPointer)
            toBeFlushed.emplace_back(buf.second.get());
    }
    for(auto& buf : persistentBuffers)
        toBeFlushed.emplace_back(buf.second.first.get());

    auto status = system.flushCPUCache(toBeFlushed);
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
        std::cout << "Flushing cache for " << toBeFlushed.size() << " host-accessible device buffers "
                  << (status ? "succeeded" : "failed") << std::endl)
    return status;
}

// All buffers the kernel execution accesses, see SystemAccess#executeQPU()
static std::vector<const DeviceBuffer*> collectBuffers(const std::unique_ptr<DeviceBuffer>& kernelBuffer,
    const std::map<unsigned, std::unique_ptr<DeviceBuffer>>& tmpBuffers,
    const std::map<unsigned, std::pair<std::shared_ptr<DeviceBuffer>, DevicePointer>>& persistentBuffers)
{
    std::vector<const DeviceBuffer*> buffers;
    buffers.reserve(1 + tmpBuffers.size() + persistentBuffers.size());
    buffers.emplace_back(kernelBuffer.get());
    for(auto& buf : tmpBuffers)
    {
        if(buf.second)
            buffers.emplace_back(buf.second.get());
    }
    for(auto& buf : persistentBuffers)
    {
        // NULL pointer arguments have no buffer
        if(buf.second.first)
            buffers.emplace_back(buf.second.first.get());
    }
    return buffers;
}

cl_int executeKernel(KernelExecution& args)
{
    const Kernel* kernel = args.kernel.get();
    CHECK_KERNEL(kernel)

    // first work-group has group_ids 0,0,0
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS> group_limits = {
        args.globalSizes[0] / args.localSizes[0],
        args.globalSizes[1] / args.localSizes[1],
        args.globalSizes[2] / args.localSizes[2],
    };
    const size_t numGroups = group_limits[0] * group_limits[1] * group_limits[2];

    auto mergeFactor = std::max(kernel->info.workItemMergeFactor, uint8_t{1});
    size_t localSize = args.localSizes[0] * args.localSizes[1] * args.localSizes[2];
    // Kernels with independent work-items (always in SIMT mode) run chunks of work-items (16 in SIMT mode, otherwise
    // 1) on any QPU: every QPU gets one block of UNIFORMs per chunk it runs, each followed by a flag whether another
    // block follows. So a single launch runs many chunks, of any work-groups.
    const bool hasIndependentWorkItems = kernel->info.uniformsUsed.getNextGroupFlagUsed();
    // SIMT kernels without that loop run a single work-group (of at most 16 work-items) per QPU and launch. Other
    // kernels run one work-item per QPU, the number of QPUs is the number of work-items in a work-group.
    const bool groupPerQPU = mergeFactor > 1 && !hasIndependentWorkItems;
    if(groupPerQPU && localSize > mergeFactor)
        return CL_INVALID_WORK_GROUP_SIZE;
    // the chunks of a work-group: a SIMT chunk is up to 16 consecutive work-items in x of a row (same y and z)
    const std::array<std::size_t, kernel_config::NUM_DIMENSIONS> chunkLimits = {
        (args.localSizes[0] + mergeFactor - 1) / mergeFactor, args.localSizes[1], args.localSizes[2]};
    const std::size_t chunksPerGroup =
        hasIndependentWorkItems ? chunkLimits[0] * chunkLimits[1] * chunkLimits[2] : 1;
    const std::size_t numChunks = numGroups * chunksPerGroup;
    size_t numQPUs = hasIndependentWorkItems ?
        std::min<size_t>(args.system->getNumQPUs(), numChunks) :
        (groupPerQPU ? std::min<size_t>(args.system->getNumQPUs(), numGroups) :
                       (localSize / mergeFactor) + (localSize % mergeFactor != 0));
    if(numQPUs > args.system->getNumQPUs())
        return CL_INVALID_GLOBAL_WORK_SIZE;

    if(numQPUs == 0)
        // OpenCL 3.0 requires that we allow to enqueue a kernel without any executions for some reason
        return CL_COMPLETE;
    std::array<std::size_t, kernel_config::NUM_DIMENSIONS> group_indices = {0, 0, 0};
    std::array<std::size_t, kernel_config::NUM_DIMENSIONS> local_indices = {0, 0, 0};

    // if the "loop-work-groups" optimization is enabled, all work-items are executed by the first call
    bool isWorkGroupLoopEnabled = kernel->info.uniformsUsed.getMaxGroupIDXUsed() ||
        kernel->info.uniformsUsed.getMaxGroupIDYUsed() || kernel->info.uniformsUsed.getMaxGroupIDZUsed();
    const std::size_t uniformsPerChunk =
        kernel->info.uniformsUsed.countUniforms() + kernel->info.getExplicitUniformCount();
    const std::size_t chunksPerLaunch = hasIndependentWorkItems ?
        std::min(numChunks, std::max(numQPUs, MAX_SIMT_UNIFORM_WORDS / uniformsPerChunk)) :
        1;

    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION, {
        std::cout << "Running kernel '" << kernel->info.name << "' with " << kernel->info.getLength()
                  << " instructions..." << std::endl;
        std::cout << "Local sizes: " << args.localSizes[0] << " " << args.localSizes[1] << " " << args.localSizes[2]
                  << " and merge-factor " << static_cast<unsigned>(mergeFactor) << " -> " << numQPUs << " QPUs"
                  << (hasIndependentWorkItems ? " (" + std::to_string(chunksPerGroup) + " chunks per work-group)" :
                                        (groupPerQPU ? std::string(" (one work-group per QPU)") : std::string()))
                  << std::endl;
        std::cout << "Global sizes: " << args.globalSizes[0] << " " << args.globalSizes[1] << " " << args.globalSizes[2]
                  << " -> " << (args.globalSizes[0] * args.globalSizes[1] * args.globalSizes[2]) / localSize
                  << " work-groups ("
                  << (isWorkGroupLoopEnabled ? "all at once" :
                                               (hasIndependentWorkItems ? "up to " + std::to_string(chunksPerLaunch) +
                                                           " chunks per launch" :
                                                                  std::string("separate")))
                  << ")" << std::endl;
    })

    //
    // ALLOCATE BUFFER
    //
    size_t buffer_size = get_size(args.system->getNumQPUs(), kernel->info.getLength() * sizeof(uint64_t),
        hasIndependentWorkItems ? chunksPerLaunch * uniformsPerChunk :
                          numQPUs * (MAX_HIDDEN_PARAMETERS + kernel->info.getExplicitUniformCount()),
        kernel->program->globalData.size() * sizeof(uint64_t), kernel->program->moduleInfo.getStackFrameSize());

    std::unique_ptr<DeviceBuffer> buffer(
        args.system->allocateBuffer(static_cast<unsigned>(buffer_size), "VC4CL kernel"));
    if(!buffer)
        return CL_OUT_OF_RESOURCES;

    //
    // SET CONTENT
    //
    /*
     * source: https://github.com/hermanhermitage/videocoreiv-qpu/blob/master/qpu-tutorial/qpu-02.c
     *
     * +---------------+
     * |  Data Segment |
     * +---------------+ <----+
     * |  QPU Code     |      |
     * |  ...          |      |
     * +---------------+ <--+ |
     * |  Uniforms     |    | |
     * +---------------+    | |
     * |  QPU0 Uniform -----+ |
     * |  QPU0 Start   -------+
     * +---------------+
     */
    unsigned* p = reinterpret_cast<unsigned*>(buffer->hostPointer);

    // Copy global data into GPU memory
    const unsigned global_data = AS_GPU_ADDRESS(p, buffer.get());
    if(!kernel->program->globalData.empty())
    {
        const void* data_start = kernel->program->globalData.data();
        const unsigned data_length = static_cast<unsigned>(kernel->program->globalData.size() * sizeof(uint64_t));
        memcpy(p, data_start, data_length);
        p += data_length / sizeof(unsigned);
        DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
            std::cout << "Copied " << data_length << " bytes of global data to device buffer" << std::endl)
    }

    // Reserve space for stack-frames and fill it with zeros (e.g. for cl_khr_initialize_memory extension)
    uint32_t maxQPUS = args.system->getNumQPUs();
    uint32_t stackFrameSize = static_cast<uint32_t>(kernel->program->moduleInfo.getStackFrameSize() * sizeof(uint64_t));
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
        std::cout << "Reserving space for " << maxQPUS << " stack-frames of " << stackFrameSize << " bytes each"
                  << std::endl)
    if(kernel->program->context()->initializeMemoryToZero(CL_CONTEXT_MEMORY_INITIALIZE_PRIVATE_KHR))
        memset(p, '\0', maxQPUS * stackFrameSize);
    p += (maxQPUS * stackFrameSize) / sizeof(unsigned);

    // Copy QPU program into GPU memory
    const unsigned* qpu_code = p;
    const void* code_start = &kernel->program->binaryCode[kernel->info.getOffset()];
    memcpy(p, code_start, kernel->info.getLength() * sizeof(uint64_t));
    p += kernel->info.getLength() * sizeof(uint64_t) / sizeof(unsigned);
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
        std::cout << "Copied " << kernel->info.getLength() * sizeof(uint64_t)
                  << " bytes of kernel code to device buffer" << std::endl)

    // Writes the UNIFORMs for the kernel parameters, returns false for unhandled argument types
    auto writeParameters = [&](unsigned*& p) -> bool {
        for(unsigned u = 0; u < kernel->info.parameters.size(); ++u)
        {
            auto tmpBufferIt = args.tmpBuffers.find(u);
            auto persistentBufferIt = args.persistentBuffers.find(u);
            if(tmpBufferIt != args.tmpBuffers.end())
            {
                if(!tmpBufferIt->second)
                {
                    // the __local parameter is lowered into VPM, so there is no temporary buffer
                    *p++ = 0u;
                    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
                        std::cout << "Setting lowered parameter " << (kernel->info.uniformsUsed.countUniforms() + u)
                                  << " to null-pointer" << std::endl)
                }
                else
                {
                    // there exists a temporary buffer for the __local/struct parameter, so set its address as
                    // kernel argument
                    *p++ = static_cast<unsigned>(tmpBufferIt->second->qpuPointer);
                    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
                        std::cout << "Setting parameter " << (kernel->info.uniformsUsed.countUniforms() + u)
                                  << " to temporary buffer " << tmpBufferIt->second->qpuPointer << std::endl)
                }
            }
            else if(persistentBufferIt != args.persistentBuffers.end())
            {
                // the argument is a pointer to a buffer, use its device pointer as kernel argument value.
                // Since the buffer pointer might be NULL, we have to check for this first
                // NOTE: For sub-buffers, the offset is added to the device pointer
                auto devicePtr =
                    persistentBufferIt->second.first.get() ? persistentBufferIt->second.second : DevicePointer{0u};
                *p++ = static_cast<uint32_t>(devicePtr);
                DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
                    std::cout << "Setting parameter " << (kernel->info.uniformsUsed.countUniforms() + u)
                              << " to buffer " << devicePtr << std::endl)
            }
            else if(auto scalarArg = dynamic_cast<const ScalarArgument*>(args.executionArguments.at(u).get()))
            {
                // "default" scalar or vector of scalar kernel argument
                // we use the actual number of vector elements here instead of the expected number, since e.g. for
                // 64-bit integer we need 2 UNIFORMs for every vector element
                for(cl_uchar i = 0; i < scalarArg->scalarValues.size(); ++i)
                    *p++ = scalarArg->scalarValues.at(i).getUnsigned();
                DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
                    std::cout << "Setting parameter " << (kernel->info.uniformsUsed.countUniforms() + u)
                              << " to scalar " << scalarArg->to_string() << std::endl)
            }
            else
            {
                // At this point all argument types should be handled already
                return false;
            }
        }
        return true;
    };

    if(hasIndependentWorkItems)
    {
        // Two blocks of UNIFORMs and launch messages: one is prepared while the other one is executed
        std::array<unsigned*, 2> uniformBlocks{p, p + chunksPerLaunch * uniformsPerChunk};
        p += 2 * chunksPerLaunch * uniformsPerChunk;
        std::array<unsigned*, 2> launchMessages{p, p + 2 * numQPUs};
        p += 4 * numQPUs;

        // Writes the UNIFORMs and launch messages for the given number of chunks (of the work-items of a work-group),
        // starting at the given linear chunk index. QPU q runs the chunks q, q + n, q + 2n, ... (with n QPUs launched).
        // The work-items are independent, so the chunks of a work-group don't need to run at the same time.
        auto prepareLaunch = [&](unsigned block, std::size_t firstChunk, std::size_t numLaunchChunks,
                                 std::size_t& launchQPUs) -> bool {
            launchQPUs = std::min(numQPUs, numLaunchChunks);
            unsigned* u = uniformBlocks[block];
            unsigned* msg = launchMessages[block];
            for(std::size_t q = 0; q < launchQPUs; ++q)
            {
                *msg++ = AS_GPU_ADDRESS(u, buffer.get());
                *msg++ = AS_GPU_ADDRESS(qpu_code, buffer.get());
                for(std::size_t c = q; c < numLaunchChunks; c += launchQPUs)
                {
                    auto chunk = firstChunk + c;
                    auto group = chunk / chunksPerGroup;
                    std::array<std::size_t, kernel_config::NUM_DIMENSIONS> groupIndices = {group % group_limits[0],
                        (group / group_limits[0]) % group_limits[1], group / (group_limits[0] * group_limits[1])};
                    // the local IDs of the chunk's first work-item, the X dimension in multiples of the merge factor
                    auto chunkInGroup = chunk % chunksPerGroup;
                    std::array<std::size_t, kernel_config::NUM_DIMENSIONS> chunkIndices = {
                        chunkInGroup % chunkLimits[0], (chunkInGroup / chunkLimits[0]) % chunkLimits[1],
                        chunkInGroup / (chunkLimits[0] * chunkLimits[1])};
                    u = set_work_item_info(u, args.numDimensions, args.globalOffsets, args.globalSizes,
                        args.localSizes, groupIndices, chunkIndices, global_data, AS_GPU_ADDRESS(u, buffer.get()),
                        kernel->info.uniformsUsed, mergeFactor);
                    if(!writeParameters(u))
                        return false;
                    *u++ = c + launchQPUs < numLaunchChunks ? 1u : 0u;
                }
            }
            return true;
        };

        const std::string dumpFile("/tmp/vc4cl-dump-" + kernel->info.name + "-" + std::to_string(rand()) + ".bin");
        std::ofstream f;
        std::size_t launchChunks = chunksPerLaunch;
        std::size_t launchQPUs = 0;
        unsigned block = 0;
        if(!prepareLaunch(block, 0, launchChunks, launchQPUs))
            return CL_INVALID_KERNEL_ARGS;
        DEBUG_LOG(DebugLevel::KERNEL_EXECUTION, {
            std::cout << "Dumping kernel buffer to " << dumpFile << std::endl;
            f.open(dumpFile, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
            dumpMemoryState(f, kernel, args, *buffer, qpu_code, uniformBlocks[0], true);
        })
        flushHostCache(*args.system, buffer, args.tmpBuffers, args.persistentBuffers);

        auto timeout = KERNEL_TIMEOUT * std::max(std::size_t{30}, chunksPerLaunch);
        const auto accessedBuffers = collectBuffers(buffer, args.tmpBuffers, args.persistentBuffers);
        std::unique_ptr<PerformanceCollector> perfCollector;
        if(args.performanceCounters)
            perfCollector.reset(
                new PerformanceCollector(*args.performanceCounters, args.kernel->info, numQPUs, numGroups));
        DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
            std::cout << "Running chunks 0 to " << (launchChunks - 1) << " on " << launchQPUs << " QPUs"
                      << std::endl)
        auto result = args.system->executeQPU(static_cast<unsigned>(launchQPUs),
            std::make_pair(launchMessages[block], AS_GPU_ADDRESS(launchMessages[block], buffer.get())),
            accessedBuffers, true, timeout);
        for(std::size_t nextChunk = launchChunks; nextChunk < numChunks; nextChunk += launchChunks)
        {
            block ^= 1u;
            launchChunks = std::min(chunksPerLaunch, numChunks - nextChunk);
            // the arguments were already written successfully for the first launch
            prepareLaunch(block, nextChunk, launchChunks, launchQPUs);
            // wait for and check previous (possible asynchronous) execution
            if(!result.waitFor())
                return CL_OUT_OF_RESOURCES;
            flushHostCache(*args.system, buffer, {}, {});
            DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
                std::cout << "Running chunks " << nextChunk << " to " << (nextChunk + launchChunks - 1)
                          << " on " << launchQPUs << " QPUs" << std::endl)
            result = args.system->executeQPU(static_cast<unsigned>(launchQPUs),
                std::make_pair(launchMessages[block], AS_GPU_ADDRESS(launchMessages[block], buffer.get())),
                accessedBuffers, false, timeout);
        }
        auto status = result.waitFor();
        perfCollector.reset();
        DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
            dumpMemoryState(f, kernel, args, *buffer, qpu_code, uniformBlocks[0], false))

        args.tmpBuffers.clear();
        args.persistentBuffers.clear();
        args.executionArguments.clear();
        return status ? CL_COMPLETE : CL_OUT_OF_RESOURCES;
    }

    // 2 times (for each UNIFORM block) 16 times (for each possible QPU)
    std::array<std::array<unsigned*, 16>, 2> uniformPointers;
    // Build Uniforms
    const unsigned* qpu_uniform_0 = p;
    // in SIMT mode, QPU i runs the i-th next work-group
    auto qpu_group_indices = group_indices;
    for(unsigned i = 0; i < numQPUs; ++i)
    {
        uniformPointers[0][i] = p;
        p = set_work_item_info(p, args.numDimensions, args.globalOffsets, args.globalSizes, args.localSizes,
            qpu_group_indices, local_indices, global_data, AS_GPU_ADDRESS(p, buffer.get()), kernel->info.uniformsUsed,
            mergeFactor);
        if(!writeParameters(p))
            return CL_INVALID_KERNEL_ARGS;
        // append UNIFORMs for "loop-work-groups" optimization to end of kernel UNIFORMs
        if(kernel->info.uniformsUsed.getUniformAddressUsed())
            *p++ = AS_GPU_ADDRESS(uniformPointers[0][i], buffer.get());
        if(kernel->info.uniformsUsed.getMaxGroupIDXUsed())
            *p++ = static_cast<unsigned>(group_limits[0]);
        if(kernel->info.uniformsUsed.getMaxGroupIDYUsed())
            *p++ = static_cast<unsigned>(group_limits[1]);
        if(kernel->info.uniformsUsed.getMaxGroupIDZUsed())
            *p++ = static_cast<unsigned>(group_limits[2]);

        if(groupPerQPU)
            increment_index(qpu_group_indices, group_limits, 1);
        else
            increment_index(local_indices, args.localSizes, 1);
    }

    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
        std::cout << (kernel->info.uniformsUsed.countUniforms() + kernel->info.parameters.size()) << " parameters set."
                  << std::endl)

    // We duplicate the UNIFORM buffer, so we can have one being used by the background execution and the other is
    // prepared for the next execution
    unsigned* qpu_uniform_1 = p;
    {
        auto uniformSize = static_cast<size_t>(qpu_uniform_1 - qpu_uniform_0);
        std::memcpy(qpu_uniform_1, qpu_uniform_0, uniformSize * sizeof(uint32_t));
        p += uniformSize;

        // the UNIFORMs of the second block are exactly the size of the first block after the corresponding UNIFORMs
        // of the first block
        for(unsigned i = 0; i < numQPUs; ++i)
            uniformPointers[1][i] = uniformPointers[0][i] + uniformSize;
    }

    /* Build QPU Launch messages */
    auto uniformsPerQPU = kernel->info.uniformsUsed.countUniforms() + kernel->info.getExplicitUniformCount();
    unsigned* qpu_msg_0 = p;
    for(unsigned i = 0; i < numQPUs; ++i)
    {
        *p++ = AS_GPU_ADDRESS(qpu_uniform_0 + i * uniformsPerQPU, buffer.get());
        *p++ = AS_GPU_ADDRESS(qpu_code, buffer.get());
    }
    unsigned* qpu_msg_1 = p;
    for(unsigned i = 0; i < numQPUs; ++i)
    {
        *p++ = AS_GPU_ADDRESS(qpu_uniform_1 + i * uniformsPerQPU, buffer.get());
        *p++ = AS_GPU_ADDRESS(qpu_code, buffer.get());
    }

    const std::string dumpFile("/tmp/vc4cl-dump-" + kernel->info.name + "-" + std::to_string(rand()) + ".bin");
    std::ofstream f;
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION, {
        // Dump all memory content accessed by this kernel execution
        std::cout << "Dumping kernel buffer to " << dumpFile << std::endl;
        f.open(dumpFile, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
        dumpMemoryState(f, kernel, args, *buffer, qpu_code, uniformPointers[0][0], true);
    })

    // flush the host caches for all host-writable buffers
    flushHostCache(*args.system, buffer, args.tmpBuffers, args.persistentBuffers);

    //
    // EXECUTION
    //
    // calculate execution timeout depending on the number of work-groups to be executed at once
    auto timeout = KERNEL_TIMEOUT * std::max(std::size_t{30}, group_limits[0] * group_limits[1] * group_limits[2]);
    const auto accessedBuffers = collectBuffers(buffer, args.tmpBuffers, args.persistentBuffers);

    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
        std::cout << "Running work-group " << group_indices[0] << ", " << group_indices[1] << ", " << group_indices[2]
                  << " with a timeout of " << std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()
                  << " us" << std::endl)
    // toggle between the first and second launch message block to toggle between the first and second UNIFORMs
    // block
    auto qpu_msg_current = qpu_msg_0;
    auto qpu_msg_next = qpu_msg_1;
    auto* uniformPointers_current = &uniformPointers[0];
    auto* uniformPointers_next = &uniformPointers[1];
    // enable performance counters depending on whether they are configured, move to heap to be able to manually control
    // object lifetime
    std::unique_ptr<PerformanceCollector> perfCollector;
    if(args.performanceCounters)
        perfCollector.reset(new PerformanceCollector(*args.performanceCounters, args.kernel->info, numQPUs,
            group_limits[0] * group_limits[1] * group_limits[2]));
    // on first execution, flush code cache
    auto start = std::chrono::high_resolution_clock::now();
    // the number of QPUs (in SIMT mode: of work-groups) run by the current execution
    size_t launchedQPUs = numQPUs;
    size_t remainingGroups = numGroups - (groupPerQPU ? numQPUs : 1);
    auto result = args.system->executeQPU(static_cast<unsigned>(launchedQPUs),
        std::make_pair(qpu_msg_current, AS_GPU_ADDRESS(qpu_msg_current, buffer.get())), accessedBuffers, true,
        timeout);
    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION, {
        // NOTE: This disables background-execution!
        auto success = result.waitFor();
        auto end = std::chrono::high_resolution_clock::now();
        std::cout << "Execution: " << (success ? "successful" : "failed") << " after "
                  << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << " us" << std::endl;
    })

    while(!isWorkGroupLoopEnabled && remainingGroups > 0 &&
        advance_index(group_indices, group_limits, groupPerQPU ? launchedQPUs : 1))
    {
        // switch between current and next launch message and UNIFORM blocks
        std::swap(qpu_msg_current, qpu_msg_next);
        std::swap(uniformPointers_current, uniformPointers_next);
        local_indices[0] = local_indices[1] = local_indices[2] = 0;
        qpu_group_indices = group_indices;
        launchedQPUs = groupPerQPU ? std::min(numQPUs, remainingGroups) : numQPUs;
        remainingGroups -= groupPerQPU ? launchedQPUs : 1;
        // re-set indices and offsets for all QPUs
        for(cl_uint i = 0; i < launchedQPUs; ++i)
        {
            set_work_item_info((*uniformPointers_current)[i], args.numDimensions, args.globalOffsets, args.globalSizes,
                args.localSizes, qpu_group_indices, local_indices, global_data,
                AS_GPU_ADDRESS((*uniformPointers_current)[i], buffer.get()), kernel->info.uniformsUsed, mergeFactor);

            if(groupPerQPU)
                increment_index(qpu_group_indices, group_limits, 1);
            else
                increment_index(local_indices, args.localSizes, 1);
        }
        // wait for and check previous work-group (possible asynchronous) execution
        if(!result.waitFor())
            return CL_OUT_OF_RESOURCES;
        flushHostCache(*args.system, buffer, {}, {});
        DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
            std::cout << "Running work-group " << group_indices[0] << ", " << group_indices[1] << ", "
                      << group_indices[2] << std::endl)
        // all following executions, don't flush cache
        result = args.system->executeQPU(static_cast<unsigned>(launchedQPUs),
            std::make_pair(qpu_msg_current, AS_GPU_ADDRESS(qpu_msg_current, buffer.get())), accessedBuffers, false,
            timeout);
        // NOTE: This disables background-execution!
        DEBUG_LOG(DebugLevel::KERNEL_EXECUTION,
            std::cout << "Execution: " << (result.waitFor() ? "successful" : "failed") << std::endl)
    }

    // wait for (possible asynchronous) execution before freeing the buffers
    auto status = result.waitFor();
    perfCollector.reset();

    DEBUG_LOG(DebugLevel::KERNEL_EXECUTION, {
        // Append the buffers after the kernel execution
        dumpMemoryState(f, kernel, args, *buffer, qpu_code, uniformPointers[0][0], false);
    })

    //
    // CLEANUP
    //

    // even though the buffers are already freed when the KernelExecution event is freed, we clear the maps here,
    // since we do not need the buffers anymore
    args.tmpBuffers.clear();
    args.persistentBuffers.clear();
    args.executionArguments.clear();

    return status ? CL_COMPLETE : CL_OUT_OF_RESOURCES;
}
