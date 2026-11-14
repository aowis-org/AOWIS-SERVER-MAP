#ifndef AOWIS_MAP_CASCADE_UPSTREAM_HEALTH_H
#define AOWIS_MAP_CASCADE_UPSTREAM_HEALTH_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

namespace Aowis::Map
{
// Every MAP response that matters to a downstream (raster tiles, normalized
// terrain, the provider catalog) carries the answering server's cascade
// instance ID. A response that carries it was produced by a MAP server, so
// that server is reachable, whatever the HTTP status says: "this tile is not
// available", "I am busy" or "my own source failed" are answers about one
// request, never a reason to stop asking that upstream for other tiles. Only
// a request that did not reach a MAP server (connection failure, timeout, a
// reverse proxy's own 5xx without the header) counts as an outage.
inline constexpr const char *CascadeInstanceHeader = "X-AOWIS-Instance";
// Optional detail of an answered non-success response. "busy" means the
// upstream's admission limits rejected the request: retry later, and never
// treat it as a reason to download the original source instead.
inline constexpr const char *CascadeUpstreamStatusHeader = "X-AOWIS-Upstream-Status";
inline constexpr const char *CascadeUpstreamStatusBusy = "busy";
// Asks a MAP server not to hold the request while it acquires a missing
// tile: it starts (or joins) the acquisition and answers 202 with
// Retry-After; the acquisition continues even when the requester goes away.
inline constexpr const char *CascadeTileDeliveryHeader = "X-AOWIS-Tile-Delivery";
inline constexpr const char *CascadeTileDeliveryCacheFirst = "cache-first";

// Same syntax as the IDs in X-AOWIS-Cascade-Visited: a lowercase UUID.
bool isCascadeInstanceId(const QByteArray &value);
// The syntactically valid IDs of a visited trace (malformed entries ignored;
// the trace itself is validated by MapTiles::acceptsCascadeTrace).
QList<QByteArray> cascadeTraceIds(const QByteArray &trace);

struct CascadeUpstreamState
{
    int consecutive_failures = 0;   // Unreachable attempts since the last answer
    qint64 cooldown_until_ms = 0;   // Epoch ms; 0: no cooldown
    qint64 last_answer_ms = 0;      // Epoch ms of the last answered request
    qint64 last_failure_ms = 0;     // Epoch ms of the last unreachable attempt
    QString last_error;             // Of the last unreachable attempt
    QByteArray instance_id;         // Learned from X-AOWIS-Instance; may be empty
};

// Reachability bookkeeping of the configured upstreams, keyed by upstream
// name. Thread-safe: raster lookups run on the MAP thread, terrain lookups on
// worker threads.
//
// Cooldown: the first unreachable attempt suspends the upstream for 5 s, each
// further one doubles that, up to 60 s; any answer ends it. A fixed 5 s
// cooldown was shorter than the 10 s it takes a dead upstream to time out, so
// most new tiles still waited for the dead upstream before failing over.
//
// Loop avoidance: an upstream whose learned instance ID already appears in a
// request's visited trace is waiting for this very server, so asking it (or
// joining a download that is asking it) can only wait for a timeout.
class CascadeUpstreamHealth final
{
public:
    static constexpr qint64 MinimumCooldownMs = 5000;
    static constexpr qint64 MaximumCooldownMs = 60000;

    CascadeUpstreamHealth() = default;
    CascadeUpstreamHealth(const CascadeUpstreamHealth &) = delete;
    CascadeUpstreamHealth &operator=(const CascadeUpstreamHealth &) = delete;

    bool coolingDown(const QString &name, qint64 now_ms) const;
    // instance_id may be empty (an older MAP release or a plain 200 without
    // the header); a valid one replaces the learned ID.
    void recordAnswered(const QString &name, const QByteArray &instance_id, qint64 now_ms);
    void recordUnreachable(const QString &name, const QString &error, qint64 now_ms);
    bool instanceInTrace(const QString &name, const QList<QByteArray> &trace_ids) const;
    // Two servers that are upstreams of each other and acquire the same tile
    // at once ask each other. Exactly one of them must refuse the other's
    // request to join its acquisition, or both wait; if both refused, both
    // would give up on the other. The tie-break: the server with the higher
    // instance ID refuses. True when the upstream `name`, which the running
    // acquisition is asking, is in trace_ids and has a lower ID than own_id.
    bool refusesJoin(const QString &name, const QList<QByteArray> &trace_ids,
                     const QByteArray &own_id) const;
    QHash<QString, CascadeUpstreamState> snapshot() const;
    // Upstreams changed (names may now denote other servers): forget all.
    void reset();

    static qint64 cooldownForFailures(int consecutive_failures);

private:
    mutable QMutex mutex;
    QHash<QString, CascadeUpstreamState> states;
};
}

#endif // AOWIS_MAP_CASCADE_UPSTREAM_HEALTH_H
