#ifndef HTTP_CLIENT_TILEFETCH_H
#define HTTP_CLIENT_TILEFETCH_H

#include <QObject>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <QJsonDocument>
#include <QJsonObject>

class TileHttpClient : public QObject
{
    Q_OBJECT
public:
    explicit TileHttpClient(const QString &url_base, QObject *parent = nullptr);
    
    void get(const QString &endpoint);
    void post(const QString &endpoint, const QJsonObject &payload);
    
private:
    QNetworkAccessManager network_manager;
    QString url_base;
    
    void handleReply(QNetworkReply *reply);
    
signals:
    void requestFinished(const QByteArray &data);
    void requestError(const QString &error);
};

#endif // HTTP_CLIENT_TILEFETCH_H
