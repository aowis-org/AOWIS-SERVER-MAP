#include "server.h"

#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_dataset.h>

#include <QDebug>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QHttpHeaders>
#endif
#include <QThread>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QSet>
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
      tcp(new QTcpServer(this)),
      terrain_admission(qMax(1, config.maximum_terrain_jobs)),
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
              config.terrain_coverage,
              config.download_coverage
          },
          this)),
      pending_request_count(0)
{
    // Terrain work must not outlive the server or consume the global Qt pool.
    this->terrain_worker_pool.setObjectName(QStringLiteral("aowis-map-terrain"));
    this->terrain_worker_pool.setMaxThreadCount(
        qMax(1, qMin(config.maximum_terrain_workers, config.maximum_terrain_jobs)));
    this->maptiles->setRemoteFetchEnabled(config.map_remote_fetch_enabled);
    this->maptiles->setPermanentDataDirectory(config.permanent_data_directory);
    this->maptiles->configureProviders(config.map_providers, config.download_coverage,
                                       config.map_coverage);
    connect(this->maptiles, &MapTiles::tileReady, this, &Server::onTileReady);
    connect(this->maptiles, &MapTiles::tileFailed, this, &Server::onTileFailed);
    setupRoutes();
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
    }

    this->tcp->close();
    disconnect(this->maptiles, nullptr, this, nullptr);
    this->maptiles->cancelUpstreamDownloads();
    this->terrain_data->cancelUpstreamDownloads();
    // Worker lambdas capture this and access terrain_data. Finish all of them
    // before QObject child destruction starts; no new requests are accepted.
    this->terrain_worker_pool.waitForDone();

    // Any queued terrain-tile completion callbacks are canceled on shutdown.
    for (const PendingPromises &promises : std::as_const(this->pending_terrain_tiles))
    {
        for (const PendingPromise &promise : promises)
        {
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
            promise->addResult(QHttpServerResponse(
                "Map server shutting down",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }
    }
    this->pending_terrain_grids.clear();
    this->pending_terrain_tile_count = 0;

    for (const PendingPromises &promises : std::as_const(pending))
    {
        for (const PendingPromise &promise : promises)
        {
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
    const QString &dataset, int zoom, int tile_x, int tile_y)
{
    // Called exclusively on the HTTP event-loop thread. Each HTTP request owns
    // a distinct promise, but identical requests share one terrain worker job.
    const QString key = dataset + QLatin1Char('/') + QString::number(zoom) +
        QLatin1Char('/') + QString::number(tile_x) + QLatin1Char('/') +
        QString::number(tile_y);
    const bool existing = this->pending_terrain_tiles.contains(key);
    if (this->shutting_down ||
        this->pending_terrain_tile_count >= this->config.maximum_pending_requests ||
        (!existing && !this->terrain_admission.tryAcquire()))
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
    ++this->pending_terrain_tile_count;
    if (existing)
        return future;

    Aowis::Map::TerrainTileAddress address;
    address.zoom = zoom;
    address.x = quint32(tile_x);
    address.y = quint32(tile_y);
    this->terrain_worker_pool.start([this, key, dataset, address]()
    {
        Aowis::Map::TerrainTileLookupResult result;
        try
        {
            result = this->terrain_data->terrainTile(dataset, address);
        }
        catch (...)
        {
            result.status = Aowis::Map::TerrainTileLookupStatus::ProviderError;
            result.error_message = QStringLiteral("Terrain processing failed");
        }
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this, [this, key, result = std::move(result)]() mutable
        {
            const PendingPromises waiters = this->pending_terrain_tiles.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                if (result.status == Aowis::Map::TerrainTileLookupStatus::Ready)
                {
                    waiter->addResult(QHttpServerResponse(
                        Aowis::Map::terrainTileMimeType().toUtf8(), result.data));
                }
                else
                {
                    waiter->addResult(makeTerrainTileErrorResponse(result));
                }
                waiter->finish();
            }
        }, Qt::QueuedConnection);
    });
    return future;
}

QFuture<QHttpServerResponse> Server::submitTerrainElevation(
    const QString &dataset, double latitude_deg, double longitude_deg,
    std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum)
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
    if (this->shutting_down ||
        this->pending_terrain_tile_count >= this->config.maximum_pending_requests ||
        (!existing && !this->terrain_admission.tryAcquire()))
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
    ++this->pending_terrain_tile_count;
    if (existing)
        return future;

    this->terrain_worker_pool.start(
        [this, key, dataset, latitude_deg, longitude_deg, requested_vertical_datum]()
    {
        Aowis::Map::TerrainElevationLookupResult result;
        try
        {
            result = this->terrain_data->sampleElevation(
                dataset, latitude_deg, longitude_deg, requested_vertical_datum);
        }
        catch (...)
        {
            result.status = Aowis::Map::TerrainElevationLookupStatus::ProviderError;
            result.error_message = QStringLiteral("Terrain processing failed");
        }
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this, [this, key, result = std::move(result)]() mutable
        {
            const PendingPromises waiters = this->pending_terrain_elevations.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                waiter->addResult(makeTerrainElevationSuccessResponse(result));
                waiter->finish();
            }
        }, Qt::QueuedConnection);
    });
    return future;
}

