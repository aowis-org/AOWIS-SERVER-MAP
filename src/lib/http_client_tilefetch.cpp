#include "http_client_tilefetch.h"

#include <aowis/map/cascade_upstream_health.h>

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

namespace
{
#ifdef Q_OS_WIN
constexpr int transfer_timeout_ms = 20000;
constexpr int overall_timeout_ms = 30000;
constexpr int timeout_retry_count = 1;
constexpr int timeout_retry_delay_ms = 250;
#else
constexpr int overall_timeout_ms = 45000;
#endif
constexpr qsizetype maximum_tile_size = 10 * 1024 * 1024;

bool hasSupportedImageSignature(const QByteArray &data)
{
    static const QByteArray png_signature("\x89PNG\r\n\x1a\n", 8);

    const bool is_png = data.startsWith(png_signature);
    const bool is_jpeg = data.size() >= 3 &&
                         static_cast<unsigned char>(data[0]) == 0xff &&
                         static_cast<unsigned char>(data[1]) == 0xd8 &&
                         static_cast<unsigned char>(data[2]) == 0xff;
    const bool is_webp = data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP";
    return is_png || is_jpeg || is_webp;
}

// Abort while streaming once a tile exceeds the size limit, instead of
// buffering an arbitrarily large upstream body before checking it.
void limitTileReplySize(QNetworkReply *reply)
{
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply](qint64 bytes_received, qint64 bytes_total)
    {
        if ((bytes_received > maximum_tile_size || bytes_total > maximum_tile_size) &&
            !reply->property("aowis_too_large").toBool())
        {
            reply->setProperty("aowis_too_large", true);
            reply->abort();
        }
    });
}

QString normalizedContentType(QNetworkReply *reply)
{
    return reply->header(QNetworkRequest::ContentTypeHeader).toString()
        .section(';', 0, 0).trimmed().toLower();
}
}

TileHttpClient::TileHttpClient(QNetworkAccessManager *network_manager, const QString &url_base,
                               QObject *parent)
    : QObject(parent),
      network_manager(network_manager),
      url_base(url_base)
{
    Q_ASSERT(this->network_manager != nullptr);
}

void TileHttpClient::configureCascade(const QByteArray &api_key,
                                      const QByteArray &source_fingerprint,
                                      const QByteArray &visited, bool poor_internet_mode,
                                      int jpeg_quality)
{
    this->cascade_poor_internet = poor_internet_mode;
    this->cascade_jpeg_quality = jpeg_quality;
    this->cascade_request = true;
    this->cascade_api_key = api_key;
    this->cascade_source = source_fingerprint;
    this->cascade_visited = visited;
}

void TileHttpClient::prepareRequest(QNetworkRequest *request) const
{
    if (!this->cascade_request) return;
    request->setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                          QNetworkRequest::ManualRedirectPolicy);
    request->setTransferTimeout(10000);
    if (!this->cascade_api_key.isEmpty())
        request->setRawHeader("X-API-Key", this->cascade_api_key);
    request->setRawHeader("X-AOWIS-Raster-Source", this->cascade_source);
    request->setRawHeader("X-AOWIS-Cascade-Visited", this->cascade_visited);
    if (this->cascade_poor_internet)
        request->setRawHeader("X-AOWIS-Raster-Transfer",
                              "adaptive-jpeg;quality=" + QByteArray::number(this->cascade_jpeg_quality));
}

void TileHttpClient::recordResponder(QNetworkReply *reply)
{
    if (!this->cascade_request) return;
    const QByteArray instance = reply->rawHeader(Aowis::Map::CascadeInstanceHeader).trimmed();
    this->upstream_answered = Aowis::Map::isCascadeInstanceId(instance);
    this->responder_instance = this->upstream_answered ? instance : QByteArray();
}

void TileHttpClient::get(const QString &endpoint)
{
    this->cancel_requested = false;
#ifdef Q_OS_WIN
    getAttempt(endpoint, 0);
#else
    QNetworkRequest request(this->url_base + endpoint);
    request.setRawHeader("User-Agent", "aowis-server-map/1.0 (https://github.com/aowis-org/AOWIS-SERVER-MAP)");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setRawHeader("Accept", "*/*");
    prepareRequest(&request);

    QNetworkReply *reply = this->network_manager->get(request);
    this->active_reply = reply;
    limitTileReplySize(reply);
    QTimer::singleShot(overall_timeout_ms, reply, [reply]()
    {
        if (reply->isFinished())
            return;

        reply->setProperty("aowis_overall_timeout", true);
        reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        handleReply(reply, true);
    });
#endif
}

