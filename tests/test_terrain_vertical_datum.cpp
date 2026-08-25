#include <aowis/map/terrain_data.h>

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>

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

bool writeTile(const Aowis::Map::TerrainData &terrain_data,
               const Aowis::Map::TerrainTile &tile,
               QString *error_message)
{
    const QString path = terrain_data.normalizedTilePath(tile.dataset, tile.address);
    const QFileInfo file_info(path);
    QDir directory;
    if (!directory.mkpath(file_info.absolutePath()))
    {
        *error_message = QStringLiteral("Failed to create terrain test directory");
        return false;
    }

    const QByteArray encoded = Aowis::Map::encodeTerrainTile(tile, error_message);
    if (encoded.isEmpty())
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        *error_message = file.errorString();
        return false;
    }
    if (file.write(encoded) != encoded.size())
    {
        *error_message = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit())
    {
        *error_message = file.errorString();
        return false;
    }
    return true;
}
}

int main()
{
    using namespace Aowis::Map;

    if (!require(terrainVerticalDatumAuthorityCode(TerrainVerticalDatum::Egm2008) ==
                     QStringLiteral("EPSG:3855"),
                 "EGM2008 authority code is incorrect"))
    {
        return 1;
    }
    if (!require(terrainVerticalDatumAuthorityCode(TerrainVerticalDatum::Egm96) ==
                     QStringLiteral("EPSG:5773"),
                 "EGM96 authority code is incorrect"))
    {
        return 1;
    }
    if (!require(terrainVerticalDatumAuthorityCode(TerrainVerticalDatum::Wgs84Ellipsoid) ==
                     QStringLiteral("EPSG:4979"),
                 "WGS84 ellipsoidal-height authority code is incorrect"))
    {
        return 1;
    }
    if (!require(terrainVerticalReference(TerrainVerticalDatum::Egm2008) ==
                     TerrainVerticalReference::Orthometric &&
                     terrainVerticalReference(TerrainVerticalDatum::Wgs84Ellipsoid) ==
                     TerrainVerticalReference::Ellipsoidal,
                 "Vertical-reference classification is incorrect"))
    {
        return 1;
    }
    if (!require(isRequestableTerrainVerticalDatum(TerrainVerticalDatum::Egm2008) &&
                     !isRequestableTerrainVerticalDatum(TerrainVerticalDatum::Unknown) &&
                     !isRequestableTerrainVerticalDatum(TerrainVerticalDatum::Local),
                 "Vertical-datum requestability policy is incorrect"))
    {
        return 1;
    }

    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(), "Failed to create vertical-datum test directory"))
        return 1;

    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = false;
    config.cache_directory = temporary_directory.path();
    TerrainData terrain_data(config);

    QString error_message;
    if (!require(terrain_data.initialize(&error_message), "Failed to initialize TerrainData"))
        return 1;

    TerrainTile tile;
    tile.address.zoom = 1;
    tile.address.x = 1;
    tile.address.y = 1;
    tile.dataset = QStringLiteral("datum-test");
    tile.nominal_resolution_m = 30.0;
    tile.vertical_datum = TerrainVerticalDatum::Egm96;
    tile.elevations_m.fill(123.5f, TerrainTileSampleCount);
    if (!require(writeTile(terrain_data, tile, &error_message),
                 "Failed to write vertical-datum test tile"))
    {
        return 1;
    }

    const double latitude_deg = -45.0;
    const double longitude_deg = 45.0;

    TerrainElevationLookupResult result =
        terrain_data.sampleElevation(tile.dataset, latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::Ready &&
                     result.sample.has_value() &&
                     result.sample->vertical_datum == TerrainVerticalDatum::Egm96 &&
                     result.sample->source_vertical_datum == TerrainVerticalDatum::Egm96,
                 "Native terrain lookup did not preserve the source vertical datum"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(tile.dataset, latitude_deg, longitude_deg,
                                          TerrainVerticalDatum::Egm96);
    if (!require(result.status == TerrainElevationLookupStatus::Ready &&
                     result.sample.has_value() &&
                     result.sample->vertical_datum == TerrainVerticalDatum::Egm96,
                 "Identity vertical-datum request failed"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(tile.dataset, latitude_deg, longitude_deg,
                                          TerrainVerticalDatum::Egm2008);
    if (!require(result.status ==
                     TerrainElevationLookupStatus::VerticalDatumConversionUnavailable &&
                     result.source_vertical_datum.has_value() &&
                     result.source_vertical_datum.value() == TerrainVerticalDatum::Egm96 &&
                     result.requested_vertical_datum.has_value() &&
                     result.requested_vertical_datum.value() == TerrainVerticalDatum::Egm2008 &&
                     !result.sample.has_value(),
                 "Cross-datum request did not fail explicitly without a transformation model"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(tile.dataset, latitude_deg, longitude_deg,
                                          TerrainVerticalDatum::Local);
    if (!require(result.status == TerrainElevationLookupStatus::InvalidVerticalDatum,
                 "Ambiguous local datum was accepted as a requested output datum"))
    {
        return 1;
    }

    TerrainElevationSample invalid_sample;
    invalid_sample.elevation_m = 1.0;
    invalid_sample.dataset = QStringLiteral("invalid-source-datum");
    invalid_sample.nominal_resolution_m = 1.0;
    invalid_sample.vertical_datum = TerrainVerticalDatum::Egm2008;
    invalid_sample.source_vertical_datum = TerrainVerticalDatum::Unknown;
    if (!require(!isValidTerrainElevationSample(invalid_sample),
                 "Sample claimed a known output datum from an unknown source datum"))
    {
        return 1;
    }

    return 0;
}
