#ifndef AOWIS_MAP_DOWNLOAD_COVERAGE_H
#define AOWIS_MAP_DOWNLOAD_COVERAGE_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Aowis::Map
{
// These limits apply exclusively to upstream acquisition. Cached or imported
// data is never hidden by a coverage profile.
class DownloadCoverage
{
public:
    struct Bounds
    {
        double west = -180.0;
        double south = -85.05112878;
        double east = 180.0;
        double north = 85.05112878;
    };
    struct Tier
    {
        int minimum_zoom = 0;
        int maximum_zoom = 30;
        QStringList regions;
    };
    struct Profile
    {
        QVector<Tier> tiers;
    };

    bool addRectangle(const QString &name, const Bounds &bounds, QString *error);
    bool addGeoJsonRegions(const QString &file_path, QString *error);
    bool addProfile(const QString &name, const Profile &profile, QString *error);
    bool hasRegion(const QString &name) const;
    bool hasProfile(const QString &name) const;
    bool permitsTile(const QString &profile, int zoom, int x, int y) const;
    // The zoom tier's rule applied to an arbitrary WGS84 rectangle (west <= east):
    // permitted when any part of it intersects an allowed region.
    bool permitsArea(const QString &profile, int zoom, const Bounds &area) const;
    QStringList profileNames() const;

private:
    struct Point { double lon = 0.0; double lat = 0.0; };
    struct Polygon { QVector<QVector<Point>> rings; Bounds envelope; };
    struct Region { QVector<Bounds> rectangles; QVector<Polygon> polygons; };
    QHash<QString, Region> regions;
    QHash<QString, Profile> profiles;

    static bool intersects(const Bounds &left, const Bounds &right);
    static bool intersectsPolygon(const Polygon &polygon, const Bounds &tile);
};
}
#endif
