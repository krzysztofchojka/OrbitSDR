#ifndef LOG_H
#define LOG_H
#include <cstdio>
#include <cstdarg>                                                              // for variadic functions

// OrbitSDR port: the stack's own diagnostic printf() lines (one per burst on a busy
// channel) are silent in-process unless built with -DTETRA_KIT_VERBOSE.
#ifdef TETRA_KIT_VERBOSE
#define TK_PRINTF(...) printf(__VA_ARGS__)
#else
#define TK_PRINTF(...) ((void)0)
#endif

namespace Tetra {

    /**
     * @brief Logging level
     *
     * Note that enum members are numbered to ease tests
     *
     */

    enum LogLevel {
        NONE     = 0,
        LOW      = 1,
        MEDIUM   = 2,
        HIGH     = 3,
        VERYHIGH = 4
    };

    /**
     * @brief Screen logger class
     *
     */

    class Log {
    public:
        Log(const LogLevel level);
        ~Log();

        void print(const LogLevel level, const char * fmt, ...);
        LogLevel getLevel();

    private:
        LogLevel m_level;                                                       ///< Minimum log level
    };
};

#endif /* LOG_H */
