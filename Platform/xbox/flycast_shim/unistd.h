// flycast_shim/unistd.h -- placeholder for POSIX <unistd.h>. Flycast's _WIN32
// code paths don't use it; it exists so transitive includes resolve. Add POSIX
// declarations here only if a compiled TU actually references them.
#pragma once