#ifdef Q_OS_WIN
void TileHttpClient::getAttempt(const QString &endpoint, int attempt)
{
    if (this->cancel_requested)
    {
        emit requestError(RequestFailureReason::Cancelled, QStringLiteral("Tile download canceled"));
        return;
    }

    QNetworkRequest request(this->url_base + endpoint);
    request.setRawHeader("User-Agent", "aowis-server-map/1.0 (https://github.com/aowis-org/AOWIS-SERVER-MAP)");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setRawHeader("Accept", "*/*");
    // Generic Windows tile timeout first: prepareRequest() then applies the
    // shorter cascade timeout, so a dead MAP upstream fails over after 10 s
    // like on the other platforms instead of after 20 s.
    request.setTransferTimeout(transfer_timeout_ms);
    prepareRequest(&request);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply *reply = this->network_manager->get(request);
    this->active_reply = reply;
    limitTileReplySize(reply);
    QTimer::singleShot(overall_timeout_ms, reply, [reply]()
    {
        if (reply->isFinished())
            return;

        reply->setProperty("aowis_overall_timeout", true);
        reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, endpoint, attempt]()
    {
        handleReply(reply, true, endpoint, attempt);
    });
}
#endif

void TileHttpClient::post(const QString &endpoint, const QJsonObject &payload)
{
    QNetworkRequest request(this->url_base + endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonDocument document(payload);
    QNetworkReply *reply = this->network_manager->post(request, document.toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        handleReply(reply, false);
    });
}

void TileHttpClient::cancel()
{
    this->cancel_requested = true;
    QPointer<QNetworkReply> reply(this->active_reply);
    if (reply.isNull())
        return;

    if (reply->thread() == QThread::currentThread())
    {
        if (!reply->isFinished())
            reply->abort();
        return;
    }

    QMetaObject::invokeMethod(reply.data(), [reply]()
    {
        if (!reply.isNull() && !reply->isFinished())
            reply->abort();
    }, Qt::QueuedConnection);
}

