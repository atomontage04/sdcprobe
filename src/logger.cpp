// Screen-and-file output. Rationale lives in logger.hpp.

#include "logger.hpp"

#include <cstdarg>
#include <ctime>

namespace {

// Timestamp for the file. The program is single-threaded, so std::localtime and
// its shared buffer are safe here, whereas localtime_r versus localtime_s would
// have to be split across platforms for no gain.
std::string timestamp(const char* format) {
    const std::time_t now = std::time(nullptr);
    const std::tm* parts = std::localtime(&now);
    char text[64];
    if (parts == nullptr || std::strftime(text, sizeof(text), format, parts) == 0) {
        return std::string("unknown-time");
    }
    return std::string(text);
}

} // namespace

Logger::~Logger() {
    close();
}

bool Logger::open(const std::string& path) {
    close();
    file_ = std::fopen(path.c_str(), "w");
    if (file_ == nullptr) {
        return false;
    }
    path_ = path;
    return true;
}

void Logger::close_progress_line() {
    if (progress_open_) {
        std::fputc('\n', stdout);
        progress_open_ = false;
    }
}

void Logger::line(const char* format, ...) {
    close_progress_line();

    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    if (file_ != nullptr) {
        va_start(args, format);
        std::vfprintf(file_, format, args);
        va_end(args);
        std::fputc('\n', file_);
        // Flushing after every line is deliberate: the run is long, and if the
        // machine hangs during it the report has to contain everything up to
        // the last second. Speed is irrelevant here, this is a handful of lines
        // per minute.
        std::fflush(file_);
    }
}

void Logger::file_line(const char* format, ...) {
    if (file_ == nullptr) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(file_, format, args);
    va_end(args);
    std::fputc('\n', file_);
    std::fflush(file_);
}

void Logger::blank() {
    close_progress_line();
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (file_ != nullptr) {
        std::fputc('\n', file_);
        std::fflush(file_);
    }
}

void Logger::progress(const char* format, ...) {
    va_list args;

    // Screen: carriage return plus a tail of spaces to wipe the remainder of
    // the previous, longer line.
    std::fputc('\r', stdout);
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputs("    ", stdout);
    std::fflush(stdout);
    progress_open_ = true;

    if (file_ != nullptr) {
        const std::string stamp = timestamp("%H:%M:%S");
        std::fprintf(file_, "[%s] ", stamp.c_str());
        va_start(args, format);
        std::vfprintf(file_, format, args);
        va_end(args);
        std::fputc('\n', file_);
        std::fflush(file_);
    }
}

void Logger::close() {
    close_progress_line();
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

std::string sdc_default_log_path() {
    return "sdcprobe-" + timestamp("%Y%m%d-%H%M%S") + ".log";
}
