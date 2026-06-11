#include "rest_client.h"

RESTClient::RESTClient(const QString &url_base, QObject *parent)
    : QObject{parent},
    url_base(url_base)
{
    
}

void RESTClient::get(const QString &endpoint)
{
    QNetworkRequest req((this->url_base + endpoint));
    req.setRawHeader("User-Agent", "aowis-server-map/1.0 (https://github.com/aowis-org/AOWIS-SERVER-MAP)");
    //req.setRawHeader("Accept-Language", "en-DK");
    //req.setRawHeader("Accept-Encoding", "gzip, deflate, br");
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setRawHeader("Accept", "*/*");
    QNetworkReply *reply = this->network_manager.get(req);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleReply(reply);
    });
}

void RESTClient::post(const QString &endpoint, const QJsonObject &payload)
{
    QNetworkRequest req((this->url_base + endpoint));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    
    QJsonDocument doc(payload);
    QNetworkReply *reply = this->network_manager.post(req, doc.toJson());
    
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleReply(reply);
    });
}

void RESTClient::handleReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError)
    {
        emit requestError(reply->errorString());
        reply->deleteLater();
        
        return;
    }
    
    emit requestFinished(reply->readAll());
    reply->deleteLater();
}
