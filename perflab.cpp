// perflab.cpp -- one small program, several very different workloads.
// Each workload is its own non-inlined function so it shows up by name in perf.
//
// Build:  g++ -O2 -g -fno-omit-frame-pointer perflab.cpp -o perflab
// Run:    ./perflab <mode>      (run with no arguments to list the modes)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <numeric>
#include <random>
#include <unistd.h>
#include <vector>

// noinline keeps each workload a separate function; extern "C" keeps its name
// short in perf output (sum_big_values, not sum_big_values(std::vector<...>)).
#define NOINLINE extern "C" __attribute__((noinline))

// Stops the compiler from optimising a value (and the work behind it) away.
template <class T>
inline void keep(T& value) { asm volatile("" : "+r"(value)); }

// ---------------------------------------------------------------- 1. hot
// Pure arithmetic on registers. Nothing to miss, nothing to mispredict.
NOINLINE uint64_t hot_loop(uint64_t n) {
    uint64_t x = 1;
    for (uint64_t i = 0; i < n; ++i) {
        x = x * 6364136223846793005ULL + i;
        x ^= x >> 29;
    }
    return x;
}

// ------------------------------------------------------------- 2. branch
// The same loop over the same values. Only the ORDER of the data differs.
NOINLINE uint64_t sum_big_values(const std::vector<int>& v, int rounds) {
    uint64_t sum = 0;
    for (int r = 0; r < rounds; ++r) {
        for (int x : v) {
            if (x >= 128) {      // taken ~50% of the time
                sum += x;
                keep(sum);       // forces a real branch (no cmov, no SIMD)
            }
        }
    }
    return sum;
}

uint64_t run_branch(bool sorted) {
    std::vector<int> v(1 << 16);
    std::mt19937 rng(42);
    for (int& x : v) x = rng() % 256;
    if (sorted) std::sort(v.begin(), v.end());
    return sum_big_values(v, 6000);
}

// -------------------------------------------------------------- 3. cache
// The same loop reading the same 256 MB array. Only the visiting order differs.
NOINLINE uint64_t sum_by_index(const std::vector<uint64_t>& data,
                               const std::vector<uint32_t>& order) {
    uint64_t sum = 0;
    for (uint32_t i : order) sum += data[i];
    return sum;
}

// Setup for cache_rand: a simple Fisher-Yates shuffle (not the thing under test).
NOINLINE void shuffle_order(std::vector<uint32_t>& order) {
    uint64_t state = 42;
    for (size_t i = order.size() - 1; i > 0; --i) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;   // xorshift
        std::swap(order[i], order[state % (i + 1)]);
    }
}

uint64_t run_cache(bool random_order) {
    const size_t n = 1 << 25;                    // 32M * 8 bytes = 256 MB
    std::vector<uint64_t> data(n, 1);
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    if (random_order) shuffle_order(order);
    return sum_by_index(data, order);
}

// -------------------------------------------------------------- 4. alloc
// Lots of small heap allocations: time goes to malloc/free, not to "our" code.
NOINLINE uint64_t alloc_churn(int n) {
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        std::vector<int>* p = new std::vector<int>(16 + (i & 63), i);
        sum += (*p)[0];
        keep(p);
        delete p;
    }
    return sum;
}

// ------------------------------------------------------------ 5. syscall
// Lots of tiny write() calls: time goes to the kernel, not to user code.
NOINLINE uint64_t syscall_storm(int n) {
    int fd = open("/dev/null", O_WRONLY);
    char byte = 'x';
    uint64_t written = 0;
    for (int i = 0; i < n; ++i) written += write(fd, &byte, 1);
    close(fd);
    return written;
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    auto is = [&](const char* m) { return strcmp(mode, m) == 0; };
    bool all = is("all");
    uint64_t result = 0;
    bool ran = false;

    if (all || is("hot"))           { result += hot_loop(1'000'000'000);  ran = true; }
    if (all || is("branch_sorted")) { result += run_branch(true);         ran = true; }
    if (all || is("branch_random")) { result += run_branch(false);        ran = true; }
    if (all || is("cache_seq"))     { result += run_cache(false);         ran = true; }
    if (all || is("cache_rand"))    { result += run_cache(true);          ran = true; }
    if (all || is("alloc"))         { result += alloc_churn(30'000'000);  ran = true; }
    if (all || is("syscall"))       { result += syscall_storm(10'000'000); ran = true; }

    if (!ran) {
        fprintf(stderr,
                "usage: %s <mode>\n"
                "  hot            tight arithmetic loop\n"
                "  branch_sorted  data-dependent branch, predictable data\n"
                "  branch_random  same branch, unpredictable data\n"
                "  cache_seq      256 MB array, read in order\n"
                "  cache_rand     same array, read in random order\n"
                "  alloc          many small new/delete\n"
                "  syscall        many tiny write() calls\n"
                "  all            everything above, one after another\n",
                argv[0]);
        return 1;
    }
    printf("%s: result=%llu\n", mode, (unsigned long long)result);
    return 0;
}
