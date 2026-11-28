#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_dataset.h>
#include <aowis/map/terrain_tile.h>

#include "../src/lib/global_terrain_overview.h"
#include <QTemporaryDir>
#include <QFileInfo>
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

    // A complete-size local overview advertises z0..7 without enabling any
    // provider downloads or changing the ordinary normalized cache namespace.
    QTemporaryDir temp;
    TerrainData::Config overview_config;
    overview_config.cache_directory = temp.path();
    overview_config.remote_fetch_enabled = true;
    TerrainData overview_data(overview_config);
    QString overview_error;
    if (!require(temp.isValid() && overview_data.initialize(&overview_error),
                 "Cannot initialize overview test"))
        return 1;
    const TerrainTileAddress address{0, 0, 0};
    TerrainTile tile;
    tile.address = address;
    tile.dataset = GlobalTerrainOverview::Dataset;
    tile.nominal_resolution_m = 1855;
    tile.vertical_datum = TerrainVerticalDatum::Egm2008;
    tile.elevations_m.fill(1234.0f, TerrainTileSampleCount);
    const QByteArray encoded = encodeTerrainTile(tile);
    if (!require(encoded.size() == GlobalTerrainOverview::RecordBytes,
                 "Overview record size must match the canonical codec"))
        return 1;
    QFile pack(GlobalTerrainOverview::path(overview_data.storagePaths().offline_packages));
    QByteArray header("AOWCOAR1", 8);
    const quint32 zoom_le = qToLittleEndian(quint32(GlobalTerrainOverview::MaximumZoom));
    const quint32 size_le = qToLittleEndian(quint32(GlobalTerrainOverview::RecordBytes));
    header.append(reinterpret_cast<const char *>(&zoom_le), 4);
    header.append(reinterpret_cast<const char *>(&size_le), 4);
    // Sparse fixture: only the root is populated; other records deliberately
    // fail CRC/format validation. No massive fixture or upstream requests.
    if (!require(pack.open(QIODevice::WriteOnly) && pack.write(header) == header.size()
                 && pack.write(encoded) == encoded.size()
                 && pack.resize(GlobalTerrainOverview::PackBytes), "Cannot create overview fixture"))
        return 1;
    pack.close();
    if (!require(overview_data.datasets().first().minimum_zoom == 0
                 && overview_data.datasets().first().native_minimum_zoom == 8
                 && !overview_data.tileNeedsRemoteAcquisition(tile.dataset, address)
                 && overview_data.acquireTileSources(tile.dataset, address).status == TerrainProviderFetchStatus::Ready,
                 "Local overview should be advertised and bypass remote acquisition"))
        return 1;
    const TerrainTileLookupResult overview = overview_data.terrainTile(tile.dataset, address);
    if (!require(overview.status == TerrainTileLookupStatus::Ready && overview.data == encoded
                 && !QFileInfo::exists(overview_data.normalizedTilePath(tile.dataset, address)),
                 "Overview must be delivered verbatim without duplicating it in cache"))
        return 1;
    if (!require(overview_data.terrainTile(tile.dataset, {1, 0, 0}).status == TerrainTileLookupStatus::CorruptTile,
                 "Corrupt overview records must be rejected"))
        return 1;
    if (!require(overview_data.sampleElevation(tile.dataset, 0.0, 0.0, std::nullopt, false).status
                     != TerrainElevationLookupStatus::Ready,
                 "Visual overview must not supply engineering point elevations"))
        return 1;
    if (!require(pack.remove() && overview_data.datasets().first().minimum_zoom == 8,
                 "Absent overview must not advertise coarse levels"))
        return 1;

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