QFuture<QHttpServerResponse> Server::submitTerrainGrid(
    const Aowis::Map::TerrainAnalysisGridRequest &request)
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
    // Reserve each unique grid's float output until its HTTP completion
    // callback has delivered the response. Prevents queued grid jobs from
    // accumulating more than 64 MiB of estimated output buffers.
    const std::uint64_t estimated_bytes =
        std::uint64_t(request.columns) * std::uint64_t(request.rows) * sizeof(float);
    bool admitted = existing;
    if (!existing && !this->shutting_down &&
        this->pending_terrain_tile_count < this->config.maximum_pending_requests &&
        this->terrain_grid_bytes.tryAcquire(estimated_bytes))
    {
        admitted = this->terrain_admission.tryAcquire();
        if (!admitted)
            this->terrain_grid_bytes.release(estimated_bytes);
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
    ++this->pending_terrain_tile_count;
    if (existing)
        return future;

    this->terrain_worker_pool.start([this, key, request, estimated_bytes]()
    {
        Aowis::Map::TerrainAnalysisGridResult result;
        try
        {
            result = this->terrain_data->analysisGrid(request);
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
        this->terrain_admission.release();
        QMetaObject::invokeMethod(this,
            [this, key, estimated_bytes, result = std::move(result), bytes = std::move(bytes)]() mutable
        {
            const PendingPromises waiters = this->pending_terrain_grids.take(key);
            this->pending_terrain_tile_count -= waiters.size();
            for (const PendingPromise &waiter : waiters)
            {
                if (result.status == Aowis::Map::TerrainProviderFetchStatus::Ready)
                {
                    waiter->addResult(QHttpServerResponse(
                        QByteArrayLiteral("application/vnd.aowis.dem-f32le"), bytes));
                }
                else
                {
                    const QHttpServerResponse::StatusCode status =
                        result.status == Aowis::Map::TerrainProviderFetchStatus::UnsupportedDataset ||
                        result.status == Aowis::Map::TerrainProviderFetchStatus::UnsupportedAddress
                        ? QHttpServerResponse::StatusCode::BadRequest :
                        result.status == Aowis::Map::TerrainProviderFetchStatus::SourceUnavailable
                        ? QHttpServerResponse::StatusCode::NotFound :
                        result.status == Aowis::Map::TerrainProviderFetchStatus::NetworkError
                        ? QHttpServerResponse::StatusCode::BadGateway :
                        QHttpServerResponse::StatusCode::ServiceUnavailable;
                    waiter->addResult(QHttpServerResponse(result.error_message, status));
                }
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

    // Discoverable catalog of map layers. Only server-configured providers
    // are exposed; URLs and credentials stay on the server.
    this->http.route("/map/v1/providers", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request)) return makeUnauthorizedResponse();
        QJsonArray providers;
        for (const Aowis::Map::MapProvider &provider : this->config.map_providers)
        {
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), provider.id);
            entry.insert(QStringLiteral("name"), provider.name);
            entry.insert(QStringLiteral("url"), provider.url_template);
            entry.insert(QStringLiteral("minimum_zoom"), provider.minimum_zoom);
            entry.insert(QStringLiteral("maximum_zoom"), provider.maximum_zoom);
            entry.insert(QStringLiteral("enabled"), provider.enabled);
            entry.insert(QStringLiteral("remote_fetch_enabled"), provider.remote_fetch_enabled);
            entry.insert(QStringLiteral("coverage"), provider.coverage);
            providers.append(entry);
        }
        QJsonObject root;
        root.insert(QStringLiteral("providers"), providers);
        return QHttpServerResponse(QByteArrayLiteral("application/json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact),
            QHttpServerResponse::StatusCode::Ok);
    });
    // As requested, provider editing currently has no authentication. Only
    // application-managed state is writable; administrator /etc is untouched.
    this->http.route("/map/v1/providers", QHttpServerRequest::Method::Put,
                     [this](const QHttpServerRequest &request)
    {
        const QJsonDocument document = QJsonDocument::fromJson(request.body());
        if (!document.isObject() || !document.object().value(QStringLiteral("providers")).isArray())
            return QHttpServerResponse("Expected {\"providers\": [...]}",
                                       QHttpServerResponse::StatusCode::BadRequest);
        const QJsonArray entries = document.object().value(QStringLiteral("providers")).toArray();
        if (entries.size() > 100)
            return QHttpServerResponse("Too many map providers",
                                       QHttpServerResponse::StatusCode::BadRequest);
        QVector<Aowis::Map::MapProvider> proposed;
        QSet<QString> ids;
        for (const QJsonValue &value : entries)
        {
            if (!value.isObject())
                return QHttpServerResponse("Provider must be an object",
                                           QHttpServerResponse::StatusCode::BadRequest);
            const QJsonObject entry = value.toObject();
            Aowis::Map::MapProvider provider;
            provider.id = entry.value(QStringLiteral("id")).toString().trimmed().toLower();
            provider.name = entry.value(QStringLiteral("name")).toString().trimmed();
            provider.url_template = entry.value(QStringLiteral("url")).toString().trimmed();
            provider.coverage = entry.value(QStringLiteral("coverage")).toString().trimmed();
            provider.minimum_zoom = entry.value(QStringLiteral("minimum_zoom")).toInt(-1);
            provider.maximum_zoom = entry.value(QStringLiteral("maximum_zoom")).toInt(-1);
            provider.enabled = entry.value(QStringLiteral("enabled")).toBool(true);
            provider.remote_fetch_enabled = entry.value(QStringLiteral("remote_fetch_enabled")).toBool(true);
            const QUrl parsed(provider.url_template);
            if (QUuid(provider.id).isNull() || provider.id.contains(QLatin1Char('{')) ||
                ids.contains(provider.id) || provider.name.isEmpty() ||
                provider.name.contains(QLatin1Char('\n')) || provider.name.contains(QLatin1Char('\r')) ||
                provider.url_template.contains(QLatin1Char('\n')) ||
                provider.url_template.contains(QLatin1Char('\r')) ||
                provider.coverage.contains(QLatin1Char('\n')) ||
                provider.minimum_zoom < 0 || provider.maximum_zoom > 30 ||
                provider.minimum_zoom > provider.maximum_zoom ||
                !this->config.download_coverage.hasProfile(provider.coverage) ||
                !(parsed.scheme() == QStringLiteral("http") || parsed.scheme() == QStringLiteral("https")) ||
                parsed.host().isEmpty() ||
                !provider.url_template.contains(QStringLiteral("{z}")) ||
                !provider.url_template.contains(QStringLiteral("{x}")) ||
                !provider.url_template.contains(QStringLiteral("{y}")))
            {
                return QHttpServerResponse("Invalid provider UUID, name, URL, coverage or zoom range",
                                           QHttpServerResponse::StatusCode::BadRequest);
            }
            ids.insert(provider.id);
            proposed.append(provider);
        }
        QString error_message;
        if (!MapServerConfiguration::saveMapProvidersToState(
                this->config.provider_state_file, proposed, &error_message))
        {
            return QHttpServerResponse(error_message,
                                       QHttpServerResponse::StatusCode::InternalServerError);
        }
        this->maptiles->cancelUpstreamDownloads();
        this->config.map_providers = proposed;
        this->maptiles->configureProviders(this->config.map_providers,
                                           this->config.download_coverage,
                                           this->config.map_coverage);
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
        return submitTerrainGrid(grid_request);
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
            dataset, latitude_deg, longitude_deg, requested_vertical_datum);
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

        return submitTerrainTile(dataset, zoom, tile_x, tile_y);
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

        const QString key = QString("%1_%2_%3_%4").arg(provider).arg(z).arg(x).arg(y);
        const QUrlQuery tile_query(request.url());
        const bool cache_first_delivery =
            tile_query.queryItemValue(QStringLiteral("delivery")).compare(
                QStringLiteral("cache-first"), Qt::CaseInsensitive) == 0
            || request.value("x-aowis-tile-delivery").compare(
                QByteArrayLiteral("cache-first"), Qt::CaseInsensitive) == 0;
        const MapTiles::TileRequestResult tile_result = this->maptiles->getTileForHttp(provider, z, x, y, key);

        if (tile_result.status == MapTiles::TileRequestStatus::Ready)
            return makeReadyResponse(QHttpServerResponse("image/png", tile_result.data));

        if (tile_result.status == MapTiles::TileRequestStatus::InvalidRequest)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Invalid map tile request",
                QHttpServerResponse::StatusCode::BadRequest));
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

void Server::onTileReady(QString key, QByteArray data)
{
    PendingPromises promises = takePendingPromises(key);
    for (const PendingPromise &promise : promises)
    {
        promise->addResult(QHttpServerResponse("image/png", data));
        promise->finish();
    }
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
        promise->addResult(QHttpServerResponse(message, status));
        promise->finish();
    }
}
