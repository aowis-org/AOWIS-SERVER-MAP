#include <aowis/map/settings_repository.h>
#include "server.h"

#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_dataset.h>
#include <aowis/map/terrain_transfer.h>

#include <QDebug>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QHttpHeaders>
#endif
#include <QThread>
#include <QTcpSocket>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <QJsonArray>
#include <QUrl>
#include <QRegularExpression>
#include "configuration.h"
#include <QJsonObject>
#include <QMutexLocker>
#include <QUrlQuery>
#include <QtEndian>
#include <cstring>
#include <cmath>
#include <QtConcurrentRun>

#include <optional>
#include <utility>

// QHttpServerRequest exposes the peer address and port, but not its socket or
// a per-request disconnect signal. Install socket tracking before the HTTP
// server consumes each accepted connection; never inspect Qt private APIs.
class TerrainTrackingTcpServer final : public QTcpServer
{
public:
    explicit TerrainTrackingTcpServer(QObject *parent) : QTcpServer(parent) {}

    std::function<void(QTcpSocket *)> socket_accepted;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor))
        {
            delete socket;
            return;
        }
        if (this->socket_accepted)
            this->socket_accepted(socket);
        addPendingConnection(socket);
        // addPendingConnection emits pendingConnectionAvailable, but the
        // QTcpServer default handler also emits newConnection explicitly.
        // QHttpServer.bind() relies on that notification to accept sockets.
        emit newConnection();
    }
};

namespace
{
QFuture<QHttpServerResponse> makeReadyResponse(QHttpServerResponse response)
{
    QPromise<QHttpServerResponse> promise;
    promise.start();
    QFuture<QHttpServerResponse> future = promise.future();
    promise.addResult(std::move(response));
    promise.finish();
    return future;
}

// Preserve Content-Type while adding the source identity required by downstream
// MAP servers. Qt 6.8+ exposes QHttpHeaders instead of setHeader().
void setRasterSourceHeader(QHttpServerResponse &response, const QByteArray &source)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QHttpHeaders headers = response.headers();
    headers.replaceOrAppend("X-AOWIS-Raster-Source", source);
    response.setHeaders(std::move(headers));
#else
    response.setHeader("X-AOWIS-Raster-Source", source);
#endif
}

void setRasterTransferVary(QHttpServerResponse &response)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QHttpHeaders headers = response.headers();
    headers.replaceOrAppend("Vary", "X-AOWIS-Raster-Transfer, X-AOWIS-Cascade-Visited");
    response.setHeaders(std::move(headers));
#else
    response.setHeader("Vary", "X-AOWIS-Raster-Transfer, X-AOWIS-Cascade-Visited");
#endif
}

// Only an upstream MAP peer with a valid cascade trace may request lossy
// transport. A GUI supplying this header without the trace is ignored.
int requestedRasterJpegQuality(const QHttpServerRequest &request)
{
    if (request.value("x-aowis-cascade-visited").isEmpty()) return 0;
    const QByteArray value = request.value("x-aowis-raster-transfer").trimmed().toLower();
    const QByteArray prefix = QByteArrayLiteral("adaptive-jpeg;quality=");
    if (!value.startsWith(prefix)) return 0;
    const QByteArray digits = value.mid(prefix.size());
    if (digits.isEmpty() || digits.size() > 3) return 0;
    for (char digit : digits)
        if (digit < '0' || digit > '9') return 0;
    bool valid = false;
    const int quality = digits.toInt(&valid);
    return valid && quality >= 1 && quality <= 100 ? quality : 0;
}

// The r2 normalization semantics are independent of the .aowterrain binary
// format version. A downstream must reject incompatible normalizer revisions.
void setTerrainRevisionHeader(QHttpServerResponse &response)
{
    const QByteArray revision = QByteArray::number(Aowis::Map::TerrainNormalizationRevision);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QHttpHeaders headers = response.headers();
    headers.replaceOrAppend("X-AOWIS-Terrain-Revision", revision);
    response.setHeaders(std::move(headers));
#else
    response.setHeader("X-AOWIS-Terrain-Revision", revision);
#endif
}

// Mark every canonical tile representation as varying with compression
// negotiation. Never change the underlying normalized tile cache.
void setTerrainTransferHeaders(QHttpServerResponse &response, bool compressed)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QHttpHeaders headers = response.headers();
    headers.replaceOrAppend("Vary", "Accept-Encoding");
    if (compressed)
        headers.replaceOrAppend("Content-Encoding", "zstd");
    response.setHeaders(std::move(headers));
#else
    response.setHeader("Vary", "Accept-Encoding");
    if (compressed)
        response.setHeader("Content-Encoding", "zstd");
#endif
}

bool requestAcceptsTerrainZstd(const QHttpServerRequest *request)
{
    if (request == nullptr) return false;
    // Qt may advertise zstd automatically for ordinary GUI HTTP requests.
    // Only MAP-to-MAP requests carry a nonempty, validated cascade trace;
    // never change the canonical terrain representation sent to GUI clients.
    if (request->value("x-aowis-cascade-visited").isEmpty()) return false;
    const QList<QByteArray> encodings = request->value("accept-encoding").toLower().split(',');
    for (const QByteArray &encoding : encodings)
    {
        const QList<QByteArray> attributes = encoding.split(';');
        if (attributes.first().trimmed() != "zstd") continue;
        bool disabled = false;
        for (qsizetype index = 1; index < attributes.size(); ++index)
        {
            const QByteArray parameter = attributes.at(index).trimmed();
            if (parameter.startsWith("q="))
            {
                bool valid = false;
                const double quality = parameter.mid(2).toDouble(&valid);
                if (!valid || quality <= 0.0) disabled = true;
            }
        }
        if (!disabled) return true;
    }
    return false;
}

