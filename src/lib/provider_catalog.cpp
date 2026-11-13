#include <aowis/map/provider_catalog.h>
#include <aowis/map/settings_repository.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

namespace
{
constexpr qsizetype MaximumCatalogBytes = 512 * 1024;
constexpr int CatalogRequestTimeoutMs = 6000;

QString origin(const QUrl &url)
{
    return url.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
}
}

namespace Aowis::Map
{
ProviderCatalog::ProviderCatalog(const QString &cache_directory, QObject *parent)
    : QObject(parent), cache_directory_(cache_directory)
{
    this->refresh_timer_.setInterval(15 * 60 * 1000);
    QObject::connect(&this->refresh_timer_, &QTimer::timeout,
                     this, &ProviderCatalog::refresh);
}

QString ProviderCatalog::cachePath(const MapCascadeUpstream &upstream) const
{
    // Use the origin rather than the display name, so changing the upstream
    // address cannot accidentally reuse an unrelated catalog snapshot.
    // A different upstream read identity may see a different provider catalog.
    // Include the key in the digest, never the cache file's contents or name.
    const QByteArray identity = origin(upstream.url).toUtf8() + '\0' + upstream.api_key;
    const QByteArray digest = QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex();
    return QDir(this->cache_directory_).filePath(
        QStringLiteral("provider-catalogs/%1.json").arg(QString::fromLatin1(digest)));
}

bool ProviderCatalog::decode(const QByteArray &payload, QVector<MapProvider> *providers) const
{
    if (payload.isEmpty() || payload.size() > MaximumCatalogBytes)
        return false;
    QString error;
    return decodeProviderCatalog(payload, {}, providers, &error);
}

void ProviderCatalog::configure(const QVector<MapProvider> &local_providers,
                                const MapCascadeConfig &cascade)
{
    ++this->generation_;
    this->local_providers_ = local_providers;
    this->cascade_ = cascade;
    // Managers of tunnels no longer configured: replies of the previous
    // generation are ignored, then the manager goes.
    QSet<QString> current_tunnels;
    for (const MapCascadeUpstream &upstream : cascade.upstreams)
        if (hasWireGuardTransport(upstream))
            current_tunnels.insert(cascadeProxyKey(upstream.proxy));
    for (QHash<QString, QNetworkAccessManager *>::iterator network = this->tunnel_networks_.begin();
         network != this->tunnel_networks_.end();)
    {
        if (current_tunnels.contains(network.key()))
        {
            ++network;
            continue;
        }
        network.value()->deleteLater();
        network = this->tunnel_networks_.erase(network);
    }
    if (cascade.upstreams.isEmpty()) this->refresh_timer_.stop();
    else this->refresh_timer_.start();
    this->upstream_catalogs_.clear();
    for (const MapCascadeUpstream &upstream : this->cascade_.upstreams)
    {
        QFile snapshot(cachePath(upstream));
        if (!snapshot.open(QIODevice::ReadOnly) || snapshot.size() > MaximumCatalogBytes)
            continue;
        QVector<MapProvider> providers;
        if (decode(snapshot.readAll(), &providers))
            this->upstream_catalogs_.insert(upstream.name, providers);
    }
    rebuild();
}

void ProviderCatalog::setLocalProviders(const QVector<MapProvider> &local_providers)
{
    this->local_providers_ = local_providers;
    rebuild();
}

void ProviderCatalog::setDisabledInheritedProviders(const QSet<QString> &ids)
{
    if (this->disabled_inherited_ids_ == ids) return;
    this->disabled_inherited_ids_ = ids;
    rebuild();
}

void ProviderCatalog::setUpstreamRoutes(const QHash<QString, QStringList> &routes)
{
    if (this->upstream_routes_ == routes) return;
    this->upstream_routes_ = routes;
    rebuild();
}

void ProviderCatalog::setAcquisitionModes(
    const QHash<QString, MapProviderAcquisitionMode> &modes)
{
    if (this->acquisition_modes_ == modes) return;
    this->acquisition_modes_ = modes;
    rebuild();
}

void ProviderCatalog::rebuild()
{
    QVector<AvailableMapProvider> combined;
    QHash<QString, qsizetype> positions;
    QSet<QString> locally_defined;
    for (const MapProvider &provider : this->local_providers_)
    {
        locally_defined.insert(provider.id);
        positions.insert(provider.id, combined.size());
        combined.append({provider, false, {}});
    }

    for (const MapCascadeUpstream &upstream : this->cascade_.upstreams)
    {
        const QVector<MapProvider> catalog = this->upstream_catalogs_.value(upstream.name);
        for (const MapProvider &advertised : catalog)
        {
            if (!advertised.enabled || !advertised.remote_fetch_enabled)
                continue;
            if (locally_defined.contains(advertised.id))
            {
                // A matching local definition remains locally editable, but
                // it is still useful to advertise which upstreams offer it.
                // Never associate a different source with the same UUID.
                const QHash<QString, qsizetype>::const_iterator local =
                    positions.constFind(advertised.id);
                if (local != positions.cend())
                {
                    AvailableMapProvider &selected = combined[local.value()];
                    if (selected.provider.url_template == advertised.url_template)
                        selected.upstream_names.append(upstream.name);
                }
                continue;
            }
            const QHash<QString, qsizetype>::const_iterator existing =
                positions.constFind(advertised.id);
            if (existing != positions.cend())
            {
                AvailableMapProvider &selected = combined[existing.value()];
                // An ID must never silently refer to different imagery/styles.
                if (selected.provider.url_template == advertised.url_template)
                    selected.upstream_names.append(upstream.name);
                continue;
            }
            MapProvider inherited = advertised;
            // An upstream's coverage profile is not necessarily defined on
            // this downstream. Apply the downstream's own default coverage.
            inherited.coverage.clear();
            inherited.enabled = !this->disabled_inherited_ids_.contains(inherited.id);
            inherited.remote_fetch_enabled = true;
            positions.insert(inherited.id, combined.size());
            combined.append({inherited, true, {upstream.name}});
        }
    }
    QJsonArray entries;
    for (AvailableMapProvider &entry : combined)
    {
        entry.acquisition_mode = this->acquisition_modes_.value(entry.provider.id,
            entry.inherited ? MapProviderAcquisitionMode::Cascade
                            : MapProviderAcquisitionMode::Automatic);
        // The administrator policy is the upper bound. Even in a local-source
        // catalog entry, Direct must never become selectable when disallowed.
        entry.direct_fetch_allowed = !entry.inherited && this->cascade_.allow_direct_fetch;
        if (!entry.direct_fetch_allowed &&
            entry.acquisition_mode == MapProviderAcquisitionMode::Direct)
            entry.acquisition_mode = MapProviderAcquisitionMode::Cascade;
        const QStringList selection = this->upstream_routes_.value(entry.provider.id);
        if (selection.isEmpty()) entry.selected_upstreams = entry.upstream_names;
        else
            for (const QString &name : selection)
                if (entry.upstream_names.contains(name)) entry.selected_upstreams.append(name);
    }
    for (const AvailableMapProvider &entry : combined)
    {
        const QVector<MapProvider> single{entry.provider};
        const QJsonDocument document = QJsonDocument::fromJson(encodeProviderCatalog(single));
        QJsonObject object = document.object().value(QStringLiteral("providers"))
            .toArray().first().toObject();
        object.insert(QStringLiteral("origin"), entry.inherited ?
            QStringLiteral("inherited") : QStringLiteral("local"));
        QJsonArray upstreams;
        for (const QString &name : entry.upstream_names)
            upstreams.append(name);
        object.insert(QStringLiteral("upstreams"), upstreams);
        QJsonArray selected_json;
        for (const QString &name : entry.selected_upstreams)
            selected_json.append(name);
        object.insert(QStringLiteral("selected_upstreams"), selected_json);
        object.insert(QStringLiteral("acquisition_mode"),
                      providerAcquisitionModeName(entry.acquisition_mode));
        object.insert(QStringLiteral("direct_fetch_allowed"), entry.direct_fetch_allowed);
        entries.append(object);
    }
    const QByteArray next = QJsonDocument(QJsonObject{
        {QStringLiteral("providers"), entries}}).toJson(QJsonDocument::Compact);
    this->available_ = combined;
    if (next != this->encoded_)
    {
        this->encoded_ = next;
        emit catalogChanged();
    }
}

QVector<AvailableMapProvider> ProviderCatalog::available() const
{
    return this->available_;
}

QByteArray ProviderCatalog::encodedAvailable() const
{
    return this->encoded_;
}

QHash<QString, QSet<QString>> ProviderCatalog::inheritedUpstreams() const
{
    QHash<QString, QSet<QString>> result;
    for (const AvailableMapProvider &entry : this->available_)
    {
        if (!entry.inherited || !entry.provider.enabled) continue;
        QSet<QString> names;
        for (const QString &name : entry.upstream_names)
            names.insert(name);
        result.insert(entry.provider.id, names);
    }
    return result;
}

void ProviderCatalog::refresh()
{
    const quint64 generation = this->generation_;
    for (const MapCascadeUpstream &upstream : this->cascade_.upstreams)
    {
        // A WireGuard upstream only through its running tunnel, never
        // directly. Its validated offline snapshot (configure) stays available.
        if (!cascadeUpstreamUsable(upstream)) continue;
        fetch(upstream, false, generation);
    }
}

void ProviderCatalog::fetch(const MapCascadeUpstream &upstream, bool legacy,
                            quint64 generation)
{
    const QString path = legacy ? QStringLiteral("/map/v1/providers") :
        QStringLiteral("/map/v1/providers/available");
    QNetworkRequest request(QUrl(origin(upstream.url) + path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(CatalogRequestTimeoutMs);
    request.setRawHeader("Accept", "application/json");
    if (!upstream.api_key.isEmpty())
        request.setRawHeader("X-API-Key", upstream.api_key);
    QNetworkAccessManager *network = &this->network_;
    if (hasWireGuardTransport(upstream))
    {
        const QString key = cascadeProxyKey(upstream.proxy);
        network = this->tunnel_networks_.value(key);
        if (network == nullptr)
        {
            network = new QNetworkAccessManager(this);
            network->setProxy(upstream.proxy);
            this->tunnel_networks_.insert(key, network);
        }
    }
    QNetworkReply *reply = network->get(request);
    QTimer::singleShot(CatalogRequestTimeoutMs, reply, [reply]()
    {
        if (!reply->isFinished()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
        [reply](qint64 received, qint64 total)
    {
        if (received > MaximumCatalogBytes || total > MaximumCatalogBytes)
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, this,
        [this, reply, upstream, legacy, generation]()
    {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status == 200 &&
            reply->bytesAvailable() <= MaximumCatalogBytes;
        const QByteArray payload = ok ? reply->readAll() : QByteArray();
        reply->deleteLater();
        if (generation != this->generation_) return;
        if (!ok)
        {
            if (status == 404 && !legacy)
                fetch(upstream, true, generation); // Older MAP version.
            return; // Keep last validated snapshot for disconnected operation.
        }
        QVector<MapProvider> providers;
        if (!decode(payload, &providers)) return;
        const QString file_path = cachePath(upstream);
        if (QDir().mkpath(QFileInfo(file_path).absolutePath()))
        {
            QSaveFile snapshot(file_path);
            if (snapshot.open(QIODevice::WriteOnly))
            {
                snapshot.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                if (snapshot.write(payload) != payload.size())
                    snapshot.cancelWriting();
                else
                    snapshot.commit();
            }
        }
        this->upstream_catalogs_.insert(upstream.name, providers);
        rebuild();
    });
}
}
