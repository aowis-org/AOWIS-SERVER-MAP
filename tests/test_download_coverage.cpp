#include <aowis/map/download_coverage.h>

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

static void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    Aowis::Map::DownloadCoverage coverage;
    QString error;
    require(coverage.addRectangle(QStringLiteral("chad"), {14, 11, 15, 12}, &error),
            "Failed to configure Chad region");
    require(coverage.addRectangle(QStringLiteral("peru"), {-76, -12, -75, -11}, &error),
            "Failed to configure Peru region");
    Aowis::Map::DownloadCoverage::Profile profile;
    profile.tiers = {
        {0, 5, {QStringLiteral("world")}},
        {6, 30, {QStringLiteral("chad"), QStringLiteral("peru")}}
    };
    require(coverage.addProfile(QStringLiteral("projects"), profile, &error),
            "Failed to configure multi-region profile");
    require(coverage.permitsTile(QStringLiteral("projects"), 0, 0, 0),
            "World overview tile should be allowed");
    // At zoom 8 Chad intersects XYZ (137,119), Peru XYZ (73,136).
    require(coverage.permitsTile(QStringLiteral("projects"), 8, 138, 119),
            "Chad tile should be allowed");
    require(coverage.permitsTile(QStringLiteral("projects"), 8, 74, 136),
            "Peru tile should be allowed");
    require(!coverage.permitsTile(QStringLiteral("projects"), 8, 200, 95),
            "Unrelated high-zoom tile should be denied");
    require(coverage.permitsTile({}, 8, 200, 95),
            "Empty policy must mean unrestricted");
    require(!coverage.permitsTile(QStringLiteral("undefined"), 8, 138, 119),
            "Unknown policy must fail closed");

    Aowis::Map::DownloadCoverage::Profile invalid;
    invalid.tiers = {{0, 10, {QStringLiteral("chad")}},
                     {10, 30, {QStringLiteral("peru")}}};
    require(!coverage.addProfile(QStringLiteral("overlap"), invalid, &error),
            "Overlapping tiers must be rejected");

    QTemporaryDir directory;
    require(directory.isValid(), "Temporary directory unavailable");
    const QString path = directory.filePath(QStringLiteral("regions.geojson"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot write test GeoJSON");
    const QByteArray geojson = R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"ISO_A2":"XY"},"geometry":{"type":"Polygon","coordinates":[[[0,0],[2,0],[2,2],[0,2],[0,0]]]}}]})";
    require(file.write(geojson) == geojson.size(), "Cannot write GeoJSON");
    file.close();
    require(coverage.addGeoJsonRegions(path, &error), "Cannot read region GeoJSON");
    require(coverage.hasRegion(QStringLiteral("XY")), "Country id not found");

    std::cout << "Download coverage tests passed\n";
    return EXIT_SUCCESS;
}
