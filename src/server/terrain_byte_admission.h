#ifndef AOWIS_SERVER_MAP_TERRAIN_BYTE_ADMISSION_H
#define AOWIS_SERVER_MAP_TERRAIN_BYTE_ADMISSION_H

#include <atomic>
#include <cstdint>

// Reserves estimated output bytes for unique in-flight terrain grid jobs.
// Admission is nonblocking; duplicate HTTP waiters do not reserve again.
class TerrainByteAdmission
{
public:
    explicit TerrainByteAdmission(std::uint64_t maximum_bytes)
        : maximum_bytes(maximum_bytes) {}

    bool tryAcquire(std::uint64_t bytes)
    {
        if (bytes > this->maximum_bytes)
            return false;
        std::uint64_t current = this->reserved.load(std::memory_order_relaxed);
        while (current <= this->maximum_bytes - bytes)
        {
            if (this->reserved.compare_exchange_weak(current, current + bytes,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    void release(std::uint64_t bytes)
    {
        this->reserved.fetch_sub(bytes, std::memory_order_acq_rel);
    }

    std::uint64_t count() const
    {
        return this->reserved.load(std::memory_order_acquire);
    }

private:
    const std::uint64_t maximum_bytes;
    std::atomic<std::uint64_t> reserved {0};
};

#endif