#ifdef Q_OS_WIN
void TileHttpClient::handleReply(QNetworkReply *reply, bool validate_tile_response,
                                 const QString &endpoint, int attempt)
{
    if (this->active_reply == reply)
        this->active_reply = nullptr;

    if (this->cancel_requested)
    {
        emit requestError(RequestFailureReason::Cancelled, QStringLiteral("Tile download canceled"));
        reply->deleteLater();
        return;
    }

    if (reply->property("aowis_too_large").toBool())
    {
        emit requestError(RequestFailureReason::InvalidResponse,
                          QStringLiteral("Tile response exceeds the maximum size of %1 bytes")
                              .arg(qlonglong(maximum_tile_size)));
        reply->deleteLater();
        return;
    }

    recordResponder(reply);
    const bool overall_timeout = reply->property("aowis_overall_timeout").toBool();
    const QNetworkReply::NetworkError network_error = reply->error();

    if (overall_timeout ||
        network_error == QNetworkReply::TimeoutError ||
        network_error == QNetworkReply::OperationCanceledError)
    {
        // A MAP upstream is not retried here: the cascade fails over to the
        // next source instead, and a retry would double the wait.
        if (validate_tile_response && attempt < timeout_retry_count && !this->cascade_request)
        {
            reply->deleteLater();
            QTimer::singleShot(timeout_retry_delay_ms, this, [this, endpoint, attempt]()
            {
                getAttempt(endpoint, attempt + 1);
            });
            return;
        }

        QString timeout_description;
        if (overall_timeout)
        {
            timeout_description = QStringLiteral("Overall tile request timed out after %1 ms")
                .arg(overall_timeout_ms);
        }
        else
        {
            timeout_description =
                QStringLiteral("Tile transfer timed out after %1 ms without data")
                    .arg(transfer_timeout_ms);
        }

        emit requestError(RequestFailureReason::Timeout, timeout_description);
        reply->deleteLater();
        return;
    }

    // Qt also reports HTTP 4xx/5xx statuses as network errors. For a MAP
    // upstream those are answers and are classified by status below; only
    // a request that got no HTTP response at all is a transport failure.
    if (network_error != QNetworkReply::NoError &&
        !(this->cascade_request &&
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()))
    {
        emit requestError(this->cascade_request ? RequestFailureReason::TransientError
                                                : RequestFailureReason::UpstreamError,
                          reply->errorString());
        reply->deleteLater();
        return;
    }
#else
void TileHttpClient::handleReply(QNetworkReply *reply, bool validate_tile_response)
{
    if (this->active_reply == reply)
        this->active_reply = nullptr;

    if (this->cancel_requested)
    {
        emit requestError(RequestFailureReason::Cancelled, QStringLiteral("Tile download canceled"));
        reply->deleteLater();
        return;
    }

    if (reply->property("aowis_too_large").toBool())
    {
        emit requestError(RequestFailureReason::InvalidResponse,
                          QStringLiteral("Tile response exceeds the maximum size of %1 bytes")
                              .arg(qlonglong(maximum_tile_size)));
        reply->deleteLater();
        return;
    }

    recordResponder(reply);
    const bool overall_timeout = reply->property("aowis_overall_timeout").toBool();
    if (overall_timeout || reply->error() == QNetworkReply::TimeoutError)
    {
        const QString timeout_description = overall_timeout
            ? QStringLiteral("Overall tile request timed out after %1 ms").arg(overall_timeout_ms)
            : reply->errorString();
        emit requestError(RequestFailureReason::Timeout, timeout_description);
        reply->deleteLater();
        return;
    }

    // Qt also reports HTTP 4xx/5xx statuses as network errors. For a MAP
    // upstream those are answers and are classified by status below; only
    // a request that got no HTTP response at all is a transport failure.
    if (reply->error() != QNetworkReply::NoError &&
        !(this->cascade_request &&
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()))
    {
        emit requestError(this->cascade_request ? RequestFailureReason::TransientError
                                                : RequestFailureReason::UpstreamError,
                          reply->errorString());
        reply->deleteLater();
        return;
    }
#endif

    const QVariant status_attribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!status_attribute.isValid())
    {
        emit requestError(RequestFailureReason::InvalidResponse,
                          QStringLiteral("Tile response has no HTTP status code"));
        reply->deleteLater();
        return;
    }

    const int status_code = status_attribute.toInt();
    // Cache lifetime and revalidation headers are deliberately ignored:
    // successfully stored tiles remain available until explicitly deleted.
    // Explicit no-store/private directives still prevent initial storage.
    const QByteArray cache_control = reply->rawHeader("Cache-Control").toLower();
    const bool may_store = !cache_control.contains("no-store") &&
                           !cache_control.contains("private");
    // A MAP-to-MAP tile must be a complete successful representation.
    // In particular, never treat 202 (still fetching) or 206 as image data.
    if (this->cascade_request && status_code != 200)
    {
        // An answer from a MAP server (busy, tile unavailable, its own source
        // failed, loop refused) concerns this tile only. A 5xx without the
        // MAP instance header came from something in between (e.g. a reverse
        // proxy that cannot reach MAP) and is an outage of that upstream.
        emit requestError(status_code >= 500 && !this->upstream_answered
                              ? RequestFailureReason::TransientError
                              : RequestFailureReason::UpstreamError,
                          QStringLiteral("MAP upstream returned HTTP %1").arg(status_code));
        reply->deleteLater();
        return;
    }
    if (status_code < 200 || status_code >= 300)
    {
        emit requestError(RequestFailureReason::UpstreamError,
                          QStringLiteral("Tile server returned HTTP %1").arg(status_code));
        reply->deleteLater();
        return;
    }

    const QByteArray data = reply->readAll();
    if (validate_tile_response && this->cascade_request)
    {
        bool length_ok = false;
        const qint64 expected_length = reply->rawHeader("Content-Length").toLongLong(&length_ok);
        if (length_ok && (expected_length < 0 || expected_length != data.size()))
        {
            emit requestError(RequestFailureReason::InvalidResponse,
                              QStringLiteral("Truncated MAP upstream raster tile response"));
            reply->deleteLater();
            return;
        }
    }
    if (validate_tile_response && this->cascade_request &&
        reply->rawHeader("X-AOWIS-Raster-Source") != this->cascade_source)
    {
        emit requestError(RequestFailureReason::InvalidResponse,
                          QStringLiteral("MAP upstream raster source/style mismatch or missing identity"));
        reply->deleteLater();
        return;
    }
    if (validate_tile_response)
    {
        if (data.isEmpty())
        {
            emit requestError(RequestFailureReason::InvalidResponse,
                              QStringLiteral("Tile server returned an empty response"));
            reply->deleteLater();
            return;
        }

        if (data.size() > maximum_tile_size)
        {
            emit requestError(RequestFailureReason::InvalidResponse,
                              QStringLiteral("Tile response exceeds the maximum size of %1 bytes")
                                  .arg(qlonglong(maximum_tile_size)));
            reply->deleteLater();
            return;
        }

        const QString content_type = normalizedContentType(reply);
        if (!content_type.isEmpty() && content_type != QStringLiteral("application/octet-stream") &&
            !content_type.startsWith(QStringLiteral("image/")))
        {
            emit requestError(RequestFailureReason::InvalidResponse,
                              QStringLiteral("Unexpected tile content type: %1").arg(content_type));
            reply->deleteLater();
            return;
        }

        if (!hasSupportedImageSignature(data))
        {
            emit requestError(RequestFailureReason::InvalidResponse,
                              QStringLiteral("Tile response is not a supported PNG, JPEG, or WebP image"));
            reply->deleteLater();
            return;
        }
    }

    emit requestFinished(data, may_store);
    reply->deleteLater();
}
