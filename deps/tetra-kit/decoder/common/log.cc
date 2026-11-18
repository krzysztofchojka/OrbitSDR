#include "log.h"

using namespace Tetra;

/**
 * @brief Constructor
 *
 */

Log::Log(const LogLevel level)
{
    m_level = level;
}

/**
 * @brief Destructor
 *
 */

Log::~Log()
{

}

/**
 * @brief Print informations regarding level required.
 *        If level <= required level, data will be printed
 *        to screen
 *
 */

void Log::print(const LogLevel level, const char * fmt, ...)
{
    if (m_level != LogLevel::NONE && level <= m_level)                         // OrbitSDR: NONE means silent
    {
        va_list args;
        va_start(args, fmt);

        // vfprintf required here since variadic call
        vfprintf(stdout, fmt, args);

        va_end(args);
        fflush(stdout);
    }
}

/**
 * @brief Return current logging level
 *
 */

LogLevel Log::getLevel()
{
    return m_level;
}
