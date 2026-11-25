#include "host_sampler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dq8::diagnostics {
namespace {

struct Histogram {
    // Keyed by code address; merged by function when printed.
    std::unordered_map<uint64_t, uint32_t> self, inclusive;
    // Caller -> callee pairs, by the low 32 bits of each address, for the
    // callers/callees breakdown of DQ8_SAMPLER_CALLERS.
    std::unordered_map<uint64_t, uint32_t> edges;
    uint64_t samples = 0u;
    void clear() {
        self.clear();
        inclusive.clear();
        edges.clear();
        samples = 0u;
    }
};

struct SampledThread {
    DWORD id = 0u;
    HANDLE handle = nullptr;
    std::string name;
    ULONG64 lastCycles = 0u;
    bool seen = false;
    Histogram cpu, wait;
};

using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
using NtQueryInformationThreadFn = NTSTATUS(NTAPI *)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);

struct ThreadBasicInformation {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    KAFFINITY AffinityMask;
    LONG Priority;
    LONG BasePriority;
};

class Sampler {
public:
    bool start() {
        const char *enabled = std::getenv("DQ8_SAMPLER");
        if (!enabled || !*enabled || std::strcmp(enabled, "0") == 0)
            return false;
        m_reportSeconds = std::max(1.0, envNumber("DQ8_SAMPLER_EVERY", 10.0));
        m_hz = std::clamp(envNumber("DQ8_SAMPLER_HZ", 1000.0), 10.0, 4000.0);
        m_top = static_cast<size_t>(std::clamp(envNumber("DQ8_SAMPLER_TOP", 30.0), 1.0, 500.0));
        // DQ8_SAMPLER_CALLERS=memcpy,RtlAllocateHeap: who calls functions whose
        // name contains one of these, and what they call.
        if (const char *focus = std::getenv("DQ8_SAMPLER_CALLERS"); focus && *focus) {
            std::string list = focus;
            for (size_t start = 0u; start <= list.size();) {
                const size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty())
                    m_focus.push_back(item);
                if (comma == std::string::npos)
                    break;
                start = comma + 1u;
            }
        }
        // DQ8_SAMPLER_ADDRESSES=N: the N hottest instruction addresses of each
        // busy thread, as symbol+offset, for reading a disassembly against.
        m_addresses = static_cast<size_t>(std::clamp(envNumber("DQ8_SAMPLER_ADDRESSES", 0.0), 0.0, 500.0));
        if (const char *path = std::getenv("DQ8_SAMPLER_OUT"); path && *path) {
            m_out = std::fopen(path, "w");
            if (!m_out)
                std::fprintf(stderr, "[sampler] cannot write %s; reporting on stderr\n", path);
        }
        if (!m_out)
            m_out = stderr;
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (kernel32)
            m_getThreadDescription = reinterpret_cast<GetThreadDescriptionFn>(
                reinterpret_cast<void *>(GetProcAddress(kernel32, "GetThreadDescription")));
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            m_queryThread = reinterpret_cast<NtQueryInformationThreadFn>(
                reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryInformationThread")));
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(ntdll);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
                reinterpret_cast<const uint8_t *>(ntdll) + dos->e_lfanew);
            m_ntdllBase = reinterpret_cast<uint64_t>(ntdll);
            m_ntdllEnd = m_ntdllBase + nt->OptionalHeader.SizeOfImage;
        }
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { run(); });
        std::fprintf(m_out, "[sampler] on: %.0f Hz, report every %.0f s, top %zu\n", m_hz, m_reportSeconds, m_top);
        std::fflush(m_out);
        return true;
    }

    void stop() {
        if (!m_running.exchange(false, std::memory_order_acq_rel))
            return;
        if (m_thread.joinable())
            m_thread.join();
        report("final");
        for (auto &thread : m_threads)
            if (thread.handle)
                CloseHandle(thread.handle);
        m_threads.clear();
        if (m_symbolsReady)
            SymCleanup(GetCurrentProcess());
        if (m_out != stderr)
            std::fclose(m_out);
    }

