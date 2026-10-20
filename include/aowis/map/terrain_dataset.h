#ifndef AOWIS_MAP_TERRAIN_DATASET_H
#define AOWIS_MAP_TERRAIN_DATASET_H

#include <aowis/map/terrain_elevation.h>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace Aowis::Map
{

// What the heights of a dataset describe. A DSM includes vegetation,
// buildings and infrastructure; a DTM is bare earth.
enum class TerrainSurfaceModel
{
    Unknown,
    Dsm,
    Dtm
};

QString terrainSurfaceModelId(TerrainSurfaceModel model);
std::optional<TerrainSurfaceModel> terrainSurfaceModelFromId(const QString &id);

// Static description of one terrain dataset a provider can serve. Renderers
// use it to decide which normalized tiles are worth requesting at all,
// instead of hardcoding provider knowledge.
struct TerrainDatasetInfo
{
    QString dataset;
    QString provider;
    double nominal_resolution_m = 0.0;
    TerrainVerticalDatum vertical_datum = TerrainVerticalDatum::Unknown;
    TerrainSurfaceModel surface_model = TerrainSurfaceModel::Unknown;

    // Normalized XYZ zoom range the provider generates. maximum_zoom is the
    // provider's native detail: the zoom whose 65x65 grid spacing is about
    // the source resolution. A finer tile would only resample the same
    // source samples, so a renderer never requests beyond it and subdivides
    // the maximum_zoom tile instead.
    int minimum_zoom = 0;
    int maximum_zoom = 0;
};

bool isValidTerrainDatasetInfo(const TerrainDatasetInfo &info);

// The dataset catalog as served by GET /terrain/v1/datasets:
//
//   {
//     "default_dataset": "copernicus-glo30",
//     "datasets": [
//       {
//         "dataset": "copernicus-glo30",
//         "provider": "copernicus",
//         "nominal_resolution_m": 30.0,
//         "vertical_datum": "egm2008",
//         "surface_model": "dsm",
//         "minimum_zoom": 8,
//         "maximum_zoom": 14,
//         "tile_grid_size": 65
//       }
//     ]
//   }
//
// One encoder for the HTTP route and in-process consumers (the GUI's
// standalone map service), so both see the identical document.
QByteArray encodeTerrainDatasetCatalogJson(const QVector<TerrainDatasetInfo> &datasets,
                                           const QString &default_dataset);

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_DATASET_H
