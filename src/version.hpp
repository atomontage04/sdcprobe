#pragma once

// Version identity.
//
// Deliberately not __DATE__: it makes builds unreproducible and tells nobody
// anything useful in a bug report. A version plus the commit it was built from
// answers "which code is this exactly", which is the only question that gets
// asked.

#define SDC_VERSION "1.0.0"

// Filled in by CMake from `git rev-parse --short HEAD` when the source tree is
// a git checkout. Released archives and tarballs build fine without it.
#if !defined(SDC_GIT_HASH)
#define SDC_GIT_HASH "unknown"
#endif
