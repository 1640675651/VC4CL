/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4CL_PRINTF_H
#define VC4CL_PRINTF_H

#include <cstdint>
#include <string>
#include <vector>

namespace vc4cl
{
    /*
     * Formats the records written by the printf() calls of a kernel into the printf buffer (see
     * kernel_config::PRINTF_BUFFER_SIZE) according to the OpenCL C printf rules.
     *
     * The format strings (and strings printed with %s) are mapped back into the global data of the program.
     */
    std::string formatPrintfBuffer(const void* buffer, const std::vector<uint64_t>& globalData);
} // namespace vc4cl

#endif /* VC4CL_PRINTF_H */
