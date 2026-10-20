#include <aowis/map/terrain_dataset.h>

#include <aowis/map/terrain_tile.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace Aowis::Map
{

QString terrainSurfaceModelId(TerrainSurfaceModel model)
{
    switch (model)
    {
        case TerrainSurfaceModel::Dsm:
            return QStringLiteral("dsm");
        case TerrainSurfaceModel::Dtm:
            return QStringLiteral("dtm");
        case TerrainSurfaceModel::Unknown:
        default:
            return QStringLiteral("unknown");
    }
}

std::optional<TerrainSurfaceModel> terrainSurfaceModelFromId(const QString &id)
{
    if (id == QStringLiteral("unknown"))
        return TerrainSurfaceModel::Unknown;
    if (id == QStringLiteral("dsm"))
        return TerrainSurfaceModel::Dsm;
    if (id == QStringLiteral("dtm"))
        return TerrainSurfaceModel::Dtm;
    return std::nullopt;
}

bool isValidTerrainDatasetInfo(const TerrainDatasetInfo &info)
{
    return isValidTerrainDatasetId(info.dataset) && !info.provider.isEmpty() &&
           std::isfinite(info.nominal_resolution_m) && info.nominal_resolution_m >= 0.0 &&
           isValidTerrainVerticalDatum(info.vertical_datum) &&
           info.minimum_zoom >= 0 && info.maximum_zoom <= TerrainTileMaximumZoom &&
           info.minimum_zoom <= info.maximum_zoom;
}

QByteArray encodeTerrainDatasetCatalogJson(const QVector<TerrainDatasetInfo> &datasets,
                                           const QString &default_dataset)
{
    QJsonArray entries;
    for (const TerrainDatasetInfo &info : datasets)
    {
        if (!isValidTerrainDatasetInfo(info))
            continue;

        QJsonObject entry;
        entry.insert(QStringLiteral("dataset"), info.dataset);
        entry.insert(QStringLiteral("provider"), info.provider);
        entry.insert(QStringLiteral("nominal_resolution_m"), info.nominal_resolution_m);
        entry.insert(QStringLiteral("vertical_datum"),
                     terrainVerticalDatumId(info.vertical_datum));
        entry.insert(QStringLiteral("surface_model"),
                     terrainSurfaceModelId(info.surface_model));
        entry.insert(QStringLiteral("minimum_zoom"), info.minimum_zoom);
        entry.insert(QStringLiteral("maximum_zoom"), info.maximum_zoom);
        entry.insert(QStringLiteral("tile_grid_size"), TerrainTileGridSize);
        entries.append(entry);
    }

    QJsonObject root;
    root.insert(QStringLiteral("default_dataset"), default_dataset);
    root.insert(QStringLiteral("datasets"), entries);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace Aowis::Map
