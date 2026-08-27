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
        InvalidResponse,
        Cancelled
    };
    Q_ENUM(RequestFailureReason)

    explicit TileHttpClient(QNetworkAccessManager *network_manager, const QString &url_base,
                            QObject *parent = nullptr);

    void get(const QString &endpoint);
    void post(const QString &endpoint, const QJsonObject &payload);
    void cancel();

private:
#ifdef Q_OS_WIN
    void getAttempt(const QString &endpoint, int attempt);
    void handleReply(QNetworkReply *reply, bool validate_tile_response,
                     const QString &endpoint = QString(), int attempt = 0);
#else
    void handleReply(QNetworkReply *reply, bool validate_tile_response);
#endif

    QNetworkAccessManager *network_manager;
    QString url_base;
    QNetworkReply *active_reply = nullptr;
    bool cancel_requested = false;

signals:
    void requestFinished(const QByteArray &data);
    void requestError(RequestFailureReason reason, const QString &error);
};

#endif // HTTP_CLIENT_TILEFETCH_H
