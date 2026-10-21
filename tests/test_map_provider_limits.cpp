#include <aowis/map/map_provider.h>
#include <aowis/map/maptiles.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char *description)
{
    if (!condition)
    {
        std::cerr << description << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QVector<Aowis::Map::MapProvider> defaults = Aowis::Map::defaultMapProviders();
    require(defaults.size() == 4, "Four built-in 2D providers expected");
    for (const Aowis::Map::MapProvider &provider : defaults)
        require(!QUuid(provider.id).isNull(), "Every provider must have a UUID");
    require(defaults.at(3).maximum_zoom == 17, "ArcGIS conservative zoom maximum changed");
    require(defaults.at(2).maximum_zoom == 17, "OpenTopoMap zoom maximum changed");
    require(defaults.at(1).maximum_zoom >= 19, "CyclOSM high-detail fallback unavailable");

    QTemporaryDir cache;
    require(cache.isValid(), "Temporary cache directory unavailable");
    MapTiles tiles(cache.path(), 2, 4);
    const QString arcgis = defaults.at(3).id;
    const MapTiles::TileRequestResult unavailable =
        tiles.getTile(arcgis, 18, 12345, 12345, QStringLiteral("overzoom"));
    require(unavailable.status == MapTiles::TileRequestStatus::InvalidRequest,
            "Overzoomed imagery must never be downloaded");

    // Already present local data is deliberately still served out of range.
    const QString path = QDir(cache.path()).filePath(
        QStringLiteral("maptiles/%1/18/12345").arg(arcgis));
    require(QDir().mkpath(path), "Cannot create local high-zoom fixture");
    QFile local(QDir(path).filePath(QStringLiteral("12345.png")));
    require(local.open(QIODevice::WriteOnly), "Cannot write local tile fixture");
    const QByteArray bytes("local-tile-contents");
    require(local.write(bytes) == bytes.size(), "Cannot write local fixture contents");
    local.close();
    const MapTiles::TileRequestResult available =
        tiles.getTile(arcgis, 18, 12345, 12345, QStringLiteral("cached"));
    require(available.status == MapTiles::TileRequestStatus::Ready && available.data == bytes,
            "Cached high-zoom tile must remain available");

    tiles.setRemoteFetchEnabled(false);
    const MapTiles::TileRequestResult offline =
        tiles.getTile(defaults.at(0).id, 8, 128, 128, QStringLiteral("offline"));
    require(offline.status == MapTiles::TileRequestStatus::InvalidRequest,
            "Disabled raster downloads must not start network requests");
    std::cout << "Map provider UUID and overzoom tests passed\n";
    return EXIT_SUCCESS;
}