// Upstream raster providers deliver PNG, JPEG or WebP (e.g. ArcGIS imagery
// is JPEG); TileHttpClient only accepts these three signatures.
QByteArray mapTileMimeType(const QByteArray &data)
{
    if (data.size() >= 3 && uchar(data.at(0)) == 0xff && uchar(data.at(1)) == 0xd8 &&
        uchar(data.at(2)) == 0xff)
        return QByteArrayLiteral("image/jpeg");
    if (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return QByteArrayLiteral("image/webp");
    return QByteArrayLiteral("image/png");
}

QHttpServerResponse makeRasterResponse(const QByteArray &bytes, const QByteArray &source)
{
    QHttpServerResponse response(mapTileMimeType(bytes), bytes);
    setRasterSourceHeader(response, source);
    setRasterTransferVary(response);
    return response;
}

QHttpServerResponse makeOptionsResponse()
{
    return QHttpServerResponse(QHttpServerResponse::StatusCode::NoContent);
}

QHttpServerResponse makeUnauthorizedResponse()
{
    return QHttpServerResponse(
        "Missing or invalid API key",
        QHttpServerResponse::StatusCode::Unauthorized);
}

QHttpServerResponse makeTerrainTileErrorResponse(
    const Aowis::Map::TerrainTileLookupResult &result)
{
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::InternalServerError;
    switch (result.status)
    {
        case Aowis::Map::TerrainTileLookupStatus::Disabled:
        case Aowis::Map::TerrainTileLookupStatus::NotInitialized:
            status = QHttpServerResponse::StatusCode::ServiceUnavailable;
            break;
        case Aowis::Map::TerrainTileLookupStatus::InvalidDataset:
        case Aowis::Map::TerrainTileLookupStatus::InvalidAddress:
            status = QHttpServerResponse::StatusCode::BadRequest;
            break;
        case Aowis::Map::TerrainTileLookupStatus::TileUnavailable:
            status = QHttpServerResponse::StatusCode::NotFound;
            break;
        case Aowis::Map::TerrainTileLookupStatus::RemoteFetchError:
            status = QHttpServerResponse::StatusCode::BadGateway;
            break;
        case Aowis::Map::TerrainTileLookupStatus::TileReadError:
        case Aowis::Map::TerrainTileLookupStatus::CorruptTile:
        case Aowis::Map::TerrainTileLookupStatus::ProviderError:
        case Aowis::Map::TerrainTileLookupStatus::Ready:
        default:
            status = QHttpServerResponse::StatusCode::InternalServerError;
            break;
    }

    const QString message = result.error_message.isEmpty()
        ? QStringLiteral("Terrain tile request failed: %1")
              .arg(Aowis::Map::terrainTileLookupStatusId(result.status))
        : result.error_message;
    return QHttpServerResponse(message, status);
}

QHttpServerResponse makeTerrainElevationJsonResponse(
    const QJsonObject &object,
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::Ok)
{
    return QHttpServerResponse(
        QByteArrayLiteral("application/json"),
        QJsonDocument(object).toJson(QJsonDocument::Compact),
        status);
}

QHttpServerResponse makeTerrainElevationErrorResponse(
    const Aowis::Map::TerrainElevationLookupResult &result)
{
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::InternalServerError;
    switch (result.status)
    {
        case Aowis::Map::TerrainElevationLookupStatus::Disabled:
        case Aowis::Map::TerrainElevationLookupStatus::NotInitialized:
            status = QHttpServerResponse::StatusCode::ServiceUnavailable;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::InvalidCoordinate:
        case Aowis::Map::TerrainElevationLookupStatus::InvalidDataset:
        case Aowis::Map::TerrainElevationLookupStatus::InvalidVerticalDatum:
        case Aowis::Map::TerrainElevationLookupStatus::VerticalDatumConversionUnavailable:
            status = QHttpServerResponse::StatusCode::BadRequest;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::OutsideCoverage:
        case Aowis::Map::TerrainElevationLookupStatus::TileUnavailable:
        case Aowis::Map::TerrainElevationLookupStatus::NoData:
            status = QHttpServerResponse::StatusCode::NotFound;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::RemoteFetchError:
            status = QHttpServerResponse::StatusCode::BadGateway;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::TileReadError:
        case Aowis::Map::TerrainElevationLookupStatus::CorruptTile:
        case Aowis::Map::TerrainElevationLookupStatus::ProviderError:
        case Aowis::Map::TerrainElevationLookupStatus::Ready:
        default:
            status = QHttpServerResponse::StatusCode::InternalServerError;
            break;
    }

    QJsonObject object;
    object.insert(QStringLiteral("status"),
                  Aowis::Map::terrainElevationLookupStatusId(result.status));
    if (!result.error_message.isEmpty())
        object.insert(QStringLiteral("error"), result.error_message);
    if (result.requested_vertical_datum.has_value())
    {
        object.insert(QStringLiteral("requested_vertical_datum"),
                      Aowis::Map::terrainVerticalDatumId(
                          result.requested_vertical_datum.value()));
    }
    if (result.source_vertical_datum.has_value())
    {
        object.insert(QStringLiteral("source_vertical_datum"),
                      Aowis::Map::terrainVerticalDatumId(
                          result.source_vertical_datum.value()));
    }
    return makeTerrainElevationJsonResponse(object, status);
}

QHttpServerResponse makeTerrainElevationSuccessResponse(
    const Aowis::Map::TerrainElevationLookupResult &result)
{
    if (result.status != Aowis::Map::TerrainElevationLookupStatus::Ready ||
        !result.sample.has_value())
    {
        return makeTerrainElevationErrorResponse(result);
    }

    const Aowis::Map::TerrainElevationSample &sample = result.sample.value();
    QJsonObject object;
    object.insert(QStringLiteral("status"), QStringLiteral("ready"));
    object.insert(QStringLiteral("elevation_m"), sample.elevation_m);
    object.insert(QStringLiteral("dataset"), sample.dataset);
    object.insert(QStringLiteral("nominal_resolution_m"), sample.nominal_resolution_m);
    object.insert(QStringLiteral("vertical_datum"),
                  Aowis::Map::terrainVerticalDatumId(sample.vertical_datum));
    object.insert(QStringLiteral("vertical_datum_name"),
                  Aowis::Map::terrainVerticalDatumDisplayName(sample.vertical_datum));
    const QString vertical_datum_authority =
        Aowis::Map::terrainVerticalDatumAuthorityCode(sample.vertical_datum);
    if (!vertical_datum_authority.isEmpty())
    {
        object.insert(QStringLiteral("vertical_datum_authority"), vertical_datum_authority);
    }
    object.insert(QStringLiteral("vertical_reference"),
                  Aowis::Map::terrainVerticalReferenceId(
                      Aowis::Map::terrainVerticalReference(sample.vertical_datum)));
    object.insert(QStringLiteral("source_vertical_datum"),
                  Aowis::Map::terrainVerticalDatumId(sample.source_vertical_datum));
    object.insert(QStringLiteral("origin"), Aowis::Map::terrainDataOriginId(sample.origin));

    if (result.tile_address.has_value())
    {
        const Aowis::Map::TerrainTileAddress &address = result.tile_address.value();
        QJsonObject tile;
        tile.insert(QStringLiteral("zoom"), address.zoom);
        tile.insert(QStringLiteral("x"), double(address.x));
        tile.insert(QStringLiteral("y"), double(address.y));
        object.insert(QStringLiteral("tile"), tile);
    }

    return makeTerrainElevationJsonResponse(object);
}

QHttpServerResponse makeTerrainCacheDeletionResponse(
    const Aowis::Map::TerrainCacheDeletionResult &result)
{
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::Ok;
    switch (result.status)
    {
        case Aowis::Map::TerrainCacheDeletionStatus::Deleted:
            // The deleted file count, as the map-tile cache DELETE answers.
            return QHttpServerResponse(QString::number(result.deleted_count),
                                       QHttpServerResponse::StatusCode::Ok);
        case Aowis::Map::TerrainCacheDeletionStatus::Disabled:
        case Aowis::Map::TerrainCacheDeletionStatus::NotInitialized:
            status = QHttpServerResponse::StatusCode::ServiceUnavailable;
            break;
        case Aowis::Map::TerrainCacheDeletionStatus::InvalidDataset:
        case Aowis::Map::TerrainCacheDeletionStatus::InvalidRange:
            status = QHttpServerResponse::StatusCode::BadRequest;
            break;
        case Aowis::Map::TerrainCacheDeletionStatus::Failed:
        default:
            status = QHttpServerResponse::StatusCode::InternalServerError;
            break;
    }

    const QString message = result.error_message.isEmpty()
        ? QStringLiteral("Terrain cache deletion failed: %1")
              .arg(Aowis::Map::terrainCacheDeletionStatusId(result.status))
        : result.error_message;
    return QHttpServerResponse(message, status);
}

bool secureEquals(const QByteArray &left, const QByteArray &right)
{
    const qsizetype maximum_size = qMax(left.size(), right.size());
    quint64 difference = quint64(left.size()) ^ quint64(right.size());
    for (qsizetype index = 0; index < maximum_size; ++index)
    {
        const uchar left_value = index < left.size() ? uchar(left.at(index)) : 0;
        const uchar right_value = index < right.size() ? uchar(right.at(index)) : 0;
        difference |= left_value ^ right_value;
    }

    return difference == 0;
}
}

Server::Server(const Config &config, QObject *parent)
    : QObject(parent),
      config(config),
      tcp(new TerrainTrackingTcpServer(this)),
      terrain_admission(qMax(1, config.maximum_terrain_jobs)),
      terrain_remote_admission(qMax(1, config.maximum_terrain_jobs / 2)),
      terrain_grid_bytes(64ULL * 1024 * 1024),
      maptiles(new MapTiles(config.cache_directory, config.maximum_active_downloads,
                            config.maximum_pending_requests, this)),
      terrain_data(new Aowis::Map::TerrainData(
          {
              config.terrain_enabled,
              config.terrain_remote_fetch_enabled,
              config.cache_directory,
              config.terrain_cache_directory,
              config.permanent_data_directory,
              config.copernicus_enabled,
              config.terrain_connect_timeout_seconds,
              config.terrain_inactivity_timeout_seconds,
              config.terrain_download_deadline_seconds,
              config.terrain_integration_source_url,
              config.terrain_coverage,
              config.download_coverage,
              config.cascade
          },
          this)),
      pending_request_count(0)
{
    static_cast<TerrainTrackingTcpServer *>(this->tcp)->socket_accepted =
        [this](QTcpSocket *socket) { trackTerrainSocket(socket); };
    // Terrain work must not outlive the server or consume the global Qt pool.
    this->terrain_worker_pool.setObjectName(QStringLiteral("aowis-map-terrain"));
    this->terrain_worker_pool.setMaxThreadCount(
        qMax(1, qMin(config.maximum_terrain_workers, config.maximum_terrain_jobs)));
    this->terrain_remote_pool.setObjectName(QStringLiteral("aowis-map-terrain-remote"));
    this->terrain_remote_pool.setMaxThreadCount(
        qMax(1, qMin(config.maximum_terrain_remote_workers, config.maximum_terrain_jobs)));
    this->raster_encode_pool.setObjectName(QStringLiteral("aowis-map-raster-encode"));
    this->raster_encode_pool.setMaxThreadCount(2);
    this->maptiles->setRemoteFetchEnabled(config.map_remote_fetch_enabled);
    this->maptiles->setCascadeConfig(config.cascade);
    this->terrain_data->setCascadeInstanceId(this->maptiles->cascadeInstanceId());
    this->maptiles->setPermanentDataDirectory(config.permanent_data_directory);
    this->provider_catalog = new Aowis::Map::ProviderCatalog(config.cache_directory, this);
    connect(this->provider_catalog, &Aowis::Map::ProviderCatalog::catalogChanged,
            this, &Server::applyAvailableProviders);
    this->provider_catalog->configure(config.map_providers, config.cascade);
    this->provider_catalog->setDisabledInheritedProviders(config.disabled_inherited_providers);
    applyAvailableProviders();
    connect(this->maptiles, &MapTiles::tileReady, this, &Server::onTileReady);
    connect(this->maptiles, &MapTiles::tileFailed, this, &Server::onTileFailed);
    setupRoutes();
}

void Server::applyAvailableProviders()
{
    QVector<Aowis::Map::MapProvider> active;
    for (const Aowis::Map::AvailableMapProvider &entry : this->provider_catalog->available())
        active.append(entry.provider);
    this->maptiles->configureProviders(active, this->config.download_coverage,
        this->config.map_coverage, this->provider_catalog->inheritedUpstreams());
}

QString Server::terrainClientKey(const QHostAddress &address, quint16 port)
{
    return address.toString() + QLatin1Char('/') + QString::number(port);
}

