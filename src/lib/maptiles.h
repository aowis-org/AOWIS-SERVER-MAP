#ifndef MAPTILES_H
#define MAPTILES_H

#include <QObject>

#include <QStandardPaths>
#include <QDir>

#include <QMutex>
#include <QMutexLocker>

#include <QRandomGenerator>

#include "http_client_tilefetch.h"

#include <QDebug>

class MapTiles : public QObject
{
    Q_OBJECT
    
public:
    explicit MapTiles(QObject *parent = nullptr);
    
    QByteArray getTile(QString provider, int z, int x, int y, QString key);
    
private:
    QString fscache_base;
    QString fscache_path;
    
    QMutex downloads_mutex;
    QSet<QString> downloads_active;
    
    void getMapTile(QString url, QString path, QString tile_path, int z, int x, int y, QString key);
    void saveMapTile(const QByteArray &data, QString tile_path);
    
    QString domainRandomizer(QString url);
    
signals:
    void tileReady(QString key, QByteArray data);
    void tileFailed(const QString &key);
};

#endif // MAPTILES_H
