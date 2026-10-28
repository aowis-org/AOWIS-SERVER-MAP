#include "../src/server/terrain_admission.h"

#include <atomic>
#include <cstdlib>

#define CHECK(condition) do { if (!(condition)) return EXIT_FAILURE; } while (false)
#include <thread>
#include <vector>

int main()
{
    TerrainAdmission admission(3);
    CHECK(admission.tryAcquire());
    CHECK(admission.tryAcquire());
    CHECK(admission.tryAcquire());
    CHECK(!admission.tryAcquire());
    CHECK(admission.count() == 3);
    admission.release();
    CHECK(admission.tryAcquire());
    admission.release();
    admission.release();
    admission.release();
    CHECK(admission.count() == 0);

    TerrainAdmission concurrent(1);
    std::atomic<int> success {0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 24; ++i)
        threads.emplace_back([&]() { if (concurrent.tryAcquire()) success.fetch_add(1); });
    for (std::thread &thread : threads)
        thread.join();
    CHECK(success.load() == 1);
    CHECK(concurrent.count() == 1);
    concurrent.release();
    return 0;
}