void Server::trackTerrainSocket(QTcpSocket *socket)
{
    const QString key = terrainClientKey(socket->peerAddress(), socket->peerPort());
    this->terrain_client_sockets.insert(key, socket);
    // Both callbacks execute on the server thread; completing a dropped
    // response must never cancel another client's single-flight waiter.
    connect(socket, &QTcpSocket::disconnected, this, [this, socket, key]()
    {
        if (this->terrain_client_sockets.value(key) == socket)
            this->terrain_client_sockets.remove(key);
        abandonTerrainWaiters(socket);
    });
    connect(socket, &QObject::destroyed, this, [this, socket, key]()
    {
        if (this->terrain_client_sockets.value(key) == socket)
            this->terrain_client_sockets.remove(key);
        abandonTerrainWaiters(socket);
    });
}

void Server::attachTerrainWaiter(const PendingPromise &promise,
                                 const QHttpServerRequest *request)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (request != nullptr)
    {
        QTcpSocket *socket = this->terrain_client_sockets.value(
            terrainClientKey(request->remoteAddress(), request->remotePort()), nullptr);
        if (socket != nullptr)
            this->terrain_waiter_sockets.insert(promise.data(), socket);
    }
#else
    Q_UNUSED(promise);
    Q_UNUSED(request);
#endif
}

void Server::forgetTerrainWaiter(const PendingPromise &promise)
{
    this->terrain_waiter_sockets.remove(promise.data());
    this->terrain_zstd_waiters.remove(promise.data());
}

void Server::abandonTerrainWaiters(QTcpSocket *socket)
{
    // Each job has its own cancellation token. Remove an orphaned flight from
    // the registry immediately, so a new client can start a fresh job rather
    // than joining a canceled one. The old worker still owns its admission
    // reservation and releases it when it finishes.
    const QList<QPair<QHash<QString, PendingPromises> *,
                      QHash<QString, TerrainCancellation> *>> registries = {
        { &this->pending_terrain_tiles, &this->tile_cancellations },
        { &this->pending_terrain_elevations, &this->elevation_cancellations },
        { &this->pending_terrain_grids, &this->grid_cancellations }
    };
    for (const QPair<QHash<QString, PendingPromises> *,
                     QHash<QString, TerrainCancellation> *> &entry : registries)
    {
        QHash<QString, PendingPromises> *registry = entry.first;
        QHash<QString, TerrainCancellation> *cancellations = entry.second;
        QList<QString> orphans;
        for (QHash<QString, PendingPromises>::iterator it = registry->begin();
             it != registry->end(); ++it)
        {
            PendingPromises &promises = it.value();
            qsizetype index = promises.size();
            while (index > 0)
            {
                --index;
                const PendingPromise promise = promises.at(index);
                if (this->terrain_waiter_sockets.value(promise.data(), nullptr) != socket)
                    continue;
                this->terrain_waiter_sockets.remove(promise.data());
                this->terrain_zstd_waiters.remove(promise.data());
                promises.removeAt(index);
                --this->pending_terrain_tile_count;
                promise->addResult(QHttpServerResponse(
                    "HTTP client disconnected",
                    QHttpServerResponse::StatusCode::ServiceUnavailable));
                promise->finish();
            }
            if (promises.isEmpty())
                orphans.append(it.key());
        }
        for (const QString &key : orphans)
        {
            const TerrainCancellation cancellation = cancellations->take(key);
            if (cancellation)
                cancellation->store(true, std::memory_order_release);
            registry->remove(key);
        }
    }
}

