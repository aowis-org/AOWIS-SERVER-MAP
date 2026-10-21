#include <aowis/map/download_coverage.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <iterator>

namespace Aowis::Map
{
namespace
{
using Bounds = DownloadCoverage::Bounds;

bool validBounds(const Bounds &b)
{
    return std::isfinite(b.west) && std::isfinite(b.east) &&
           std::isfinite(b.south) && std::isfinite(b.north) &&
           b.west >= -180 && b.east <= 180 && b.west <= b.east &&
           b.south >= -90 && b.north <= 90 && b.south <= b.north;
}

Bounds xyzBounds(int zoom, int x, int y)
{
    const double count = double(quint64(1) << zoom);
    const double west = double(x) / count * 360.0 - 180.0;
    const double east = double(x + 1) / count * 360.0 - 180.0;
    const auto latitude = [count](int row) -> double
    {
        return qRadiansToDegrees(std::atan(std::sinh(std::numbers::pi * (1.0 - 2.0 * double(row) / count))));
    };
    return { west, latitude(y + 1), east, latitude(y) };
}

bool ringContains(const QVector<QPair<double, double>> &ring, double x, double y)
{
    bool inside = false;
    for (qsizetype i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
    {
        const auto &a = ring[i];
        const auto &b = ring[j];
        if ((a.second > y) != (b.second > y) &&
            x < (b.first - a.first) * (y - a.second) / (b.second - a.second) + a.first)
            inside = !inside;
    }
    return inside;
}

bool segmentsIntersect(double ax, double ay, double bx, double by,
                       double cx, double cy, double dx, double dy)
{
    const auto cross = [](double x1, double y1, double x2, double y2, double x3, double y3)
    {
        return (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    };
    const double o1 = cross(ax, ay, bx, by, cx, cy);
    const double o2 = cross(ax, ay, bx, by, dx, dy);
    const double o3 = cross(cx, cy, dx, dy, ax, ay);
    const double o4 = cross(cx, cy, dx, dy, bx, by);
    if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
        ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
        return true;
    constexpr double epsilon = 1e-10;
    const auto on = [epsilon](double px, double py, double ax, double ay,
                              double bx, double by)
    {
        return std::abs((bx - ax) * (py - ay) - (by - ay) * (px - ax)) < epsilon &&
               px >= std::min(ax, bx) - epsilon && px <= std::max(ax, bx) + epsilon &&
               py >= std::min(ay, by) - epsilon && py <= std::max(ay, by) + epsilon;
    };
    return (std::abs(o1) < epsilon && on(cx, cy, ax, ay, bx, by)) ||
           (std::abs(o2) < epsilon && on(dx, dy, ax, ay, bx, by)) ||
           (std::abs(o3) < epsilon && on(ax, ay, cx, cy, dx, dy)) ||
           (std::abs(o4) < epsilon && on(bx, by, cx, cy, dx, dy));
}
}

bool DownloadCoverage::intersects(const Bounds &a, const Bounds &b)
{
    return a.west <= b.east && b.west <= a.east &&
           a.south <= b.north && b.south <= a.north;
}

bool DownloadCoverage::addRectangle(const QString &name, const Bounds &b, QString *error)
{
    if (name.trimmed().isEmpty() || name == QStringLiteral("world") || !validBounds(b))
    {
        if (error) *error = QStringLiteral("Invalid region '%1' or bounding rectangle").arg(name);
        return false;
    }
    this->regions[name].rectangles.append(b);
    return true;
}

bool DownloadCoverage::hasRegion(const QString &name) const
{
    return name == QStringLiteral("world") || this->regions.contains(name);
}

bool DownloadCoverage::addProfile(const QString &name, const Profile &profile, QString *error)
{
    if (name.isEmpty() || profile.tiers.isEmpty())
    {
        if (error) *error = QStringLiteral("Coverage profile '%1' has no zoom tiers").arg(name);
        return false;
    }
    bool seen[31] = {};
    for (const Tier &tier : profile.tiers)
    {
        if (tier.minimum_zoom < 0 || tier.maximum_zoom > 30 || tier.minimum_zoom > tier.maximum_zoom ||
            tier.regions.isEmpty())
        {
            if (error) *error = QStringLiteral("Invalid zoom range in coverage '%1'").arg(name);
            return false;
        }
        for (const QString &region : tier.regions)
        {
            if (!hasRegion(region))
            {
                if (error) *error = QStringLiteral("Coverage '%1' references unknown region '%2'")
                                        .arg(name, region);
                return false;
            }
        }
        for (int z = tier.minimum_zoom; z <= tier.maximum_zoom; ++z)
        {
            if (seen[z])
            {
                if (error) *error = QStringLiteral("Overlapping zoom tiers in coverage '%1'").arg(name);
                return false;
            }
            seen[z] = true;
        }
    }
    // Missing zoom ranges are deliberately disallowed rather than silently
    // permitting the entire world at an unexpectedly uncovered resolution.
    if (std::find(std::begin(seen), std::end(seen), false) != std::end(seen))
    {
        if (error) *error = QStringLiteral("Coverage '%1' must define every zoom 0 through 30").arg(name);
        return false;
    }
    this->profiles.insert(name, profile);
    return true;
}

bool DownloadCoverage::hasProfile(const QString &name) const
{
    return name.isEmpty() || this->profiles.contains(name);
}

QStringList DownloadCoverage::profileNames() const
{
    QStringList names = this->profiles.keys();
    names.sort();
    return names;
}

bool DownloadCoverage::intersectsPolygon(const Polygon &polygon, const Bounds &tile)
{
    if (!intersects(polygon.envelope, tile) || polygon.rings.isEmpty())
        return false;

    QVector<QVector<QPair<double, double>>> rings;
    for (const QVector<Point> &ring : polygon.rings)
    {
        QVector<QPair<double, double>> pairs;
        for (const Point &p : ring)
            pairs.append(qMakePair(p.lon, p.lat));
        rings.append(pairs);
    }
    const auto inside = [&rings](double x, double y)
    {
        if (!ringContains(rings.first(), x, y)) return false;
        for (qsizetype i = 1; i < rings.size(); ++i)
            if (ringContains(rings[i], x, y)) return false;
        return true;
    };
    for (const Point &p : polygon.rings.first())
        if (p.lon >= tile.west && p.lon <= tile.east &&
            p.lat >= tile.south && p.lat <= tile.north && inside(p.lon, p.lat))
            return true;
    for (const double lon : {tile.west, tile.east})
        for (const double lat : {tile.south, tile.north})
            if (inside(lon, lat)) return true;

    const double x1[4] = { tile.west, tile.east, tile.east, tile.west };
    const double y1[4] = { tile.south, tile.south, tile.north, tile.north };
    for (const QVector<Point> &ring : polygon.rings)
    {
        for (qsizetype i = 1; i < ring.size(); ++i)
        {
            for (int edge = 0; edge < 4; ++edge)
            {
                if (segmentsIntersect(ring[i-1].lon, ring[i-1].lat,
                                      ring[i].lon, ring[i].lat,
                                      x1[edge], y1[edge], x1[(edge+1)%4], y1[(edge+1)%4]))
                    return true;
            }
        }
    }
    return false;
}

bool DownloadCoverage::permitsTile(const QString &profile, int zoom, int x, int y) const
{
    if (profile.isEmpty()) return true;
    if (zoom < 0 || zoom > 30 || !this->profiles.contains(profile)) return false;
    const int count = 1 << zoom;
    if (y < 0 || y >= count) return false;
    x = int((qint64(x) % count + count) % count);
    const Bounds tile = xyzBounds(zoom, x, y);
    for (const Tier &tier : this->profiles.value(profile).tiers)
    {
        if (zoom < tier.minimum_zoom || zoom > tier.maximum_zoom) continue;
        for (const QString &name : tier.regions)
        {
            if (name == QStringLiteral("world")) return true;
            const auto region_it = this->regions.constFind(name);
            if (region_it == this->regions.cend()) continue;
            const Region &region = region_it.value();
            for (const Bounds &rectangle : region.rectangles)
                if (intersects(rectangle, tile)) return true;
            for (const Polygon &polygon : region.polygons)
                if (intersectsPolygon(polygon, tile)) return true;
        }
    }
    return false;
}

bool DownloadCoverage::addGeoJsonRegions(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("Cannot open region GeoJSON: %1").arg(path);
        return false;
    }
    QJsonParseError parse_error;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !json.isObject() ||
        json.object().value(QStringLiteral("type")).toString() != QStringLiteral("FeatureCollection"))
    {
        if (error) *error = QStringLiteral("Invalid GeoJSON FeatureCollection: %1").arg(path);
        return false;
    }
    const QJsonArray features = json.object().value(QStringLiteral("features")).toArray();
    for (const QJsonValue &feature_value : features)
    {
        const QJsonObject feature = feature_value.toObject();
        const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
        // ISO_A2 is used by Natural Earth; id allows custom named regions.
        QString id = properties.value(QStringLiteral("ISO_A2")).toString();
        if (id.isEmpty() || id == QStringLiteral("-99"))
            id = properties.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const QJsonObject geometry = feature.value(QStringLiteral("geometry")).toObject();
        const QString type = geometry.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("Polygon") && type != QStringLiteral("MultiPolygon")) continue;
        const QJsonArray polygons = type == QStringLiteral("Polygon")
            ? QJsonArray {geometry.value(QStringLiteral("coordinates"))}
            : geometry.value(QStringLiteral("coordinates")).toArray();
        Region &region = this->regions[id];
        for (const QJsonValue &polygon_value : polygons)
        {
            Polygon polygon;
            polygon.envelope = {180.0, 90.0, -180.0, -90.0};
            for (const QJsonValue &ring_value : polygon_value.toArray())
            {
                QVector<Point> ring;
                for (const QJsonValue &position : ring_value.toArray())
                {
                    const QJsonArray coordinates = position.toArray();
                    if (coordinates.size() < 2 || !coordinates[0].isDouble() || !coordinates[1].isDouble())
                        continue;
                    const Point point {coordinates[0].toDouble(), coordinates[1].toDouble()};
                    if (!std::isfinite(point.lon) || !std::isfinite(point.lat) ||
                        point.lon < -180 || point.lon > 180 ||
                        point.lat < -90 || point.lat > 90)
                    {
                        if (error) *error = QStringLiteral("Invalid coordinate in GeoJSON region '%1'").arg(id);
                        return false;
                    }
                    if (!ring.isEmpty() && std::abs(point.lon - ring.last().lon) > 180)
                    {
                        if (error) *error = QStringLiteral("GeoJSON region '%1' crosses the antimeridian; "
                            "split its polygon at longitude 180 to avoid ambiguous coverage").arg(id);
                        return false;
                    }
                    ring.append(point);
                    polygon.envelope.west = std::min(polygon.envelope.west, point.lon);
                    polygon.envelope.east = std::max(polygon.envelope.east, point.lon);
                    polygon.envelope.south = std::min(polygon.envelope.south, point.lat);
                    polygon.envelope.north = std::max(polygon.envelope.north, point.lat);
                }
                if (ring.size() >= 4) polygon.rings.append(ring);
            }
            if (!polygon.rings.isEmpty()) region.polygons.append(polygon);
        }
    }
    if (this->regions.isEmpty())
    {
        if (error) *error = QStringLiteral("Region GeoJSON contains no named polygons");
        return false;
    }
    return true;
}
}
