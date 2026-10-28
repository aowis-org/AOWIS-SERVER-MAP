#include "../src/server/terrain_byte_admission.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) return EXIT_FAILURE; } while (false)

int main()
{
    constexpr std::uint64_t mib = 1024ULL * 1024;
    TerrainByteAdmission budget(64 * mib);
    CHECK(budget.tryAcquire(32 * mib));
    CHECK(budget.tryAcquire(32 * mib));
    CHECK(!budget.tryAcquire(1));
    CHECK(!budget.tryAcquire(65 * mib));
    CHECK(budget.count() == 64 * mib);
    budget.release(32 * mib);
    CHECK(budget.tryAcquire(16 * mib));
    budget.release(32 * mib);
    budget.release(16 * mib);
    CHECK(budget.count() == 0);

    TerrainByteAdmission concurrent(1);
    std::atomic<int> accepted {0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 24; ++index)
        threads.emplace_back([&]() { if (concurrent.tryAcquire(1)) ++accepted; });
    for (std::thread &thread : threads)
        thread.join();
    CHECK(accepted.load() == 1);
    CHECK(concurrent.count() == 1);
    concurrent.release(1);
    CHECK(concurrent.count() == 0);
    return EXIT_SUCCESS;
}