Server::~Server()
{
    // Stop new responses from joining the pending map-tile registry. Drain
    // every pending future so callers never retain an unfinished promise.
    QHash<QString, PendingPromises> pending;
    {
        QMutexLocker locker(&this->mutex_pending);
        this->shutting_down = true;
        pending.swap(this->connections_pending);
        this->pending_request_count = 0;
        this->raster_transfer_waiters.clear();
    }

    this->tcp->close();
    disconnect(this->maptiles, nullptr, this, nullptr);
    this->maptiles->cancelUpstreamDownloads();
    this->terrain_data->cancelUpstreamDownloads();
    // Worker lambdas capture this and access terrain_data. Finish all of them
    // before QObject child destruction starts; no new requests are accepted.
    this->terrain_remote_pool.waitForDone();
    this->terrain_worker_pool.waitForDone();
    this->raster_encode_pool.waitForDone();

    // Any queued terrain-tile completion callbacks are canceled on shutdown.
    for (const PendingPromises &promises : std::as_const(this->pending_terrain_tiles))
    {
        for (const PendingPromise &promise : promises)
        {
            forgetTerrainWaiter(promise);
            promise->addResult(QHttpServerResponse(
                "Map server shutting down",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }
    }
    this->pending_terrain_tiles.clear();
    for (const PendingPromises &promises : std::as_const(this->pending_terrain_elevations))
    {
        for (const PendingPromise &promise : promises)
        {
            forgetTerrainWaiter(promise);
            promise->addResult(QHttpServerResponse(
                "Map server shutting down",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }
    }
    this->pending_terrain_elevations.clear();
    for (const PendingPromises &promises : std::as_const(this->pending_terrain_grids))
    {
        for (const PendingPromise &promise : promises)
        {
            forgetTerrainWaiter(promise);
            promise->addResult(QHttpServerResponse(
                "Map server shutting down",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }
    }
    this->pending_terrain_grids.clear();
    this->pending_terrain_tile_count = 0;
    this->tile_cancellations.clear();
    this->elevation_cancellations.clear();
    this->grid_cancellations.clear();
    this->terrain_waiter_sockets.clear();
    this->terrain_zstd_waiters.clear();
    this->terrain_client_sockets.clear();

    for (const PendingPromises &promises : std::as_const(pending))
    {
        for (const PendingPromise &promise : promises)
        {
            forgetTerrainWaiter(promise);
            promise->addResult(QHttpServerResponse(
                "Map server shutting down",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }
    }
}

QFuture<QHttpServerResponse> Server::submitTerrainJob(std::function<QHttpServerResponse()> task)
{
    if (this->shutting_down || !this->terrain_admission.tryAcquire())
    {
        QHttpServerResponse response(
            "Terrain service busy; retry later",
            QHttpServerResponse::StatusCode::ServiceUnavailable);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QHttpHeaders headers = response.headers();
        headers.replaceOrAppend("Retry-After", "1");
        response.setHeaders(std::move(headers));
#else
        response.setHeader("Retry-After", "1");
#endif
        return makeReadyResponse(std::move(response));
    }

    return QtConcurrent::run(&this->terrain_worker_pool, [this, task = std::move(task)]() mutable
    {
        struct ReleaseOnExit
        {
            TerrainAdmission *admission;
            ~ReleaseOnExit() { admission->release(); }
        } release { &this->terrain_admission };
        return task();
    });
}

QFuture<QHttpServerResponse> Server::submitTerrainTile(
    const QString &dataset, int zoom, int tile_x, int tile_y,
    const QHttpServerRequest *client_request)
{
    // Called exclusively on the HTTP event-loop thread. Each HTTP request owns
    // a distinct promise, but identical requests share one terrain worker job.
    const QByteArray cascade_trace = client_request == nullptr ? QByteArray() :
        client_request->value("x-aowis-cascade-visited");
    const QString key = dataset + QLatin1Char('/') + QString::number(zoom) +
        QLatin1Char('/') + QString::number(tile_x) + QLatin1Char('/') +
        QString::number(tile_y);
    const bool existing = this->pending_terrain_tiles.contains(key);
    Aowis::Map::TerrainTileAddress address;
    address.zoom = zoom;
    address.x = quint32(tile_x);
    address.y = quint32(tile_y);
    // Only requests that may download take a remote slot; cached sources
    // and normalized tiles go straight to the CPU pool.
    const bool remote_job = this->terrain_data->tileNeedsRemoteAcquisition(dataset, address);
    const bool remote_admitted = existing || (!this->shutting_down &&
        this->pending_terrain_tile_count < this->config.maximum_pending_requests &&
        (!remote_job ||
        this->terrain_remote_admission.tryAcquire()));
    const bool overall_admitted = existing ||
        (remote_admitted && this->terrain_admission.tryAcquire());
    if (!existing && remote_job && remote_admitted && !overall_admitted)
        this->terrain_remote_admission.release();
    if (this->shutting_down ||
        this->pending_terrain_tile_count >= this->config.maximum_pending_requests ||
        !overall_admitted)
    {
        QHttpServerResponse response(
            "Terrain service busy; retry later",
            QHttpServerResponse::StatusCode::ServiceUnavailable);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QHttpHeaders headers = response.headers();
        headers.replaceOrAppend("Retry-After", "1");
        response.setHeaders(std::move(headers));
#else
        response.setHeader("Retry-After", "1");
#endif
        return makeReadyResponse(std::move(response));
    }

    PendingPromise promise = PendingPromise::create();
    promise->start();
    QFuture<QHttpServerResponse> future = promise->future();
    this->pending_terrain_tiles[key].append(promise);
    if (requestAcceptsTerrainZstd(client_request))
        this->terrain_zstd_waiters.insert(promise.data());
    attachTerrainWaiter(promise, client_request);
    ++this->pending_terrain_tile_count;
    if (existing)
        return future;
    const TerrainCancellation cancellation = std::make_shared<std::atomic_bool>(false);
    this->tile_cancellations.insert(key, cancellation);

    // Split missing-source acquisition from CPU normalization. The remote
    // worker *enqueues* CPU processing and returns; it never waits on it.
    // The CPU stage never downloads: remote jobs acquired their sources
    // already, and local jobs must not occupy a CPU worker with a transfer.
    const std::function<void()> process_tile = [this, key, dataset, address, cancellation]()
    {
        Aowis::Map::TerrainTileLookupResult result;
        try
        {
            if (!cancellation->load(std::memory_order_acquire))
                result = this->terrain_data->terrainTile(dataset, address, false);
        }
        catch (...)
        {
            result.status = Aowis::Map::TerrainTileLookupStatus::ProviderError;
            result.error_message = QStringLiteral("Terrain processing failed");
        }
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this, [this, key, cancellation, result = std::move(result)]() mutable
        {
            if (this->tile_cancellations.value(key) != cancellation)
                return;
            this->tile_cancellations.remove(key);
            const PendingPromises waiters = this->pending_terrain_tiles.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            bool compression_attempted = false;
            std::optional<QByteArray> compressed;
            for (const PendingPromise &waiter : waiters)
            {
                const bool wants_zstd = this->terrain_zstd_waiters.contains(waiter.data());
                forgetTerrainWaiter(waiter);
                if (result.status == Aowis::Map::TerrainTileLookupStatus::Ready)
                {
                    if (wants_zstd && !compression_attempted)
                    {
                        compressed = Aowis::Map::compressTerrainTileForTransfer(result.data);
                        compression_attempted = true;
                    }
                    const bool use_zstd = wants_zstd && compressed.has_value();
                    QHttpServerResponse response(
                        Aowis::Map::terrainTileMimeType().toUtf8(),
                        use_zstd ? compressed.value() : result.data);
                    setTerrainRevisionHeader(response);
                    setTerrainTransferHeaders(response, use_zstd);
                    waiter->addResult(std::move(response));
                }
                else
                    waiter->addResult(makeTerrainTileErrorResponse(result));
                waiter->finish();
            }
        }, Qt::QueuedConnection);
    };
    if (!remote_job)
    {
        this->terrain_worker_pool.start(process_tile);
        return future;
    }
    this->terrain_remote_pool.start([this, key, dataset, address, process_tile, cancellation, cascade_trace]()
    {
        Aowis::Map::TerrainProviderFetchResult acquired;
        try
        {
            acquired = this->terrain_data->acquireTileSources(dataset, address, cancellation, cascade_trace);
        }
        catch (...)
        {
            acquired.status = Aowis::Map::TerrainProviderFetchStatus::NetworkError;
            acquired.error_message = QStringLiteral("Terrain source acquisition failed");
        }
        this->terrain_remote_admission.release();
        if (acquired.status == Aowis::Map::TerrainProviderFetchStatus::Ready &&
            !cancellation->load(std::memory_order_acquire))
        {
            this->terrain_worker_pool.start(process_tile);
            return;
        }
        this->terrain_admission.release();
        Aowis::Map::TerrainTileLookupResult result;
        result.status = acquired.status == Aowis::Map::TerrainProviderFetchStatus::Cancelled ||
                        acquired.status == Aowis::Map::TerrainProviderFetchStatus::NetworkError
            ? Aowis::Map::TerrainTileLookupStatus::RemoteFetchError
            : Aowis::Map::TerrainTileLookupStatus::ProviderError;
        result.error_message = acquired.error_message;
        QMetaObject::invokeMethod(this, [this, key, cancellation, result = std::move(result)]() mutable
        {
            if (this->tile_cancellations.value(key) != cancellation)
                return;
            this->tile_cancellations.remove(key);
            const PendingPromises waiters = this->pending_terrain_tiles.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                forgetTerrainWaiter(waiter);
                waiter->addResult(makeTerrainTileErrorResponse(result));
                waiter->finish();
            }
        }, Qt::QueuedConnection);
    });
    return future;
}

QFuture<QHttpServerResponse> Server::submitTerrainElevation(
    const QString &dataset, double latitude_deg, double longitude_deg,
    std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum,
    const QHttpServerRequest *client_request)
{
    // Route handlers run on the server thread. Numeric keys avoid duplicating
    // work for equivalent textual representations of the same coordinate.
    const QString key = dataset + QLatin1Char('/') +
        QString::number(latitude_deg == 0.0 ? 0.0 : latitude_deg, 'g', 17) +
        QLatin1Char('/') +
        QString::number(longitude_deg == 0.0 ? 0.0 : longitude_deg, 'g', 17) +
        QLatin1Char('/') +
        (requested_vertical_datum.has_value()
            ? Aowis::Map::terrainVerticalDatumId(*requested_vertical_datum)
            : QStringLiteral("native"));
    const bool existing = this->pending_terrain_elevations.contains(key);
    const bool remote_job = this->terrain_data->elevationNeedsRemoteAcquisition(
        dataset, latitude_deg, longitude_deg);
    const bool remote_admitted = existing || (!this->shutting_down &&
        this->pending_terrain_tile_count < this->config.maximum_pending_requests &&
        (!remote_job || this->terrain_remote_admission.tryAcquire()));
    const bool overall_admitted = existing ||
        (remote_admitted && this->terrain_admission.tryAcquire());
    if (!existing && remote_job && remote_admitted && !overall_admitted)
        this->terrain_remote_admission.release();
    if (this->shutting_down ||
        this->pending_terrain_tile_count >= this->config.maximum_pending_requests ||
        !overall_admitted)
    {
        QHttpServerResponse response(
            "Terrain service busy; retry later",
            QHttpServerResponse::StatusCode::ServiceUnavailable);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QHttpHeaders headers = response.headers();
        headers.replaceOrAppend("Retry-After", "1");
        response.setHeaders(std::move(headers));
#else
        response.setHeader("Retry-After", "1");
#endif
        return makeReadyResponse(std::move(response));
    }

    PendingPromise promise = PendingPromise::create();
    promise->start();
    QFuture<QHttpServerResponse> future = promise->future();
    this->pending_terrain_elevations[key].append(promise);
    attachTerrainWaiter(promise, client_request);
    ++this->pending_terrain_tile_count;
    if (existing)
        return future;
    const TerrainCancellation cancellation = std::make_shared<std::atomic_bool>(false);
    this->elevation_cancellations.insert(key, cancellation);

    const std::function<void()> process_elevation =
        [this, key, dataset, latitude_deg, longitude_deg, requested_vertical_datum, cancellation]()
    {
        Aowis::Map::TerrainElevationLookupResult result;
        try
        {
            if (!cancellation->load(std::memory_order_acquire))
                result = this->terrain_data->sampleElevation(
                    dataset, latitude_deg, longitude_deg, requested_vertical_datum, false);
        }
        catch (...)
        {
            result.status = Aowis::Map::TerrainElevationLookupStatus::ProviderError;
            result.error_message = QStringLiteral("Terrain processing failed");
        }
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this, [this, key, cancellation, result = std::move(result)]() mutable
        {
            if (this->elevation_cancellations.value(key) != cancellation)
                return;
            this->elevation_cancellations.remove(key);
            const PendingPromises waiters = this->pending_terrain_elevations.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                forgetTerrainWaiter(waiter);
                waiter->addResult(makeTerrainElevationSuccessResponse(result));
                waiter->finish();
            }
        }, Qt::QueuedConnection);
    };
    if (!remote_job)
    {
        this->terrain_worker_pool.start(process_elevation);
        return future;
    }
    this->terrain_remote_pool.start(
        [this, key, dataset, latitude_deg, longitude_deg, process_elevation, cancellation]()
    {
        Aowis::Map::TerrainProviderFetchResult acquired;
        try
        {
            acquired = this->terrain_data->acquireElevationSources(
                dataset, latitude_deg, longitude_deg, cancellation);
        }
        catch (...)
        {
            acquired.status = Aowis::Map::TerrainProviderFetchStatus::NetworkError;
            acquired.error_message = QStringLiteral("Elevation source acquisition failed");
        }
        this->terrain_remote_admission.release();
        if (acquired.status == Aowis::Map::TerrainProviderFetchStatus::Ready &&
            !cancellation->load(std::memory_order_acquire))
        {
            this->terrain_worker_pool.start(process_elevation);
            return;
        }
        this->terrain_admission.release();
        Aowis::Map::TerrainElevationLookupResult result;
        result.status = acquired.status == Aowis::Map::TerrainProviderFetchStatus::NetworkError ||
                        acquired.status == Aowis::Map::TerrainProviderFetchStatus::Cancelled
            ? Aowis::Map::TerrainElevationLookupStatus::RemoteFetchError
            : Aowis::Map::TerrainElevationLookupStatus::ProviderError;
        result.error_message = acquired.error_message;
        QMetaObject::invokeMethod(this, [this, key, cancellation, result = std::move(result)]() mutable
        {
            if (this->elevation_cancellations.value(key) != cancellation)
                return;
            this->elevation_cancellations.remove(key);
            const PendingPromises waiters = this->pending_terrain_elevations.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                forgetTerrainWaiter(waiter);
                waiter->addResult(makeTerrainElevationSuccessResponse(result));
                waiter->finish();
            }
        }, Qt::QueuedConnection);
    });
    return future;
}

QFuture<QHttpServerResponse> Server::submitTerrainGrid(
    const Aowis::Map::TerrainAnalysisGridRequest &request,
    const QHttpServerRequest *client_request)
{
    // Values have already passed HTTP validation. Canonical numeric keys
    // coalesce equivalent query representations without merging distinct grids.
    const auto number = [](double value)
    {
        return QString::number(value == 0.0 ? 0.0 : value, 'g', 17);
    };
    const QString key = request.dataset + QLatin1Char('/') +
        number(request.bounds.west_deg) + QLatin1Char('/') +
        number(request.bounds.south_deg) + QLatin1Char('/') +
        number(request.bounds.east_deg) + QLatin1Char('/') +
        number(request.bounds.north_deg) + QLatin1Char('/') +
        QString::number(request.columns) + QLatin1Char('/') +
        QString::number(request.rows);
    const bool existing = this->pending_terrain_grids.contains(key);
    // Reserve both the float result and the serialized response payload for
    // each unique grid until HTTP completion. The two buffers coexist while
    // serializing, so reserving only the float output undercounts peak use.
    const std::uint64_t estimated_bytes =
        std::uint64_t(request.columns) * std::uint64_t(request.rows) *
        (sizeof(float) + sizeof(float));
    const bool remote_job = this->terrain_data->gridNeedsRemoteAcquisition(request);
    bool admitted = existing;
    if (!existing && !this->shutting_down &&
        this->pending_terrain_tile_count < this->config.maximum_pending_requests &&
        this->terrain_grid_bytes.tryAcquire(estimated_bytes))
    {
        const bool remote_admitted = !remote_job ||
            this->terrain_remote_admission.tryAcquire();
        admitted = remote_admitted && this->terrain_admission.tryAcquire();
        if (!admitted)
        {
            if (remote_job && remote_admitted)
                this->terrain_remote_admission.release();
            this->terrain_grid_bytes.release(estimated_bytes);
        }
    }
    if (this->shutting_down ||
        this->pending_terrain_tile_count >= this->config.maximum_pending_requests ||
        !admitted)
    {
        QHttpServerResponse response(
            "Terrain service busy; retry later",
            QHttpServerResponse::StatusCode::ServiceUnavailable);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QHttpHeaders headers = response.headers();
        headers.replaceOrAppend("Retry-After", "1");
        response.setHeaders(std::move(headers));
#else
        response.setHeader("Retry-After", "1");
#endif
        return makeReadyResponse(std::move(response));
    }

    PendingPromise promise = PendingPromise::create();
    promise->start();
    QFuture<QHttpServerResponse> future = promise->future();
    this->pending_terrain_grids[key].append(promise);
    attachTerrainWaiter(promise, client_request);
    ++this->pending_terrain_tile_count;
    if (existing)
        return future;
    const TerrainCancellation cancellation = std::make_shared<std::atomic_bool>(false);
    this->grid_cancellations.insert(key, cancellation);

    const std::function<void()> process_grid = [this, key, request, estimated_bytes, cancellation]()
    {
        Aowis::Map::TerrainAnalysisGridResult result;
        try
        {
            if (!cancellation->load(std::memory_order_acquire))
                result = this->terrain_data->analysisGrid(request, false);
        }
        catch (...)
        {
            result.status = Aowis::Map::TerrainProviderFetchStatus::ConversionError;
            result.error_message = QStringLiteral("Terrain processing failed");
        }
        // Serialize once for all waiting responses. Input dimensions are
        // bounded by the route to 512x512 (at most 1 MiB of floats).
        QByteArray bytes;
        if (result.status == Aowis::Map::TerrainProviderFetchStatus::Ready)
        {
            bytes.resize(result.elevations_m.size() * qsizetype(sizeof(float)));
            for (qsizetype index = 0; index < result.elevations_m.size(); ++index)
            {
                const float value = result.elevations_m.at(index);
                quint32 bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                qToLittleEndian(bits, bytes.data() + index * qsizetype(sizeof(float)));
            }
        }
        const Aowis::Map::TerrainProviderFetchStatus result_status = result.status;
        QString error_message = std::move(result.error_message);
        // Do not retain the float grid in the queued HTTP delivery closure.
        // The serialized bytes are sufficient for all single-flight waiters.
        result.elevations_m.clear();
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this,
            [this, key, estimated_bytes, cancellation, result_status,
             error_message = std::move(error_message), bytes = std::move(bytes)]() mutable
        {
            if (this->grid_cancellations.value(key) != cancellation)
            {
                this->terrain_grid_bytes.release(estimated_bytes);
                return;
            }
            this->grid_cancellations.remove(key);
            const PendingPromises waiters = this->pending_terrain_grids.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                forgetTerrainWaiter(waiter);
                if (result_status == Aowis::Map::TerrainProviderFetchStatus::Ready)
                {
                    waiter->addResult(QHttpServerResponse(
                        QByteArrayLiteral("application/vnd.aowis.dem-f32le"), bytes));
                }
                else
                {
                    const QHttpServerResponse::StatusCode status =
                        result_status == Aowis::Map::TerrainProviderFetchStatus::UnsupportedDataset ||
                        result_status == Aowis::Map::TerrainProviderFetchStatus::UnsupportedAddress
                        ? QHttpServerResponse::StatusCode::BadRequest :
                        result_status == Aowis::Map::TerrainProviderFetchStatus::SourceUnavailable
                        ? QHttpServerResponse::StatusCode::NotFound :
                        result_status == Aowis::Map::TerrainProviderFetchStatus::NetworkError
                        ? QHttpServerResponse::StatusCode::BadGateway :
                        QHttpServerResponse::StatusCode::ServiceUnavailable;
                    waiter->addResult(QHttpServerResponse(error_message, status));
                }
                waiter->finish();
            }
            this->terrain_grid_bytes.release(estimated_bytes);
        }, Qt::QueuedConnection);
    };
    if (!remote_job)
    {
        this->terrain_worker_pool.start(process_grid);
        return future;
    }
    this->terrain_remote_pool.start([this, key, request, estimated_bytes, process_grid, cancellation]()
    {
        Aowis::Map::TerrainProviderFetchResult acquired;
        try
        {
            acquired = this->terrain_data->acquireGridSources(request, cancellation);
        }
        catch (...)
        {
            acquired.status = Aowis::Map::TerrainProviderFetchStatus::NetworkError;
            acquired.error_message = QStringLiteral("Grid source acquisition failed");
        }
        this->terrain_remote_admission.release();
        if (acquired.status == Aowis::Map::TerrainProviderFetchStatus::Ready &&
            !cancellation->load(std::memory_order_acquire))
        {
            this->terrain_worker_pool.start(process_grid);
            return;
        }
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this,
            [this, key, estimated_bytes, cancellation, acquired = std::move(acquired)]() mutable
        {
            if (this->grid_cancellations.value(key) != cancellation)
            {
                this->terrain_grid_bytes.release(estimated_bytes);
                return;
            }
            this->grid_cancellations.remove(key);
            const PendingPromises waiters = this->pending_terrain_grids.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            const QHttpServerResponse::StatusCode status =
                acquired.status == Aowis::Map::TerrainProviderFetchStatus::UnsupportedDataset ||
                acquired.status == Aowis::Map::TerrainProviderFetchStatus::UnsupportedAddress
                ? QHttpServerResponse::StatusCode::BadRequest :
                acquired.status == Aowis::Map::TerrainProviderFetchStatus::SourceUnavailable
                ? QHttpServerResponse::StatusCode::NotFound :
                acquired.status == Aowis::Map::TerrainProviderFetchStatus::NetworkError
                ? QHttpServerResponse::StatusCode::BadGateway
                : QHttpServerResponse::StatusCode::ServiceUnavailable;
            for (const PendingPromise &waiter : waiters)
            {
                forgetTerrainWaiter(waiter);
                waiter->addResult(QHttpServerResponse(acquired.error_message, status));
                waiter->finish();
            }
            this->terrain_grid_bytes.release(estimated_bytes);
        }, Qt::QueuedConnection);
    });
    return future;
}

