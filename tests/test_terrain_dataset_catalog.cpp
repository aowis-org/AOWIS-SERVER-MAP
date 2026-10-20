#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_dataset.h>
#include <aowis/map/terrain_tile.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <iostream>

namespace
{
bool require(bool condition, const char *message)
{
    if (condition)
        return true;

    std::cerr << message << '\n';
    return false;
}
}

int main()
{
    using namespace Aowis::Map;

    if (!require(terrainSurfaceModelId(TerrainSurfaceModel::Dsm) == QStringLiteral("dsm") &&
                     terrainSurfaceModelId(TerrainSurfaceModel::Dtm) == QStringLiteral("dtm") &&
                     terrainSurfaceModelId(TerrainSurfaceModel::Unknown) ==
                         QStringLiteral("unknown"),
                 "Terrain surface model identifiers are not stable"))
    {
        return 1;
    }
    if (!require(terrainSurfaceModelFromId(QStringLiteral("dsm")) == TerrainSurfaceModel::Dsm &&
                     !terrainSurfaceModelFromId(QStringLiteral("bare")).has_value(),
                 "Terrain surface model identifiers do not round-trip"))
    {
        return 1;
    }

    // Provider knowledge is static: available without initialize().
    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = false;
    TerrainData terrain_data(config);
    const QVector<TerrainDatasetInfo> datasets = terrain_data.datasets();
    if (!require(datasets.size() == 1, "Expected exactly the Copernicus terrain dataset"))
        return 1;

    const TerrainDatasetInfo &copernicus = datasets.first();
    if (!require(copernicus.dataset == QStringLiteral("copernicus-glo30") &&
                     copernicus.provider == QStringLiteral("copernicus") &&
                     std::abs(copernicus.nominal_resolution_m - 30.0) < 0.001 &&
                     copernicus.vertical_datum == TerrainVerticalDatum::Egm2008 &&
                     copernicus.surface_model == TerrainSurfaceModel::Dsm &&
                     copernicus.minimum_zoom == 8 && copernicus.maximum_zoom == 14 &&
                     isValidTerrainDatasetInfo(copernicus),
                 "Copernicus terrain dataset description is incorrect"))
    {
        return 1;
    }

    // An invalid description is dropped from the catalog, not served.
    TerrainDatasetInfo invalid = copernicus;
    invalid.dataset = QStringLiteral("other");
    invalid.minimum_zoom = 15;
    if (!require(!isValidTerrainDatasetInfo(invalid),
                 "A dataset with minimum_zoom > maximum_zoom was accepted"))
    {
        return 1;
    }

    QVector<TerrainDatasetInfo> catalog_input = datasets;
    catalog_input.append(invalid);
    const QByteArray json =
        encodeTerrainDatasetCatalogJson(catalog_input, QStringLiteral("copernicus-glo30"));
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
    if (!require(parse_error.error == QJsonParseError::NoError && document.isObject(),
                 "Terrain dataset catalog is not a JSON object"))
    {
        return 1;
    }

    const QJsonObject root = document.object();
    const QJsonArray entries = root.value(QStringLiteral("datasets")).toArray();
    if (!require(root.value(QStringLiteral("default_dataset")).toString() ==
                         QStringLiteral("copernicus-glo30") &&
                     entries.size() == 1,
                 "Terrain dataset catalog root is incorrect"))
    {
        return 1;
    }

    const QJsonObject entry = entries.first().toObject();
    if (!require(entry.value(QStringLiteral("dataset")).toString() ==
                         QStringLiteral("copernicus-glo30") &&
                     entry.value(QStringLiteral("provider")).toString() ==
                         QStringLiteral("copernicus") &&
                     std::abs(entry.value(QStringLiteral("nominal_resolution_m")).toDouble() -
                              30.0) < 0.001 &&
                     entry.value(QStringLiteral("vertical_datum")).toString() ==
                         QStringLiteral("egm2008") &&
                     entry.value(QStringLiteral("surface_model")).toString() ==
                         QStringLiteral("dsm") &&
                     entry.value(QStringLiteral("minimum_zoom")).toInt(-1) == 8 &&
                     entry.value(QStringLiteral("maximum_zoom")).toInt(-1) == 14 &&
                     entry.value(QStringLiteral("tile_grid_size")).toInt(-1) ==
                         TerrainTileGridSize,
                 "Terrain dataset catalog entry is incorrect"))
    {
        return 1;
    }

    std::cout << "terrain dataset catalog test passed\n";
    return 0;
}
