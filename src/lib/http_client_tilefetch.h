#ifndef HTTP_CLIENT_TILEFETCH_H
#define HTTP_CLIENT_TILEFETCH_H

#include <QObject>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

class TileHttpClient : public QObject
{
    Q_OBJECT

public:
    enum class RequestFailureReason
    {
        UpstreamError,
        Timeout,
        InvalidResponse
    };
    Q_ENUM(RequestFailureReason)

    explicit TileHttpClient(QNetworkAccessManager *network_manager, const QString &url_base,
                            QObject *parent = nullptr);

    void get(const QString &endpoint);
    void post(const QString &endpoint, const QJsonObject &payload);

private:
    void handleReply(QNetworkReply *reply, bool validate_tile_response);

    QNetworkAccessManager *network_manager;
    QString url_base;

signals:
    void requestFinished(const QByteArray &data);
    void requestError(RequestFailureReason reason, const QString &error);
};

#endif // HTTP_CLIENT_TILEFETCH_H