bool Server::start()
{
    if (this->config.require_api_key && this->config.api_key.isEmpty())
    {
        qCritical() << "The map server requires an API key, but authentication/api_key is empty";
        return false;
    }
    if (this->config.require_delete_api_key && this->config.delete_api_key.isEmpty())
    {
        qCritical() << "The map server requires a delete API key, but authentication/delete_api_key is empty";
        return false;
    }

    QDir cache_directory;
    if (this->config.cache_directory.trimmed().isEmpty()
        || !cache_directory.mkpath(this->config.cache_directory))
    {
        qCritical() << "Failed to create map tile cache directory:"
                    << this->config.cache_directory;
        return false;
    }

    const QFileInfo cache_info(this->config.cache_directory);
    if (!cache_info.isDir() || !cache_info.isWritable())
    {
        qCritical() << "Map tile cache directory is not writable:"
                    << this->config.cache_directory;
        return false;
    }

    QString terrain_error_message;
    if (!this->terrain_data->initialize(&terrain_error_message))
    {
        qCritical().noquote() << terrain_error_message;
        return false;
    }

    if (!this->tcp->listen(this->config.listen_address, this->config.port))
    {
        qCritical() << "Failed to listen on" << this->config.listen_address.toString()
                    << "port" << this->config.port << ':' << this->tcp->errorString();
        return false;
    }

    if (!this->http.bind(this->tcp))
    {
        qCritical() << "Failed to bind QHttpServer to QTcpServer";
        this->tcp->close();
        return false;
    }

    const QString read_authentication_status = this->config.api_key.isEmpty()
        ? QStringLiteral("read API-key authentication disabled")
        : QStringLiteral("read API-key authentication enabled");
    const QString delete_authentication_status = this->config.delete_api_key.isEmpty()
        ? QStringLiteral("delete API-key authentication disabled")
        : QStringLiteral("delete API-key authentication enabled");

    const Aowis::Map::TerrainData::StoragePaths terrain_paths = this->terrain_data->storagePaths();
    const QString terrain_status = !this->terrain_data->isEnabled()
        ? QStringLiteral("terrain subsystem disabled")
        : this->terrain_data->isRemoteFetchEnabled()
              ? QStringLiteral("terrain subsystem enabled with remote fetching")
              : QStringLiteral("terrain subsystem enabled in offline-only mode");

    qInfo() << "AOWIS map server listening on" << this->tcp->serverAddress().toString()
            << "port" << this->tcp->serverPort()
            << "with at most" << this->config.maximum_active_downloads << "active tile downloads and"
            << this->config.maximum_pending_requests << "pending HTTP tile requests"
            << "using cache directory" << this->config.cache_directory
            << terrain_status
            << (this->terrain_data->isEnabled()
                    ? QStringLiteral("terrain cache: %1, default dataset: %2")
                          .arg(terrain_paths.root, this->config.terrain_default_dataset)
                    : QString())
            << read_authentication_status << "and" << delete_authentication_status;
    this->provider_catalog->refresh();
    return true;
}

