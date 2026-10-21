// Test-only Linux seccomp guard installed after dynamic loading, before main.
#include <array>
#include <cstddef>
#include <cstdlib>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

namespace
{
__attribute__((constructor)) void InstallGuard()
{
    // Unknown architectures fail closed; the hosted Linux matrix is x86_64.
#if defined(__x86_64__)
    constexpr unsigned int kArchitecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr unsigned int kArchitecture = AUDIT_ARCH_AARCH64;
#else
    std::_Exit(96);
#endif
    const sock_filter architecture[]{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArchitecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
    constexpr std::array denied{
        SYS_socket,   SYS_connect,   SYS_bind,     SYS_listen, SYS_openat,
        SYS_mkdirat,  SYS_unlinkat,  SYS_renameat, SYS_linkat, SYS_symlinkat,
        SYS_truncate, SYS_ftruncate, SYS_unshare,  SYS_clone,  SYS_execve,
#if defined(__x86_64__)
        SYS_open,     SYS_creat,     SYS_mkdir,    SYS_unlink, SYS_rename,
        SYS_link,     SYS_symlink,
#endif
    };
    std::array<sock_filter, std::size(architecture) + denied.size() * 2U + 1U> filter{};
    std::size_t next{};
    for (const auto instruction : architecture) {
        filter[next++] = instruction;
    }
    for (const auto number : denied) {
        filter[next++] =
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned int>(number), 0, 1);
        filter[next++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
    }
    filter[next] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
        std::_Exit(96);
    }
}
} // namespace
