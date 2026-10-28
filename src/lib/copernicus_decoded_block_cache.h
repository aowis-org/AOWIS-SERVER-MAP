#pragma once

#include <QByteArray>
#include <QHash>
#include <QQueue>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

namespace Aowis::Map::Internal
{
// Shared accounting covers retained decoded blocks across all open Copernicus
// raster readers. Temporary decode buffers and libtiff internals are not charged.
class CopernicusDecodedBlockBudget
{
public:
    explicit CopernicusDecodedBlockBudget(qsizetype maximum_bytes)
        : maximum_bytes(maximum_bytes > 0 ? maximum_bytes : 0)
    {
    }

    bool reserve(qsizetype bytes)
    {
        QMutexLocker locker(&this->mutex);
        if (bytes <= 0 || bytes > this->maximum_bytes - this->reserved_bytes)
            return false;
        this->reserved_bytes += bytes;
        return true;
    }

    void release(qsizetype bytes)
    {
        QMutexLocker locker(&this->mutex);
        Q_ASSERT(bytes >= 0 && bytes <= this->reserved_bytes);
        this->reserved_bytes -= bytes;
    }

    qsizetype bytes() const
    {
        QMutexLocker locker(&this->mutex);
        return this->reserved_bytes;
    }

private:
    const qsizetype maximum_bytes;
    mutable QMutex mutex;
    qsizetype reserved_bytes = 0;
};

inline CopernicusDecodedBlockBudget &sharedCopernicusDecodedBlockBudget()
{
    static CopernicusDecodedBlockBudget budget(128LL * 1024LL * 1024LL);
    return budget;
}

// Per-open-raster bound. Evict older decoded TIFF blocks instead of retaining
// every block sampled by a large terrain grid for the lifetime of the request.
class CopernicusDecodedBlockCache
{
public:
    explicit CopernicusDecodedBlockCache(qsizetype maximum_bytes,
                                        CopernicusDecodedBlockBudget *shared_budget = nullptr)
        : maximum_bytes(maximum_bytes > 0 ? maximum_bytes : 0),
          shared_budget(shared_budget)
    {
    }

    ~CopernicusDecodedBlockCache()
    {
        if (this->shared_budget != nullptr && this->stored_bytes > 0)
            this->shared_budget->release(this->stored_bytes);
    }

    CopernicusDecodedBlockCache(const CopernicusDecodedBlockCache &) = delete;
    CopernicusDecodedBlockCache &operator=(const CopernicusDecodedBlockCache &) = delete;

    const QByteArray *find(quint32 key) const
    {
        const QHash<quint32, QByteArray>::const_iterator found = this->blocks.constFind(key);
        return found == this->blocks.cend() ? nullptr : &found.value();
    }

    bool insert(quint32 key, const QByteArray &block)
    {
        if (block.isEmpty() || block.size() > this->maximum_bytes)
            return false;
        if (this->blocks.contains(key))
            return false;
        while (this->stored_bytes > this->maximum_bytes - block.size())
        {
            if (this->insertion_order.isEmpty())
                return false;
            const quint32 oldest = this->insertion_order.dequeue();
            const QHash<quint32, QByteArray>::iterator existing = this->blocks.find(oldest);
            if (existing != this->blocks.end())
            {
                const qsizetype evicted_bytes = existing.value().size();
                this->stored_bytes -= evicted_bytes;
                if (this->shared_budget != nullptr)
                    this->shared_budget->release(evicted_bytes);
                this->blocks.erase(existing);
            }
        }
        if (this->shared_budget != nullptr && !this->shared_budget->reserve(block.size()))
            return false;
        this->blocks.insert(key, block);
        this->insertion_order.enqueue(key);
        this->stored_bytes += block.size();
        return true;
    }

    qsizetype bytes() const { return this->stored_bytes; }
    qsizetype count() const { return this->blocks.size(); }

private:
    qsizetype maximum_bytes = 0;
    CopernicusDecodedBlockBudget *shared_budget = nullptr;
    qsizetype stored_bytes = 0;
    QHash<quint32, QByteArray> blocks;
    QQueue<quint32> insertion_order;
};
}
