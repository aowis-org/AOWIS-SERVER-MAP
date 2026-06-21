#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QCoreApplication>
#include <QMutex>

#include <QFuture>
#include <QHttpServerRequest>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QHttpHeaders>
#include <QTcpServer>
#include <QHostAddress>

#include <QHash>
#include <QVector>

#include <variant>

#include "maptiles.h"

// QHttpServer is move only.
// Storing it in QList, QVector ... will always result in those things trying to copy during reallocation, which will break
// Therefore, we implement our own datastructure for this
struct PendingList {
    QPromise<QHttpServerResponse> **items = nullptr;
    int count = 0;
    int capacity = 0;
    
    void append(QPromise<QHttpServerResponse> *p) {
        if (count == capacity) {
            int newCap = capacity == 0 ? 4 : capacity * 2;
            auto **newItems = new QPromise<QHttpServerResponse>*[newCap];
            for (int i = 0; i < count; ++i)
                newItems[i] = items[i];
            delete[] items;
            items = newItems;
            capacity = newCap;
        }
        items[count++] = p;
    }
    
    ~PendingList() {
        delete[] items;
    }
};



class Server : public QObject
{
    Q_OBJECT
public:
    explicit Server(QCoreApplication *app, QObject *parent = nullptr);
    
private:
    QMutex mutex_pending;
    
    void setupRoutes();
    
    QCoreApplication *app;
    QHttpServer http;
    QTcpServer *tcp;
    
    MapTiles *maptiles;
    
    QHash<QString, PendingList> connections_pending;
    
signals:
    
private slots:
    void onTileReady(QString key, QByteArray data);
};

#endif // SERVER_H
