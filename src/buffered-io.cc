#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants these headers rather than the C++ version
#include <stdio.h>
#include <string.h>
// NOLINTEND(modernize-deprecated-headers)
#include <cstdio>
#include <nix/util/error.hh>
// NOLINTBEGIN(misc-header-include-cycle)
#include <nix/util/signals.hh>
#include <nix/util/signals-impl.hh>
// NOLINTEND(misc-header-include-cycle)
#include <string>
#include <string_view>

#include "buffered-io.hh"

[[nodiscard]] auto tryWriteLine(int file_descriptor, std::string str) -> int {
    str += "\n";
    std::string_view string_view{str};
    while (!string_view.empty()) {
        nix::checkInterrupt();
        // NOLINTNEXTLINE(misc-include-cleaner)
        const ssize_t res =
            write(file_descriptor, string_view.data(), string_view.size());
        if (res == -1 && errno != EINTR) {
            return -errno;
        }
        if (res > 0) {
            string_view.remove_prefix(res);
        }
    }
    return 0;
}

LineReader::LineReader(int file_descriptor)
    : stream(fdopen(file_descriptor, "r")) {
    if (stream == nullptr) {
        throw nix::SysError("fdopen(%d)", file_descriptor);
    }
}

LineReader::LineReader(LineReader &&other) noexcept
    : stream(other.stream.release()), buffer(other.buffer.release()),
      len(other.len) {
    other.stream = nullptr;
    other.len = 0;
}

[[nodiscard]] auto LineReader::readLine() -> std::string_view {
    char *buf = buffer.release();
    const ssize_t read = getline(&buf, &len, stream.get());
    buffer.reset(buf);

    if (read == -1) {
        return {}; // Return an empty string_view in case of error
    }

    nix::checkInterrupt();

    // Remove trailing newline
    char const *line = buffer.get();
    return {line, static_cast<size_t>(read) - 1};
}
