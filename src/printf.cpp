/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "printf.h"

#include "vc4cl_config.h"

#include <algorithm>
#include <cctype>
#include <cfenv>
#include <cstdio>
#include <cstring>

using namespace vc4cl;

namespace
{
    /*
     * A record, see VC4C's normalization::lowerPrintf():
     * | size of the record in bytes | address of the global data | address of the format string | arguments ... |
     */
    struct Record
    {
        uint32_t globalDataAddress;
        const std::vector<uint64_t>& globalData;
        const uint32_t* arguments;
        std::size_t numArguments;
        std::size_t nextArgument;

        bool hasNextWord() const
        {
            return nextArgument < numArguments;
        }

        uint32_t nextWord()
        {
            return arguments[nextArgument++];
        }

        // Maps the device address of a string into the global data, returns an empty string for strings outside of it
        std::string getString(uint32_t address) const
        {
            const auto* start = reinterpret_cast<const char*>(globalData.data());
            const std::size_t size = globalData.size() * sizeof(uint64_t);
            if(address < globalDataAddress || address - globalDataAddress >= size)
                return "";
            const auto offset = address - globalDataAddress;
            return std::string(start + offset, strnlen(start + offset, size - offset));
        }
    };
} // namespace

template <typename T>
static void appendFormatted(std::string& out, const std::string& spec, T value)
{
    auto length = std::snprintf(nullptr, 0, spec.data(), value);
    if(length <= 0)
        return;
    std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
    std::snprintf(buffer.data(), buffer.size(), spec.data(), value);
    out.append(buffer.data(), static_cast<std::size_t>(length));
}

static float toFloat(uint32_t word)
{
    float f;
    memcpy(&f, &word, sizeof(f));
    return f;
}

/*
 * Formats a conversion specification:
 *
 * %[flags][width][.precision][vector specifier][length modifier]conversion
 *
 * Returns the position after the conversion specification.
 */
static std::size_t formatConversion(std::string& out, const std::string& format, std::size_t pos, Record& record)
{
    const auto start = pos++; // skip '%'
    std::string spec = "%";
    while(pos < format.size() && strchr("-+ #0", format[pos]))
        spec.push_back(format[pos++]);
    while(pos < format.size() && isdigit(format[pos]))
        spec.push_back(format[pos++]);
    if(pos < format.size() && format[pos] == '.')
    {
        spec.push_back(format[pos++]);
        while(pos < format.size() && isdigit(format[pos]))
            spec.push_back(format[pos++]);
    }
    unsigned vectorWidth = 1;
    if(pos < format.size() && format[pos] == 'v')
    {
        ++pos;
        vectorWidth = 0;
        while(pos < format.size() && isdigit(format[pos]))
            vectorWidth = vectorWidth * 10 + static_cast<unsigned>(format[pos++] - '0');
    }
    std::string lengthModifier;
    if(format.compare(pos, 2, "hh") == 0 || format.compare(pos, 2, "hl") == 0)
    {
        lengthModifier = format.substr(pos, 2);
        pos += 2;
    }
    else if(pos < format.size() && (format[pos] == 'h' || format[pos] == 'l'))
        lengthModifier = format.substr(pos++, 1);
    if(pos >= format.size() || vectorWidth == 0)
    {
        // invalid conversion specification, print as is
        out.append(format, start, std::string::npos);
        return format.size();
    }
    const char conversion = format[pos++];
    // 64-bit integers are written as lower and upper word
    const bool isLong = lengthModifier == "l" && strchr("diouxX", conversion);
    if(isLong)
        spec.append("ll");
    spec.push_back(conversion);

    for(unsigned element = 0; element < vectorWidth; ++element)
    {
        if(!record.hasNextWord())
            break;
        if(element > 0)
            out.push_back(',');
        const uint32_t word = record.nextWord();
        const uint64_t longWord =
            isLong && record.hasNextWord() ? (static_cast<uint64_t>(record.nextWord()) << 32) | word : word;
        switch(conversion)
        {
        case 'd':
        case 'i':
            if(isLong)
                appendFormatted(out, spec, static_cast<long long>(longWord));
            else if(lengthModifier == "hh")
                appendFormatted(out, spec, static_cast<int>(static_cast<int8_t>(word)));
            else if(lengthModifier == "h")
                appendFormatted(out, spec, static_cast<int>(static_cast<int16_t>(word)));
            else
                appendFormatted(out, spec, static_cast<int32_t>(word));
            break;
        case 'o':
        case 'u':
        case 'x':
        case 'X':
            if(isLong)
                appendFormatted(out, spec, static_cast<unsigned long long>(longWord));
            else if(lengthModifier == "hh")
                appendFormatted(out, spec, static_cast<unsigned>(static_cast<uint8_t>(word)));
            else if(lengthModifier == "h")
                appendFormatted(out, spec, static_cast<unsigned>(static_cast<uint16_t>(word)));
            else
                appendFormatted(out, spec, word);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            appendFormatted(out, spec, static_cast<double>(toFloat(word)));
            break;
        case 'c':
            appendFormatted(out, spec, static_cast<int>(static_cast<unsigned char>(word)));
            break;
        case 's':
            appendFormatted(out, spec, record.getString(word).data());
            break;
        case 'p':
            appendFormatted(out, spec, reinterpret_cast<void*>(static_cast<uintptr_t>(word)));
            break;
        default:
            // unknown conversion, print as is
            out.append(format, start, pos - start);
            return pos;
        }
    }
    return pos;
}

std::string vc4cl::formatPrintfBuffer(const void* buffer, const std::vector<uint64_t>& globalData)
{
    const auto* words = reinterpret_cast<const uint32_t*>(buffer);
    const std::size_t usedBytes = std::min(words[0], uint32_t{kernel_config::PRINTF_BUFFER_SIZE});
    const std::size_t usedWords = usedBytes / sizeof(uint32_t);

    // the device rounds toward zero, which also applies to the conversion of floating-point values to decimal
    const int hostRounding = fegetround();
    fesetround(FE_TOWARDZERO);

    std::string out;
    std::size_t index = 0;
    while(index + 3 <= usedWords)
    {
        const uint32_t* recordWords = words + 1 + index;
        const std::size_t recordWordCount = recordWords[0] / sizeof(uint32_t);
        if(recordWordCount < 3 || index + recordWordCount > usedWords)
            // corrupt record
            break;
        Record record{recordWords[1], globalData, recordWords + 3, recordWordCount - 3, 0};
        const std::string format = record.getString(recordWords[2]);
        for(std::size_t pos = 0; pos < format.size();)
        {
            if(format[pos] != '%')
                out.push_back(format[pos++]);
            else if(format.compare(pos, 2, "%%") == 0)
            {
                out.push_back('%');
                pos += 2;
            }
            else
                pos = formatConversion(out, format, pos, record);
        }
        index += recordWordCount;
    }
    fesetround(hostRounding);
    return out;
}
