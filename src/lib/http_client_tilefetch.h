#ifndef HTTP_CLIENT_TILEFETCH_H
#define HTTP_CLIENT_TILEFETCH_H

#include <QObject>

#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
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
        TransientError,
        Timeout,
        InvalidResponse,
        Cancelled
    };
    Q_ENUM(RequestFailureReason)

    explicit TileHttpClient(QNetworkAccessManager *network_manager, const QString &url_base,
                            QObject *parent = nullptr);

    // Cascade requests are authenticated, source-bound, loop-tracked and
    // redirect-restricted. Normal provider downloads remain unchanged.
    void configureCascade(const QByteArray &api_key, const QByteArray &source_fingerprint,
                          const QByteArray &visited, bool poor_internet_mode = false,
                          int jpeg_quality = 75);
    void get(const QString &endpoint);
    void post(const QString &endpoint, const QJsonObject &payload);
    void cancel();
    // Cascade requests only, valid once requestFinished/requestError was
    // emitted: whether a MAP server answered (its X-AOWIS-Instance header
    // was present, whatever the HTTP status), and its instance ID. An
    // answered failure concerns one tile, not the upstream's reachability.
    bool upstreamAnswered() const { return this->upstream_answered; }
    QByteArray responderInstance() const { return this->responder_instance; }

private:
#ifdef Q_OS_WIN
    void getAttempt(const QString &endpoint, int attempt);
    void handleReply(QNetworkReply *reply, bool validate_tile_response,
                     const QString &endpoint = QString(), int attempt = 0);
#else
    void handleReply(QNetworkReply *reply, bool validate_tile_response);
#endif
    void recordResponder(QNetworkReply *reply);

    QNetworkAccessManager *network_manager;
    QString url_base;
    QNetworkReply *active_reply = nullptr;
    bool cancel_requested = false;
    bool cascade_request = false;
    QByteArray cascade_api_key;
    QByteArray cascade_source;
    QByteArray cascade_visited;
    bool cascade_poor_internet = false;
    int cascade_jpeg_quality = 75;
    bool upstream_answered = false;
    QByteArray responder_instance;
    void prepareRequest(QNetworkRequest *request) const;

signals:
    void requestFinished(const QByteArray &data, bool may_store);
    void requestError(RequestFailureReason reason, const QString &error);
};

#endif // HTTP_CLIENT_TILEFETCH_H
