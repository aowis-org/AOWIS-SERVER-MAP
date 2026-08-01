#ifndef MAPTILES_H
#define MAPTILES_H

#include <QObject>

#include <QByteArray>
#include <QMutex>
#include <QSet>
#include <QString>

class MapTiles : public QObject
{
    Q_OBJECT

public:
    explicit MapTiles(QObject *parent = nullptr);

    QByteArray getTile(QString provider, int z, int x, int y, QString key);
    int deleteTiles(const QString &provider, int zoom, int tile_x_min, int tile_x_max, int tile_y_min, int tile_y_max);

private:
    QString providerCachePath(const QString &provider) const;
    QString providerUrl(const QString &provider) const;
    QString providerTilePath(const QString &provider, int zoom, int x, int y) const;
    QString canonicalTileKey(const QString &provider, int zoom, int x, int y) const;

    void getMapTile(const QString &url, const QString &path, const QString &tile_path,
                    const QString &canonical_key, const QString &response_key);
    void saveMapTile(const QByteArray &data, const QString &tile_path);

    QString domainRandomizer(const QString &url) const;

    QString fscache_base;

    QMutex downloads_mutex;
    QSet<QString> downloads_active;
    QSet<QString> downloads_invalidated;

signals:
    void tileReady(QString key, QByteArray data);
    void tileFailed(const QString &key);
};

#endif // MAPTILES_H
