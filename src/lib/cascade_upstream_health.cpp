#include <aowis/map/cascade_upstream_health.h>

#include <QMutexLocker>

namespace Aowis::Map
{
bool isCascadeInstanceId(const QByteArray &value)
{
    if (value.size() != 36) return false;
    for (qsizetype index = 0; index < value.size(); ++index)
    {
        const char character = value.at(index);
        const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen ? character != '-' :
            !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            return false;
    }
    return true;
}

QList<QByteArray> cascadeTraceIds(const QByteArray &trace)
{
    QList<QByteArray> ids;
    if (trace.isEmpty()) return ids;
    const QList<QByteArray> parts = trace.split(',');
    for (const QByteArray &part : parts)
    {
        const QByteArray id = part.trimmed();
        if (isCascadeInstanceId(id)) ids.append(id);
    }
    return ids;
}

qint64 CascadeUpstreamHealth::cooldownForFailures(int consecutive_failures)
{
    if (consecutive_failures <= 0) return 0;
    // 5, 10, 20, 40, then 60 s: the shift is capped before it can overflow.
    const int doublings = consecutive_failures - 1 < 4 ? consecutive_failures - 1 : 4;
    const qint64 cooldown = MinimumCooldownMs << doublings;
    return cooldown < MaximumCooldownMs ? cooldown : MaximumCooldownMs;
}

bool CascadeUpstreamHealth::coolingDown(const QString &name, qint64 now_ms) const
{
    QMutexLocker locker(&this->mutex);
    const QHash<QString, CascadeUpstreamState>::const_iterator state = this->states.constFind(name);
    return state != this->states.constEnd() && state->cooldown_until_ms > now_ms;
}

void CascadeUpstreamHealth::recordAnswered(const QString &name, const QByteArray &instance_id,
                                           qint64 now_ms)
{
    QMutexLocker locker(&this->mutex);
    CascadeUpstreamState &state = this->states[name];
    state.consecutive_failures = 0;
    state.cooldown_until_ms = 0;
    state.last_answer_ms = now_ms;
    if (isCascadeInstanceId(instance_id))
        state.instance_id = instance_id;
}

void CascadeUpstreamHealth::recordUnreachable(const QString &name, const QString &error,
                                              qint64 now_ms)
{
    QMutexLocker locker(&this->mutex);
    CascadeUpstreamState &state = this->states[name];
    state.last_failure_ms = now_ms;
    state.last_error = error;
    // Parallel requests that were already in flight when the upstream went
    // away fail one after another during the cooldown their first failure
    // started; they are one outage, not several, and must not escalate it.
    if (state.cooldown_until_ms > now_ms)
        return;
    ++state.consecutive_failures;
    state.cooldown_until_ms = now_ms + cooldownForFailures(state.consecutive_failures);
}

bool CascadeUpstreamHealth::instanceInTrace(const QString &name,
                                            const QList<QByteArray> &trace_ids) const
{
    if (trace_ids.isEmpty()) return false;
    QMutexLocker locker(&this->mutex);
    const QHash<QString, CascadeUpstreamState>::const_iterator state = this->states.constFind(name);
    return state != this->states.constEnd() && !state->instance_id.isEmpty() &&
           trace_ids.contains(state->instance_id);
}

bool CascadeUpstreamHealth::refusesJoin(const QString &name, const QList<QByteArray> &trace_ids,
                                        const QByteArray &own_id) const
{
    if (trace_ids.isEmpty() || !isCascadeInstanceId(own_id)) return false;
    QMutexLocker locker(&this->mutex);
    const QHash<QString, CascadeUpstreamState>::const_iterator state = this->states.constFind(name);
    return state != this->states.constEnd() && !state->instance_id.isEmpty() &&
           trace_ids.contains(state->instance_id) && state->instance_id < own_id;
}

QHash<QString, CascadeUpstreamState> CascadeUpstreamHealth::snapshot() const
{
    QMutexLocker locker(&this->mutex);
    return this->states;
}

void CascadeUpstreamHealth::reset()
{
    QMutexLocker locker(&this->mutex);
    this->states.clear();
}
}