void Server::setupRoutes()
{
    this->http.route("/status", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request))
            return makeUnauthorizedResponse();

        const Aowis::Map::TerrainData::StoragePaths terrain_paths = this->terrain_data->storagePaths();
        const QString terrain_status = !this->terrain_data->isEnabled()
            ? QStringLiteral("disabled")
            : this->terrain_data->isRemoteFetchEnabled()
                  ? QStringLiteral("enabled, remote fetching enabled")
                  : QStringLiteral("enabled, offline-only");
        QString status = QStringLiteral("AOWIS map server running with Qt %1\nTerrain subsystem: %2")
                             .arg(QString::fromLatin1(QT_VERSION_STR), terrain_status);
        if (this->terrain_data->isEnabled())
        {
            status += QStringLiteral("\nTerrain cache: %1\nTerrain default dataset: %2")
                          .arg(terrain_paths.root, this->config.terrain_default_dataset);
        }

        return QHttpServerResponse(status, QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/status", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/upstream/v1/activity", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request))
            return makeUnauthorizedResponse();

        const MapTiles::UpstreamActivity tile_activity = this->maptiles->upstreamActivity();
        const Aowis::Map::TerrainUpstreamActivity terrain_activity =
            this->terrain_data->upstreamActivity();

        QJsonObject map_tiles;
        map_tiles.insert(QStringLiteral("active"), tile_activity.active);
        map_tiles.insert(QStringLiteral("queued"), tile_activity.queued);

        QJsonObject terrain;
        terrain.insert(QStringLiteral("active"), terrain_activity.active);
        terrain.insert(QStringLiteral("queued"), terrain_activity.queued);

        QJsonObject root;
        root.insert(QStringLiteral("map_tiles"), map_tiles);
        root.insert(QStringLiteral("terrain"), terrain);
        return QHttpServerResponse(
            QByteArrayLiteral("application/json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact),
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/upstream/v1/activity", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/upstream/v1/map-tiles", QHttpServerRequest::Method::Delete,
                     [this](const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request))
            return makeUnauthorizedResponse();

        this->maptiles->cancelUpstreamDownloads();
        return QHttpServerResponse(
            "Map tile upstream downloads canceled",
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/upstream/v1/map-tiles", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/upstream/v1/terrain", QHttpServerRequest::Method::Delete,
                     [this](const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request))
            return makeUnauthorizedResponse();

        this->terrain_data->cancelUpstreamDownloads();
        return QHttpServerResponse(
            "Terrain upstream downloads canceled",
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/upstream/v1/terrain", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Delete,
                     [this](const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                            int tile_y_min, int tile_y_max, const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request))
            return makeUnauthorizedResponse();

        const int deleted_count = this->maptiles->deleteTiles(
            provider, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);
        if (deleted_count == -1)
        {
            return QHttpServerResponse(
                "Invalid tile cache deletion request",
                QHttpServerResponse::StatusCode::BadRequest);
        }
        if (deleted_count < -1)
        {
            return QHttpServerResponse(
                "Failed to delete one or more cached tiles",
                QHttpServerResponse::StatusCode::InternalServerError);
        }

        return QHttpServerResponse(QString::number(deleted_count), QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int, int, int)
    {
        return makeOptionsResponse();
    });

    // Terrain cache deletion, the map-tile cache DELETE's shape per zoom
    // level: the normalized .aowterrain tiles of an XYZ range, or the
    // provider-native source data (downloaded DEM rasters) covering its
    // area. Both use the delete API key and run the file work outside the
    // HTTP event loop (a source file being downloaded is waited for briefly).
    this->http.route("/terrain/v1/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>",
                     QHttpServerRequest::Method::Delete,
                     [this](const QString &dataset, int zoom, int tile_x_min, int tile_x_max,
                            int tile_y_min, int tile_y_max,
                            const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isDeleteAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        return submitTerrainJob(
            [this, dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max]()
        {
            return makeTerrainCacheDeletionResponse(this->terrain_data->deleteNormalizedTiles(
                dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max));
        });
    });

    this->http.route("/terrain/v1/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>",
                     QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int, int, int)
    {
        return makeOptionsResponse();
    });

    this->http.route("/terrain/v1/source/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>",
                     QHttpServerRequest::Method::Delete,
                     [this](const QString &dataset, int zoom, int tile_x_min, int tile_x_max,
                            int tile_y_min, int tile_y_max,
                            const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isDeleteAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        return submitTerrainJob(
            [this, dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max]()
        {
            return makeTerrainCacheDeletionResponse(this->terrain_data->deleteSourceData(
                dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max));
        });
    });

    this->http.route("/terrain/v1/source/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>",
                     QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int, int, int)
    {
        return makeOptionsResponse();
    });

    // Read-only merged catalog for downstream MAP servers and GUI clients.
    // PUT /map/v1/providers remains local-only and never edits inherited rows.
    this->http.route("/map/v1/providers/available", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request)) return makeUnauthorizedResponse();
        if (!this->maptiles->acceptsCascadeTrace(
                request.value("x-aowis-cascade-visited")))
            return QHttpServerResponse("Cascade loop detected",
                QHttpServerResponse::StatusCode::Conflict);
        return QHttpServerResponse(QByteArrayLiteral("application/json"),
            this->provider_catalog->encodedAvailable(), QHttpServerResponse::StatusCode::Ok);
    });
    this->http.route("/map/v1/providers/available", QHttpServerRequest::Method::Options,
                     []() { return makeOptionsResponse(); });

    // Local enablement override for an inherited provider. Never writes to an
    // upstream and never creates a local provider definition.
    this->http.route("/map/v1/providers/inherited", QHttpServerRequest::Method::Put,
                     [this](const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request)) return makeUnauthorizedResponse();
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parse_error);
        if (request.body().size() > 1024 || parse_error.error != QJsonParseError::NoError ||
            !document.isObject())
            return QHttpServerResponse("Invalid provider enablement request",
                QHttpServerResponse::StatusCode::BadRequest);
        const QJsonObject object = document.object();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (QUuid(id).isNull() || !object.value(QStringLiteral("enabled")).isBool())
            return QHttpServerResponse("Missing provider ID or enabled boolean",
                QHttpServerResponse::StatusCode::BadRequest);
        bool inherited = false;
        for (const Aowis::Map::AvailableMapProvider &entry : this->provider_catalog->available())
            if (entry.provider.id == id && entry.inherited)
                inherited = true;
        if (!inherited)
            return QHttpServerResponse("Provider is not inherited or is unavailable",
                QHttpServerResponse::StatusCode::NotFound);
        if (this->config.managed_settings_root.isEmpty())
            return QHttpServerResponse("Managed MAP settings are unavailable",
                QHttpServerResponse::StatusCode::ServiceUnavailable);
        QString error_message;
        std::unique_ptr<Aowis::Map::SettingsRepository> repository =
            Aowis::Map::SettingsRepository::openAtOwnerRoot(
                QStringLiteral("server-map"), this->config.managed_settings_root,
                &error_message);
        if (!repository)
            return QHttpServerResponse(error_message,
                QHttpServerResponse::StatusCode::InternalServerError);
        QSet<QString> disabled = this->config.disabled_inherited_providers;
        if (object.value(QStringLiteral("enabled")).toBool()) disabled.remove(id);
        else disabled.insert(id);
        if (disabled == this->config.disabled_inherited_providers)
            return QHttpServerResponse("Inherited provider state unchanged",
                QHttpServerResponse::StatusCode::Ok);
        QString revision;
        if (!repository->saveDisabledInheritedProviders(
                disabled, this->config.inherited_provider_revision, &revision, &error_message))
        {
            const bool conflict = error_message.contains(QStringLiteral("revision conflict"));
            return QHttpServerResponse(error_message, conflict ?
                QHttpServerResponse::StatusCode::Conflict :
                QHttpServerResponse::StatusCode::InternalServerError);
        }
        this->config.inherited_provider_revision = revision;
        this->config.disabled_inherited_providers = disabled;
        this->maptiles->cancelUpstreamDownloads();
        this->provider_catalog->setDisabledInheritedProviders(disabled);
        return QHttpServerResponse("Inherited provider enabled state updated",
            QHttpServerResponse::StatusCode::Ok);
    });
    this->http.route("/map/v1/providers/inherited", QHttpServerRequest::Method::Options,
                     []() { return makeOptionsResponse(); });

    // Discoverable catalog of map layers. Only server-configured providers
    // are exposed; URLs and credentials stay on the server.
    this->http.route("/map/v1/providers", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request)) return makeUnauthorizedResponse();
        return QHttpServerResponse(QByteArrayLiteral("application/json"),
            Aowis::Map::encodeProviderCatalog(this->config.map_providers),
            QHttpServerResponse::StatusCode::Ok);
    });
    // As requested, provider editing currently has no authentication. Only
    // application-managed state is writable; administrator /etc is untouched.
    this->http.route("/map/v1/providers", QHttpServerRequest::Method::Put,
                     [this](const QHttpServerRequest &request)
    {
        QVector<Aowis::Map::MapProvider> proposed;
        QString error_message;
        // Share the exact provider schema and URL-template validation with the
        // MAP library. In particular, CyclOSM's {s} hostname placeholder must
        // be expanded before QUrl checks the host. Validating the raw URL here
        // used to reject an otherwise unchanged, built-in provider catalog.
        if (!Aowis::Map::decodeProviderCatalog(request.body(), this->config.map_providers,
                                                &proposed, &error_message))
            return QHttpServerResponse(error_message.toUtf8(),
                                       QHttpServerResponse::StatusCode::BadRequest);
        for (const Aowis::Map::MapProvider &provider : proposed)
        {
            if (provider.id.contains(QLatin1Char('{')) ||
                !this->config.download_coverage.hasProfile(provider.coverage))
                return QHttpServerResponse("Invalid provider UUID or coverage profile",
                                           QHttpServerResponse::StatusCode::BadRequest);
        }
        if (this->config.managed_settings_root.isEmpty())
            return QHttpServerResponse("Managed MAP settings are unavailable",
                                       QHttpServerResponse::StatusCode::ServiceUnavailable);
        std::unique_ptr<Aowis::Map::SettingsRepository> repository =
            Aowis::Map::SettingsRepository::openAtOwnerRoot(
                QStringLiteral("server-map"), this->config.managed_settings_root,
                &error_message);
        if (!repository)
            return QHttpServerResponse(error_message,
                                       QHttpServerResponse::StatusCode::InternalServerError);
        QString new_revision;
        if (!repository->saveProviders(proposed, this->config.provider_revision,
                                       &new_revision, &error_message))
        {
            // Never overwrite another editor's update with an out-of-date
            // in-memory catalog. Restart/reload before trying again.
            const bool conflict = error_message.contains(QStringLiteral("revision conflict"));
            return QHttpServerResponse(error_message, conflict ?
                QHttpServerResponse::StatusCode::Conflict :
                QHttpServerResponse::StatusCode::InternalServerError);
        }
        this->config.provider_revision = new_revision;
        this->maptiles->cancelUpstreamDownloads();
        this->config.map_providers = proposed;
        this->provider_catalog->setLocalProviders(this->config.map_providers);
        return QHttpServerResponse("Map providers saved and applied",
                                   QHttpServerResponse::StatusCode::Ok);
    });
    this->http.route("/map/v1/providers", QHttpServerRequest::Method::Options,
                     []() {return makeOptionsResponse();});

    // Static description of the served terrain datasets (native resolution,
    // generated zoom range, datum) and which one the elevation route and
    // renderers use by default. Renderers cap their terrain requests at a
    // dataset's maximum_zoom instead of hardcoding provider knowledge.
    this->http.route("/terrain/v1/datasets",
                     QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request))
            return makeUnauthorizedResponse();

        if (!this->terrain_data->isEnabled() || !this->terrain_data->isInitialized())
        {
            Aowis::Map::TerrainTileLookupResult unavailable_result;
            unavailable_result.status = this->terrain_data->isEnabled()
                ? Aowis::Map::TerrainTileLookupStatus::NotInitialized
                : Aowis::Map::TerrainTileLookupStatus::Disabled;
            unavailable_result.error_message = this->terrain_data->isEnabled()
                ? QStringLiteral("Terrain subsystem is not initialized")
                : QStringLiteral("Terrain subsystem is disabled");
            return makeTerrainTileErrorResponse(unavailable_result);
        }

        return QHttpServerResponse(
            QByteArrayLiteral("application/json"),
            Aowis::Map::encodeTerrainDatasetCatalogJson(
                this->terrain_data->datasets(), this->config.terrain_default_dataset),
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/terrain/v1/datasets",
                     QHttpServerRequest::Method::Options,
                     []()
    {
        return makeOptionsResponse();
    });

    // Source-grade DEM windows: identical values/semantics to TerrainData::analysisGrid().
    // No .aowterrain normalization or rendered-tile quantization is involved.
    this->http.route("/terrain/v1/analysis/grid",
                     QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());
        const QUrlQuery query(request.url());
        const QStringList names = { QStringLiteral("west"), QStringLiteral("south"),
                                    QStringLiteral("east"), QStringLiteral("north") };
        double values[4] = {0.0, 0.0, 0.0, 0.0};
        bool valid = true;
        for (int index = 0; index < 4; ++index)
        {
            const QString text = query.queryItemValue(names[index]);
            bool ok = false;
            values[index] = text.toDouble(&ok);
            valid = valid && ok && !text.isEmpty() && std::isfinite(values[index]);
        }
        bool columns_ok = false;
        bool rows_ok = false;
        const int columns = query.queryItemValue(QStringLiteral("columns")).toInt(&columns_ok);
        const int rows = query.queryItemValue(QStringLiteral("rows")).toInt(&rows_ok);
        valid = valid && columns_ok && rows_ok &&
            columns >= 1 && columns <= 512 && rows >= 1 && rows <= 512 &&
            values[0] >= -180.0 && values[2] <= 180.0 && values[0] < values[2] &&
            values[1] >= -85.0 && values[3] <= 85.0 && values[1] < values[3];
        if (!valid)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Invalid grid request; supply WGS84 west,south,east,north and 1..512 columns,rows",
                QHttpServerResponse::StatusCode::BadRequest));
        }
        Aowis::Map::TerrainAnalysisGridRequest grid_request;
        grid_request.dataset = query.queryItemValue(QStringLiteral("dataset"));
        if (grid_request.dataset.isEmpty())
            grid_request.dataset = this->config.terrain_default_dataset;
        grid_request.bounds = {values[0], values[1], values[2], values[3]};
        grid_request.columns = columns;
        grid_request.rows = rows;
        return submitTerrainGrid(grid_request, &request);
    });

    this->http.route("/terrain/v1/analysis/grid",
                     QHttpServerRequest::Method::Options,
                     []() { return makeOptionsResponse(); });

    this->http.route("/terrain/v1/elevation",
                     QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        const QUrlQuery query(request.url());
        const QString latitude_text = query.queryItemValue(QStringLiteral("latitude"));
        const QString longitude_text = query.queryItemValue(QStringLiteral("longitude"));
        bool latitude_ok = false;
        bool longitude_ok = false;
        const double latitude_deg = latitude_text.toDouble(&latitude_ok);
        const double longitude_deg = longitude_text.toDouble(&longitude_ok);
        if (!latitude_ok || !longitude_ok || latitude_text.isEmpty() || longitude_text.isEmpty())
        {
            Aowis::Map::TerrainElevationLookupResult invalid_result;
            invalid_result.status = Aowis::Map::TerrainElevationLookupStatus::InvalidCoordinate;
            invalid_result.error_message = QStringLiteral(
                "Terrain elevation requests require numeric latitude and longitude query parameters");
            return makeReadyResponse(makeTerrainElevationErrorResponse(invalid_result));
        }

        std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum;
        const QString vertical_datum_text =
            query.queryItemValue(QStringLiteral("vertical_datum")).trimmed().toLower();
        if (!vertical_datum_text.isEmpty() && vertical_datum_text != QStringLiteral("native"))
        {
            const std::optional<Aowis::Map::TerrainVerticalDatum> parsed_vertical_datum =
                Aowis::Map::terrainVerticalDatumFromId(vertical_datum_text);
            if (!parsed_vertical_datum.has_value() ||
                !Aowis::Map::isRequestableTerrainVerticalDatum(parsed_vertical_datum.value()))
            {
                Aowis::Map::TerrainElevationLookupResult invalid_result;
                invalid_result.status = Aowis::Map::TerrainElevationLookupStatus::InvalidVerticalDatum;
                invalid_result.error_message = QStringLiteral(
                    "vertical_datum must be native, wgs84-ellipsoid, egm96 or egm2008");
                return makeReadyResponse(makeTerrainElevationErrorResponse(invalid_result));
            }
            requested_vertical_datum = parsed_vertical_datum.value();
        }

        const QString dataset = this->config.terrain_default_dataset;
        return submitTerrainElevation(
            dataset, latitude_deg, longitude_deg, requested_vertical_datum, &request);
    });

    this->http.route("/terrain/v1/elevation",
                     QHttpServerRequest::Method::Options,
                     []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/terrain/v1/<arg>/<arg>/<arg>/<arg>.aowterrain",
                     QHttpServerRequest::Method::Get,
                     [this](const QString &dataset, int zoom, int tile_x, int tile_y,
                            const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        Aowis::Map::TerrainTileAddress address;
        address.zoom = zoom;
        if (tile_x >= 0)
            address.x = quint32(tile_x);
        if (tile_y >= 0)
            address.y = quint32(tile_y);

        if (tile_x < 0 || tile_y < 0)
        {
            Aowis::Map::TerrainTileLookupResult invalid_result;
            invalid_result.status = Aowis::Map::TerrainTileLookupStatus::InvalidAddress;
            invalid_result.error_message = QStringLiteral("Terrain tile X and Y must be non-negative");
            return makeReadyResponse(makeTerrainTileErrorResponse(invalid_result));
        }

        if (!this->maptiles->acceptsCascadeTrace(
                request.value("x-aowis-cascade-visited")))
        {
            return makeReadyResponse(QHttpServerResponse(
                "Invalid or cyclic MAP cascade",
                QHttpServerResponse::StatusCode::BadRequest));
        }
        return submitTerrainTile(dataset, zoom, tile_x, tile_y, &request);
    });

    this->http.route("/terrain/v1/<arg>/<arg>/<arg>/<arg>.aowterrain",
                     QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int)
    {
        return makeOptionsResponse();
    });

    this->http.route("/<arg>/<arg>/<arg>/<arg>.png", QHttpServerRequest::Method::Get,
                     [this](const QString &provider, int z, int x, int y,
                            const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        const QByteArray expected_source = this->maptiles->rasterSourceFingerprint(provider);
        const QByteArray claimed_source = request.value("x-aowis-raster-source");
        if (!claimed_source.isEmpty() && claimed_source != expected_source)
            return makeReadyResponse(QHttpServerResponse(
                "Raster provider source/style mismatch",
                QHttpServerResponse::StatusCode::Conflict));
        const QByteArray cascade_trace = request.value("x-aowis-cascade-visited");
        if (!this->maptiles->acceptsCascadeTrace(cascade_trace))
            return makeReadyResponse(QHttpServerResponse(
                "Invalid or cyclic MAP cascade",
                QHttpServerResponse::StatusCode::LoopDetected));

        const QString key = QString("%1_%2_%3_%4").arg(provider).arg(z).arg(x).arg(y);
        const QUrlQuery tile_query(request.url());
        const bool cache_first_delivery =
            tile_query.queryItemValue(QStringLiteral("delivery")).compare(
                QStringLiteral("cache-first"), Qt::CaseInsensitive) == 0
            || request.value("x-aowis-tile-delivery").compare(
                QByteArrayLiteral("cache-first"), Qt::CaseInsensitive) == 0;
        const int raster_quality = this->maptiles->allowsLossyRasterTransfer(provider)
            ? requestedRasterJpegQuality(request) : 0;
        const MapTiles::TileRequestResult tile_result = this->maptiles->getTileForHttp(provider, z, x, y, key, cascade_trace);

        if (tile_result.status == MapTiles::TileRequestStatus::Ready)
        {
            if (raster_quality == 0)
                return makeReadyResponse(makeRasterResponse(tile_result.data, expected_source));
            PendingPromise promise = PendingPromise::create();
            promise->start();
            QFuture<QHttpServerResponse> future = promise->future();
            completeRasterPromises({promise}, tile_result.data, expected_source, z, raster_quality);
            return future;
        }

        if (tile_result.status == MapTiles::TileRequestStatus::InvalidRequest)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Invalid map tile request",
                QHttpServerResponse::StatusCode::BadRequest));
        }

        if (tile_result.status == MapTiles::TileRequestStatus::Unavailable)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Map tile is unavailable from the configured sources",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
        }

        if (tile_result.status == MapTiles::TileRequestStatus::ServerBusy)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Map tile download queue is full",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
        }

        if (cache_first_delivery)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Map tile is being fetched",
                QHttpServerResponse::StatusCode::Accepted));
        }

        PendingPromise promise = PendingPromise::create();
        promise->start();
        QFuture<QHttpServerResponse> future = promise->future();
        if (!appendPendingPromise(key, promise))
        {
            promise->addResult(QHttpServerResponse(
                "Too many pending map tile requests",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }
        else if (raster_quality != 0)
            this->raster_transfer_waiters.insert(promise.data(), raster_quality);

        return future;
    });

    this->http.route("/<arg>/<arg>/<arg>/<arg>.png", QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int)
    {
        return makeOptionsResponse();
    });
}

