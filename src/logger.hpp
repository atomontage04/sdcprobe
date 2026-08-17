#pragma once

// Output to the screen and to a file at the same time.
//
// Why a file: a sweep across every core takes hours and nobody sits watching
// it. By the time something is found the console has scrolled, or been closed.
// A report worth sending to someone else has to exist on disk by itself,
// without anyone having remembered to redirect the output.
//
// Why not plain redirection: the progress line is rewritten with '\r', which
// in a file would collapse into one endless line. So progress goes out in two
// different shapes — rewritten on screen, separate timestamped lines in the
// file.

#include <cstdio>
#include <string>

// Compiler checking of format strings.
//
// The archetype is stated explicitly, and that is not a detail. Under MinGW,
// GCC checks against ms_printf rules by default, while MinGW-w64 itself
// substitutes the ANSI printf implementation when building C++
// (__USE_MINGW_ANSI_STDIO). The result is warnings about perfectly correct %zu
// and %llu, in which real mistakes then drown.
#if defined(__MINGW32__) && defined(__USE_MINGW_ANSI_STDIO) && __USE_MINGW_ANSI_STDIO
#define SDC_PRINTF_FMT(fmt_index, first_arg) \
    __attribute__((format(gnu_printf, fmt_index, first_arg)))
#elif defined(__GNUC__)
#define SDC_PRINTF_FMT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define SDC_PRINTF_FMT(fmt_index, first_arg)
#endif

class Logger {
public:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // false means the file could not be opened. That is not fatal: the screen
    // still works, and the run matters more than the log. The caller must warn.
    bool open(const std::string& path);

    const std::string& path() const { return path_; }
    bool has_file() const { return file_ != nullptr; }

    // Screen and file alike.
    void line(const char* format, ...) SDC_PRINTF_FMT(2, 3);

    // File only. For lines the user has already seen on screen before the run
    // started: the report has to stand on its own without the console, but
    // there is no point printing the same thing twice in a row.
    void file_line(const char* format, ...) SDC_PRINTF_FMT(2, 3);

    // A blank separator line. A method of its own rather than line(""),
    // because a zero-length format string is a legitimate thing to warn about:
    // it almost always means a typo.
    void blank();

    // Progress line: rewritten in place on screen, written to the file as an
    // ordinary timestamped line.
    void progress(const char* format, ...) SDC_PRINTF_FMT(2, 3);

    void close();

private:
    // The on-screen progress line is not terminated by a newline. Anything
    // printed over it has to close it first, or the report gets overwritten by
    // its tail.
    void close_progress_line();

    std::FILE* file_ = nullptr;
    std::string path_;
    bool progress_open_ = false;
};

// Default file name: sdcprobe-YYYYMMDD-HHMMSS.log in the current directory.
// The time is in the name so a second run does not overwrite the first report.
std::string sdc_default_log_path();
