#include "server.h"

#include <aowis/map/map_provider.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QThread>

#include <png.h>

#include <cstdio>
#include <vector>

class RasterJpegHttpTestAccess
{
public:
    static quint16 port(const Server &server) { return server.tcp->serverPort(); }
};

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

QByteArray pngTile()
{
    std::vector<unsigned char> pixels(256 * 256 * 3);
    quint32 state = 0xdeadbeef;
    for (int y = 0; y < 256; ++y)
    {
        for (int x = 0; x < 256; ++x)
        {
            state = state * 1664525u + 1013904223u;
            const int noise = int(state >> 24) & 31;
            const size_t index = (size_t(y) * 256 + x) * 3;
            pixels[index] = static_cast<unsigned char>((2 * x + noise) % 256);
            pixels[index + 1] = static_cast<unsigned char>((2 * y + noise) % 256);
            pixels[index + 2] = static_cast<unsigned char>((x + y + noise) % 256);
        }
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = 256;
    image.height = 256;
    image.format = PNG_FORMAT_RGB;
    png_alloc_size_t count = 0;
    if (!png_image_write_to_memory(&image, nullptr, &count, 0, pixels.data(), 0, nullptr))
        return {};
    QByteArray encoded(int(count), Qt::Uninitialized);
    if (!png_image_write_to_memory(&image, encoded.data(), &count, 0,
                                   pixels.data(), 0, nullptr)) return {};
    encoded.resize(int(count));
    return encoded;
}

bool await(QCoreApplication &application, QNetworkReply *reply)
{
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < 5000)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return reply->isFinished();
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(directory.isValid(), "Cannot create raster test cache")) return 1;
    Server::Config config;
    config.port = 0;
    config.cache_directory = directory.path();
    config.terrain_enabled = false;
    config.map_remote_fetch_enabled = false;
    config.cascade.adaptive_compression = false; // deterministic compression test
    const QString provider = config.map_providers.first().id;
    const QByteArray original = pngTile();
    if (!require(original.size() > 12 * 1024, "Bad test PNG")) return 1;
    const QString cache_path = QDir(directory.path()).filePath(
        QStringLiteral("maptiles/%1/12/2100/1390.png").arg(provider));
    if (!require(QDir().mkpath(QFileInfo(cache_path).absolutePath()),
                 "Cannot create raster cache directory")) return 1;
    QFile cached(cache_path);
    if (!require(cached.open(QIODevice::WriteOnly) && cached.write(original) == original.size(),
                 "Cannot seed raster cache")) return 1;
    cached.close();

    Server server(config);
    if (!require(server.start(), "Cannot start raster HTTP test server")) return 1;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/%2/12/2100/1390.png")
                   .arg(RasterJpegHttpTestAccess::port(server)).arg(provider));
    const QByteArray trace = QByteArrayLiteral("00000000-0000-4000-8000-000000000001");
    QNetworkAccessManager network;

    QNetworkRequest ordinary(url);
    QNetworkReply *normal_reply = network.get(ordinary);
    if (!require(await(application, normal_reply) && normal_reply->readAll() == original &&
                 normal_reply->rawHeader("Content-Type").startsWith("image/png"),
                 "Ordinary GUI did not receive original PNG")) return 1;
    normal_reply->deleteLater();

    QNetworkRequest disguised(url);
    disguised.setRawHeader("X-AOWIS-Raster-Transfer", "adaptive-jpeg;quality=75");
    QNetworkReply *disguised_reply = network.get(disguised);
    if (!require(await(application, disguised_reply) && disguised_reply->readAll() == original,
                 "Non-MAP client was allowed to force lossy raster data")) return 1;
    disguised_reply->deleteLater();

    QNetworkRequest cascade(url);
    cascade.setRawHeader("X-AOWIS-Cascade-Visited", trace);
    cascade.setRawHeader("X-AOWIS-Raster-Transfer", "adaptive-jpeg;quality=75");
    QNetworkReply *optimized_reply = network.get(cascade);
    if (!require(await(application, optimized_reply), "MAP-to-MAP JPEG response timed out"))
        return 1;
    const QByteArray optimized = optimized_reply->readAll();
    if (!require(optimized_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                 optimized_reply->rawHeader("Content-Type").startsWith("image/jpeg") &&
                 optimized_reply->rawHeader("Vary").contains("X-AOWIS-Raster-Transfer") &&
                 optimized.startsWith("\xff\xd8\xff") && optimized.size() < original.size(),
                 "MAP peer did not receive smaller JPEG image")) return 1;
    optimized_reply->deleteLater();

    QNetworkReply *again_reply = network.get(cascade);
    if (!require(await(application, again_reply) && again_reply->readAll() == optimized,
                 "MAP-to-MAP JPEG result not reused for subsequent requests")) return 1;
    again_reply->deleteLater();

    Server::Config protected_config = config;
    protected_config.map_providers.first().allow_lossy_transfer = false;
    Server protected_server(protected_config);
    if (!require(protected_server.start(), "Cannot start provider opt-out test server")) return 1;
    const QUrl protected_url(QStringLiteral("http://127.0.0.1:%1/%2/12/2100/1390.png")
        .arg(RasterJpegHttpTestAccess::port(protected_server)).arg(provider));
    QNetworkRequest protected_request(protected_url);
    protected_request.setRawHeader("X-AOWIS-Cascade-Visited", trace);
    protected_request.setRawHeader("X-AOWIS-Raster-Transfer", "adaptive-jpeg;quality=75");
    QNetworkReply *protected_reply = network.get(protected_request);
    if (!require(await(application, protected_reply) && protected_reply->readAll() == original,
                 "Provider lossy opt-out did not preserve original PNG")) return 1;
    protected_reply->deleteLater();
    return 0;
}
