#include "rss.hh"

#include <cstddef>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <cstdio>
#include <string>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#elif defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

namespace {
constexpr size_t MIB = 1024 * 1024;

[[maybe_unused]] auto pagesToMiB(size_t pages) -> size_t {
    static const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return pages * pageSize / MIB;
}
} // namespace

auto residentMemoryMiB(pid_t pid) -> size_t {
#ifdef __linux__
    const std::string path = "/proc/" + std::to_string(pid) + "/statm";
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    FILE *file = std::fopen(path.c_str(), "re");
    if (file == nullptr) {
        return 0;
    }
    unsigned long size = 0;
    unsigned long resident = 0;
    // NOLINTNEXTLINE(cert-err34-c)
    const int n = std::fscanf(file, "%lu %lu", &size, &resident);
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    std::fclose(file);
    return n == 2 ? pagesToMiB(resident) : 0;
#elif defined(__APPLE__)
    struct rusage_info_v4 info = {};
    if (proc_pid_rusage(
            pid, RUSAGE_INFO_V4,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            reinterpret_cast<rusage_info_t *>(&info)) != 0) {
        return 0;
    }
    return info.ri_phys_footprint / MIB;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    struct kinfo_proc info = {};
    size_t len = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &info, &len, nullptr, 0) != 0 || len == 0) {
        return 0;
    }
#ifdef __DragonFly__
    return pagesToMiB(info.kp_vm_rssize);
#else
    return pagesToMiB(info.ki_rssize);
#endif
#elif defined(__NetBSD__) || defined(__OpenBSD__)
#ifdef __NetBSD__
    struct kinfo_proc2 info = {};
    int mib[6] = {CTL_KERN, KERN_PROC2, KERN_PROC_PID, pid, sizeof(info), 1};
#else
    struct kinfo_proc info = {};
    int mib[6] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid, sizeof(info), 1};
#endif
    size_t len = sizeof(info);
    if (sysctl(mib, 6, &info, &len, nullptr, 0) != 0 || len == 0) {
        return 0;
    }
    return pagesToMiB(info.p_vm_rssize);
#else
#error "residentMemoryMiB() is not implemented for this platform"
#endif
}
