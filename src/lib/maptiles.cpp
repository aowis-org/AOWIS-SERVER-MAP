#include "maptiles.h"

MapTiles::MapTiles(QObject *parent)
    : QObject{parent}
{
    // init tile cache location
    this->fscache_base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QByteArray MapTiles::getTile(QString provider, int z, int x, int y, QString key)
{
    QString path = QString("%1/%2/%3.png").arg(z).arg(x).arg(y);
    QString url = "";
    if (provider == "openstreetmap")
    {
        url = "https://tile.openstreetmap.org/";
        this->fscache_path = this->fscache_base + "/maptiles/openstreetmap/";
    }
    else if (provider == "osmcyclo")
    {
        url = "https://a.tile-cyclosm.openstreetmap.fr/cyclosm/";
        this->fscache_path = this->fscache_base + "/maptiles/osmcyclo/";
    }
    else if (provider == "opentopomap")
    {
        url = "https://tile.opentopomap.org/";
        this->fscache_path = this->fscache_base + "/maptiles/opentopomap/";
    }
    else if (provider == "arcgis")
    {
        path = QString("%1/%2/%3").arg(z).arg(y).arg(x);
        url = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/";
        this->fscache_path = this->fscache_base + "/maptiles/arcgis/";
    }
    QDir dir;
    if (!dir.mkpath(this->fscache_path)) {
        qWarning() << "Failed to create directory:" << this->fscache_path;
    }
    
    QString file_name = QString("%1/%2/%3.png").arg(z).arg(x).arg(y);    
    QString tile_path = QDir(this->fscache_path).filePath(file_name);
    
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
            getMapTile(url, path, tile_path, z, x, y, key);
            
            // return empty QByteArray to signal that download in progress
            return {};
        }
        
        QByteArray data = file.readAll();
        file.close();
        return data;
        
    } else {
        // File does NOT exist yet: Initiating Download
        qDebug() << "File does NOT exist:" << tile_path;
        
        getMapTile(url, path, tile_path, z, x, y, key);
        
        // return empty QByteArray to signal that download in progress
        return {};
    }
}

void MapTiles::getMapTile(QString url, QString path, QString tile_path, int z, int x, int y, QString key)
{
    // check if this tile is already being downloaded and only proceede if not yet
    {
        QMutexLocker locker(&this->downloads_mutex);
        if (this->downloads_active.contains(key))
            return;
        this->downloads_active.insert(key);
    }
    
    TileHttpClient *rest = new TileHttpClient(url, this);
    connect(rest, &TileHttpClient::requestFinished, this, [this, rest, tile_path, key](const QByteArray &data)
            {
                saveMapTile(data, tile_path);
                
                {
                    QMutexLocker locker(&this->downloads_mutex);
                    this->downloads_active.remove(key);
                }
                
                emit tileReady(key, data);
                
                rest->deleteLater();
            });
    connect(rest, &TileHttpClient::requestError, this, [this, rest, key](const QString &err)
            {
                qWarning() << "Tile request failed:" << key << err;
                
                {
                    QMutexLocker locker(&this->downloads_mutex);
                    this->downloads_active.remove(key);
                }
                
                emit tileFailed(key);
                
                rest->deleteLater();
            });
    rest->get(path);
}

void MapTiles::saveMapTile(const QByteArray &data, QString tile_path)
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