private:
    static double envNumber(const char *name, double fallback) {
        const char *value = std::getenv(name);
        if (!value || !*value)
            return fallback;
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        return end != value ? parsed : fallback;
    }

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const DWORD self = GetCurrentThreadId();
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        const LONG periodMs = std::max(1L, static_cast<LONG>(1000.0 / m_hz));
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(periodMs) * 10000;
            if (!SetWaitableTimer(timer, &due, periodMs, nullptr, nullptr, FALSE)) {
                CloseHandle(timer);
                timer = nullptr;
            }
        }
        using Clock = std::chrono::steady_clock;
        auto lastRefresh = Clock::time_point{};
        auto lastReport = Clock::now();
        while (m_running.load(std::memory_order_acquire)) {
            if (timer)
                WaitForSingleObject(timer, static_cast<DWORD>(periodMs) + 10u);
            else
                Sleep(static_cast<DWORD>(periodMs));
            const auto now = Clock::now();
            if (now - lastRefresh > std::chrono::seconds(1)) {
                refreshThreads(self);
                lastRefresh = now;
            }
            ++m_ticks;
            for (auto &thread : m_threads)
                sample(thread);
            if (now - lastReport >= std::chrono::duration<double>(m_reportSeconds)) {
                report("period");
                lastReport = now;
            }
        }
        if (timer)
            CloseHandle(timer);
    }

    void refreshThreads(DWORD self) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        const DWORD pid = GetCurrentProcessId();
        for (auto &thread : m_threads)
            thread.seen = false;
        THREADENTRY32 entry;
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                continue;
            auto existing = std::find_if(m_threads.begin(), m_threads.end(),
                                         [&](const SampledThread &t) { return t.id == entry.th32ThreadID; });
            if (existing != m_threads.end()) {
                existing->seen = true;
                continue;
            }
            SampledThread thread;
            thread.id = entry.th32ThreadID;
            thread.handle = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (!thread.handle)
                continue;
            thread.seen = true;
            thread.name = describe(thread.handle, thread.id);
            QueryThreadCycleTime(thread.handle, &thread.lastCycles);
            m_threads.push_back(std::move(thread));
        }
        CloseHandle(snapshot);
        // Threads that ended keep their samples until the next report.
        for (auto &thread : m_threads) {
            if (!thread.seen && thread.handle) {
                DWORD code = 0u;
                if (GetExitCodeThread(thread.handle, &code) && code != STILL_ACTIVE) {
                    CloseHandle(thread.handle);
                    thread.handle = nullptr;
                }
            }
            if (thread.name.empty() || thread.name.find("thread ") == 0)
                thread.name = describe(thread.handle, thread.id);
        }
    }

    std::string describe(HANDLE handle, DWORD id) const {
        if (handle && m_getThreadDescription) {
            PWSTR description = nullptr;
            if (SUCCEEDED(m_getThreadDescription(handle, &description)) && description) {
                std::string name;
                for (const wchar_t *p = description; *p; ++p)
                    name.push_back(*p < 128 ? static_cast<char>(*p) : '?');
                LocalFree(description);
                if (!name.empty())
                    return name + " (" + std::to_string(id) + ")";
            }
        }
        return "thread " + std::to_string(id);
    }

    bool stackBounds(HANDLE handle, uint64_t &low, uint64_t &high) const {
        if (!m_queryThread)
            return false;
        ThreadBasicInformation info{};
        if (m_queryThread(handle, static_cast<THREADINFOCLASS>(0), &info, sizeof(info), nullptr) != 0 ||
            !info.TebBaseAddress)
            return false;
        const auto *tib = static_cast<const NT_TIB *>(info.TebBaseAddress);
        low = reinterpret_cast<uint64_t>(tib->StackLimit);
        high = reinterpret_cast<uint64_t>(tib->StackBase);
        return low < high;
    }

    void sample(SampledThread &thread) {
        if (!thread.handle)
            return;
        // Suspending a thread runs a kernel APC on it, which is charged to its
        // cycle count, so a sleeping thread still gains a few tens of
        // thousands of cycles per sample. A thread that ran for most of the
        // interval gains millions.
        ULONG64 cycles = thread.lastCycles;
        QueryThreadCycleTime(thread.handle, &cycles);
        const ULONG64 gained = cycles - thread.lastCycles;
        thread.lastCycles = cycles;

        // Nothing below allocates while the thread is stopped: it may hold the
        // heap lock.
        uint64_t frames[kMaxFrames];
        size_t count = 0u;
        if (SuspendThread(thread.handle) == static_cast<DWORD>(-1))
            return;
        CONTEXT context;
        std::memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(thread.handle, &context)) {
            uint64_t low = 0u, high = 0u;
            const bool bounded = stackBounds(thread.handle, low, high);
            // The fiber the EE thread runs guest code on has its own stack,
            // which SwitchToFiber records in the TEB like any other.
            while (count < kMaxFrames && context.Rip != 0u) {
                frames[count++] = context.Rip;
                if (bounded && (context.Rsp < low || context.Rsp >= high))
                    break;
                DWORD64 imageBase = 0u;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!function) {
                    // A leaf function without unwind data: the return address
                    // is at the stack pointer.
                    if (!bounded || context.Rsp + 8u > high)
                        break;
                    context.Rip = *reinterpret_cast<const uint64_t *>(context.Rsp);
                    context.Rsp += 8u;
                    continue;
                }
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0u;
                const uint64_t before = context.Rsp;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                 &establisher, nullptr);
                if (context.Rsp <= before)
                    break;
            }
        }
        ResumeThread(thread.handle);
        if (count == 0u)
            return;

        // Parked in a system call (the leaf is in ntdll) unless it clearly
        // burned the interval there, as memcpy or a spin would.
        const bool inNtdll = frames[0] >= m_ntdllBase && frames[0] < m_ntdllEnd;
        const bool ran = gained > (inNtdll ? kBusyCyclesInSyscall : kBusyCycles);
        Histogram &histogram = ran ? thread.cpu : thread.wait;
        ++histogram.samples;
        ++histogram.self[frames[0]];
        if (!m_focus.empty())
            for (size_t i = 0u; i + 1u < count; ++i)
                ++histogram.edges[(frames[i + 1u] << 32u) | (frames[i] & 0xFFFFFFFFu)];
        // Each address once per sample, so recursion does not count twice.
        std::sort(frames, frames + count);
        for (size_t i = 0u; i < count; ++i)
            if (i == 0u || frames[i] != frames[i - 1u])
                ++histogram.inclusive[frames[i]];
    }

    struct Function {
        std::string name;
    };

    const std::string &symbolize(uint64_t address) {
        auto cached = m_symbols.find(address);
        if (cached != m_symbols.end())
            return cached->second;
        if (!m_symbolsReady) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
            m_symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }
        std::string name;
        if (m_symbolsReady) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0u;
            // A return address is one past the call; the call site's function
            // is what the frame belongs to.
            if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
                name = symbol->Name;
                // Function-level merge: all addresses of a symbol share its
                // start, so callers aggregate per function.
                const uint64_t base = symbol->Address;
                HMODULE module = nullptr;
                char moduleName[MAX_PATH] = "";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(base)), &module) &&
                    GetModuleFileNameA(module, moduleName, MAX_PATH)) {
                    const char *slash = std::strrchr(moduleName, '\\');
                    name = std::string(slash ? slash + 1 : moduleName) + "!" + name;
                }
                (void)base;
            }
        }
        if (name.empty()) {
            char text[64];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
            name = text;
        }
        return m_symbols.emplace(address, std::move(name)).first->second;
    }

    void printTop(const char *title, const std::unordered_map<uint64_t, uint32_t> &counts, uint64_t total) {
        if (counts.empty() || total == 0u)
            return;
        std::unordered_map<std::string, uint32_t> byFunction;
        for (const auto &[address, count] : counts)
            byFunction[symbolize(address)] += count;
        std::vector<std::pair<std::string, uint32_t>> sorted(byFunction.begin(), byFunction.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto &a, const auto &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        std::fprintf(m_out, "  %s\n", title);
        for (size_t i = 0u; i < sorted.size() && i < m_top; ++i)
            std::fprintf(m_out, "   %5.1f%%  %s\n", 100.0 * sorted[i].second / static_cast<double>(total),
                         sorted[i].first.c_str());
    }

    // symbol+offset for one instruction address.
    std::string describeAddress(uint64_t address) {
        const std::string &name = symbolize(address);
        if (!m_symbolsReady)
            return name;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0u;
        if (!SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
            return name;
        char text[640];
        std::snprintf(text, sizeof(text), "%s+0x%llx", name.c_str(), static_cast<unsigned long long>(displacement));
        return text;
    }

    void printAddresses(const Histogram &histogram) {
        if (m_addresses == 0u || histogram.self.empty())
            return;
        std::vector<std::pair<uint64_t, uint32_t>> sorted(histogram.self.begin(), histogram.self.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::fprintf(m_out, "  hottest addresses:\n");
        for (size_t i = 0u; i < sorted.size() && i < m_addresses; ++i)
            std::fprintf(m_out, "   %5.1f%%  %s\n", 100.0 * sorted[i].second / static_cast<double>(histogram.samples),
                         describeAddress(sorted[i].first).c_str());
    }

    // Low 32 bits of an address back to a full one seen in this period.
    void printCallers(const Histogram &histogram) {
        if (m_focus.empty() || histogram.edges.empty())
            return;
        std::unordered_map<uint32_t, uint64_t> full;
        for (const auto &[address, count] : histogram.inclusive)
            full[static_cast<uint32_t>(address)] = address;
        // Per focus function: callers by function and callees by function.
        for (const std::string &needle : m_focus) {
            std::unordered_map<std::string, uint32_t> callers, callees;
            uint32_t total = 0u;
            for (const auto &[edge, count] : histogram.edges) {
                const auto callerIt = full.find(static_cast<uint32_t>(edge >> 32u));
                const auto calleeIt = full.find(static_cast<uint32_t>(edge));
                if (callerIt == full.end() || calleeIt == full.end())
                    continue;
                const std::string &callerName = symbolize(callerIt->second);
                const std::string &calleeName = symbolize(calleeIt->second);
                if (calleeName.find(needle) != std::string::npos) {
                    callers[callerName] += count;
                    total += count;
                }
                if (callerName.find(needle) != std::string::npos)
                    callees[calleeName] += count;
            }
            if (total == 0u)
                continue;
            auto print = [&](const char *title, std::unordered_map<std::string, uint32_t> &map) {
                std::vector<std::pair<std::string, uint32_t>> sorted(map.begin(), map.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                std::fprintf(m_out, "  %s %s (%u edge samples):\n", title, needle.c_str(), total);
                for (size_t i = 0u; i < sorted.size() && i < 12u; ++i)
                    std::fprintf(m_out, "   %5.1f%%  %s\n", 100.0 * sorted[i].second / static_cast<double>(total),
                                 sorted[i].first.c_str());
            };
            print("callers of", callers);
            print("callees of", callees);
        }
    }

    void report(const char *what) {
        if (m_ticks == 0u)
            return;
        std::fprintf(m_out, "[sampler] %s report: %llu ticks\n", what, static_cast<unsigned long long>(m_ticks));
        for (auto &thread : m_threads) {
            if (thread.cpu.samples == 0u && thread.wait.samples == 0u)
                continue;
            std::fprintf(m_out, "[sampler] %s: running %.1f%% of ticks (%llu samples), waiting %llu samples\n",
                         thread.name.c_str(), 100.0 * thread.cpu.samples / static_cast<double>(m_ticks),
                         static_cast<unsigned long long>(thread.cpu.samples),
                         static_cast<unsigned long long>(thread.wait.samples));
            // Threads that mostly sleep are only interesting for where they wait.
            if (thread.cpu.samples * 50u >= m_ticks) {
                printTop("self (running):", thread.cpu.self, thread.cpu.samples);
                printTop("inclusive (running):", thread.cpu.inclusive, thread.cpu.samples);
                printAddresses(thread.cpu);
                printCallers(thread.cpu);
            }
            if (thread.wait.samples * 50u >= m_ticks && thread.cpu.samples * 2u >= m_ticks)
                printTop("inclusive (waiting):", thread.wait.inclusive, thread.wait.samples);
            thread.cpu.clear();
            thread.wait.clear();
        }
        std::fflush(m_out);
        m_ticks = 0u;
    }

    static constexpr size_t kMaxFrames = 96u;
    // About a tenth of a millisecond on a 3 GHz core; a suspend costs far less.
    static constexpr ULONG64 kBusyCycles = 300000u;
    static constexpr ULONG64 kBusyCyclesInSyscall = 2000000u;
    uint64_t m_ntdllBase = 0u, m_ntdllEnd = 0u;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::vector<SampledThread> m_threads;
    std::unordered_map<uint64_t, std::string> m_symbols;
    GetThreadDescriptionFn m_getThreadDescription = nullptr;
    NtQueryInformationThreadFn m_queryThread = nullptr;
    FILE *m_out = nullptr;
    double m_reportSeconds = 10.0, m_hz = 1000.0;
    size_t m_top = 30u;
    size_t m_addresses = 0u;
    std::vector<std::string> m_focus;
    uint64_t m_ticks = 0u;
    bool m_symbolsReady = false;
};

Sampler &sampler() {
    static Sampler instance;
    return instance;
}

} // namespace

bool startHostSampler() { return sampler().start(); }
void stopHostSampler() { sampler().stop(); }

} // namespace dq8::diagnostics

#else // !_WIN32

namespace dq8::diagnostics {

bool startHostSampler() {
    const char *enabled = std::getenv("DQ8_SAMPLER");
    if (enabled && *enabled && std::strcmp(enabled, "0") != 0)
        std::fprintf(stderr, "[sampler] DQ8_SAMPLER is Windows-only; use perf or Instruments here\n");
    return false;
}

void stopHostSampler() {}

} // namespace dq8::diagnostics

#endif
