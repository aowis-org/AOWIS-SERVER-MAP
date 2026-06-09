#include "maptiles.h"

MapTiles::MapTiles(QObject *parent)
    : QObject{parent}
{
    // init tile cache location
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    this->path_osm = base + "/maptiles/osm/";
    
    QDir dir;
    if (!dir.mkpath(this->path_osm)) {
        qWarning() << "Failed to create directory:" << this->path_osm;
    }
}

QByteArray MapTiles::getTile(QString provider, int z, int x, int y, QString key)
{
    QString file_name = QString("%1/%2/%3.png").arg(z).arg(x).arg(y);    
    QString tile_path = QDir(this->path_osm).filePath(file_name);
    
    qDebug() << tile_path;
    
    QFileInfo info(tile_path);
    if (info.exists() && info.isFile())
    {
        // File does exist already: returning it as QByteArray
        qDebug() << "File exists:" << tile_path;
        
        QFile file(tile_path);
        if (!file.open(QIODevice::ReadOnly))
        {
            // if for some reason the tile turns out to be not readable, download it again
            getOpenStreetMapTile(tile_path, z, x, y, key);
            
            // return empty QByteArray to signal that download in progress
            return {};
        }
        
        QByteArray data = file.readAll();
        file.close();
        return data;
        
    } else {
        // File does NOT exist yet: Initiating Download
        qDebug() << "File does NOT exist:" << tile_path;
        
        getOpenStreetMapTile(tile_path, z, x, y, key);
        
        // return empty QByteArray to signal that download in progress
        return {};
    }
}

void MapTiles::getOpenStreetMapTile(QString tile_path, int z, int x, int y, QString key)
{
    // check if this tile is already being downloaded and only proceede if not yet
    {
        QMutexLocker locker(&this->downloads_mutex);
        if (this->downloads_active.contains(key))
            return;
        this->downloads_active.insert(key);
    }
    
    RESTClient *rest = new RESTClient("https://tile.openstreetmap.org/", this);
    connect(rest, &RESTClient::requestFinished, this, [this, rest, tile_path, z, x, y, key](const QByteArray &data) {
        
        emit tileReady(key, data);
        
        saveOpenStreetMapTile(key, data, tile_path);
        
        QMutexLocker locker(&this->downloads_mutex);
        this->downloads_active.remove(key);
        
        rest->deleteLater();
    });
    connect(rest, &RESTClient::requestError, this, [this, rest, key](const QString &err) {
        qDebug() << "fail: " << err;
        
        QMutexLocker locker(&this->downloads_mutex);
        this->downloads_active.remove(key);
        
        rest->deleteLater();
    });
    QString path = QString("%1/%2/%3.png").arg(z).arg(x).arg(y);
    rest->get(path);
}

void MapTiles::saveOpenStreetMapTile(QString key, const QByteArray &data, QString tile_path)
{
    // create all dirs in path if not exist yet
    QFileInfo info(tile_path);
    QDir dir = info.dir();
    if (!dir.exists())
    {
        dir.mkpath(".");
    }
    
    // save tile
    QFile file(tile_path);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(data);
        file.close();
    } else {
        qDebug() << "Failed to save tile: " << tile_path;
    }
}