bool Server::isReadAuthorized(const QHttpServerRequest &request) const
{
    if (this->config.api_key.isEmpty())
        return true;

    return requestContainsKey(request, this->config.api_key);
}

bool Server::isDeleteAuthorized(const QHttpServerRequest &request) const
{
    if (this->config.delete_api_key.isEmpty())
        return true;

    return requestContainsKey(request, this->config.delete_api_key);
}

bool Server::requestContainsKey(const QHttpServerRequest &request, const QByteArray &expected_key) const
{
    const QByteArray direct_key = request.value("x-api-key");
    if (secureEquals(direct_key, expected_key))
        return true;

    const QByteArray authorization = request.value("authorization");
    static const QByteArray bearer_prefix("Bearer ");
    if (authorization.size() <= bearer_prefix.size()
        || authorization.first(bearer_prefix.size()).compare(bearer_prefix, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    return secureEquals(authorization.sliced(bearer_prefix.size()), expected_key);
}

bool Server::appendPendingPromise(const QString &key, const PendingPromise &promise)
{
    QMutexLocker locker(&this->mutex_pending);
    if (this->shutting_down ||
        this->pending_request_count >= this->config.maximum_pending_requests)
        return false;

    this->connections_pending[key].append(promise);
    ++this->pending_request_count;
    return true;
}

Server::PendingPromises Server::takePendingPromises(const QString &key)
{
    QMutexLocker locker(&this->mutex_pending);
    PendingPromises promises = this->connections_pending.take(key);
    this->pending_request_count -= promises.size();
    return promises;
}

void Server::completeRasterPromises(const PendingPromises &promises, const QByteArray &data,
                                    const QByteArray &source, int zoom, int requested_quality)
{
    if (promises.isEmpty()) return;
    if (requested_quality == 0 || mapTileMimeType(data) != "image/png" || data.size() < 12 * 1024)
    {
        for (const PendingPromise &promise : promises)
        {
            promise->addResult(makeRasterResponse(data, source));
            promise->finish();
        }
        return;
    }

    const std::optional<Aowis::Map::RasterTransferOptimizer::Selection> cached =
        this->raster_transfer.lookup(data, requested_quality);
    if (cached.has_value())
    {
        for (const PendingPromise &promise : promises)
        {
            this->raster_transfer.recordTransfer(data.size(), cached->bytes.size());
            promise->addResult(makeRasterResponse(cached->bytes, source));
            promise->finish();
        }
        return;
    }

    // No unbounded queue. If both workers are occupied, send canonical bytes
    // immediately. In-progress identical encodes are also deduplicated by the
    // selector and cannot delay unrelated tile responses.
    if (!this->raster_encode_admission.tryAcquire())
    {
        for (const PendingPromise &promise : promises)
        {
            this->raster_transfer.recordTransfer(data.size(), data.size());
            promise->addResult(makeRasterResponse(data, source));
            promise->finish();
        }
        return;
    }

    this->raster_encode_pool.start([this, promises, data, source, zoom, requested_quality]()
    {
        const Aowis::Map::RasterTransferOptimizer::Selection choice =
            this->raster_transfer.select(data, source, zoom, requested_quality,
                                         this->config.cascade.adaptive_compression);
        for (const PendingPromise &promise : promises)
        {
            this->raster_transfer.recordTransfer(data.size(), choice.bytes.size());
            promise->addResult(makeRasterResponse(choice.bytes, source));
            promise->finish();
        }
        this->raster_encode_admission.release();
    });
}

void Server::onTileReady(QString key, QByteArray data)
{
    PendingPromises promises = takePendingPromises(key);
    const QString provider = key.section(QLatin1Char('_'), 0, 0);
    const QByteArray source = this->maptiles->rasterSourceFingerprint(provider);
    const int zoom = key.section(QLatin1Char('_'), 1, 1).toInt();
    QHash<int, PendingPromises> groups;
    for (const PendingPromise &promise : promises)
    {
        const int quality = this->raster_transfer_waiters.take(promise.data());
        groups[quality].append(promise);
    }
    for (QHash<int, PendingPromises>::const_iterator group = groups.constBegin();
         group != groups.constEnd(); ++group)
        completeRasterPromises(group.value(), data, source, zoom, group.key());
}

void Server::onTileFailed(const QString &key, MapTiles::TileFailureReason reason)
{
    PendingPromises promises = takePendingPromises(key);
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::BadGateway;
    QString message = QStringLiteral("Failed to download map tile");
    if (reason == MapTiles::TileFailureReason::Timeout)
    {
        status = QHttpServerResponse::StatusCode::GatewayTimeout;
        message = QStringLiteral("Map tile download timed out");
    }
    else if (reason == MapTiles::TileFailureReason::Cancelled)
    {
        status = QHttpServerResponse::StatusCode::ServiceUnavailable;
        message = QStringLiteral("Map tile upstream download canceled");
    }

    for (const PendingPromise &promise : promises)
    {
        this->raster_transfer_waiters.remove(promise.data());
        promise->addResult(QHttpServerResponse(message, status));
        promise->finish();
    }
}
