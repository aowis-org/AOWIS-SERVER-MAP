#include "http_client_tilefetch.h"

#include <QTimer>

namespace
{
constexpr int transfer_timeout_ms = 15000;
constexpr int overall_timeout_ms = 45000;
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

void TileHttpClient::get(const QString &endpoint)
{
    QNetworkRequest request(this->url_base + endpoint);
    request.setRawHeader("User-Agent", "aowis-server-map/1.0 (https://github.com/aowis-org/AOWIS-SERVER-MAP)");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setRawHeader("Accept", "*/*");
    request.setTransferTimeout(transfer_timeout_ms);

    QNetworkReply *reply = this->network_manager->get(request);
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
}

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

void TileHttpClient::handleReply(QNetworkReply *reply, bool validate_tile_response)
{
    const bool overall_timeout = reply->property("aowis_overall_timeout").toBool();
    if (overall_timeout || reply->error() == QNetworkReply::TimeoutError)
    {
        emit requestError(RequestFailureReason::Timeout, reply->errorString());
        reply->deleteLater();
        return;
    }

    if (reply->error() != QNetworkReply::NoError)
    {
        emit requestError(RequestFailureReason::UpstreamError, reply->errorString());
        reply->deleteLater();
        return;
    }

    const QVariant status_attribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!status_attribute.isValid())
    {
        emit requestError(RequestFailureReason::InvalidResponse,
                          QStringLiteral("Tile response has no HTTP status code"));
        reply->deleteLater();
        return;
    }

    const int status_code = status_attribute.toInt();
    if (status_code < 200 || status_code >= 300)
    {
        emit requestError(RequestFailureReason::UpstreamError,
                          QStringLiteral("Tile server returned HTTP %1").arg(status_code));
        reply->deleteLater();
        return;
    }

    const QByteArray data = reply->readAll();
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

    emit requestFinished(data);
    reply->deleteLater();
}
