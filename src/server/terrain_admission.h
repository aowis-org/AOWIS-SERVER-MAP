#ifndef AOWIS_SERVER_MAP_TERRAIN_ADMISSION_H
#define AOWIS_SERVER_MAP_TERRAIN_ADMISSION_H

#include <atomic>

// Counts both executing and queued terrain requests. Admission is nonblocking.
class TerrainAdmission
{
public:
    explicit TerrainAdmission(int maximum_jobs) : maximum_jobs(maximum_jobs) {}

    bool tryAcquire()
    {
        int current = this->outstanding.load(std::memory_order_relaxed);
        while (current < this->maximum_jobs)
        {
            if (this->outstanding.compare_exchange_weak(current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    void release()
    {
        this->outstanding.fetch_sub(1, std::memory_order_acq_rel);
    }

    int count() const { return this->outstanding.load(std::memory_order_acquire); }

private:
    const int maximum_jobs;
    std::atomic<int> outstanding {0};
};

#endif
