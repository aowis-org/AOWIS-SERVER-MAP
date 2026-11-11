#ifndef AOWIS_MAP_SETTINGS_REPOSITORY_H
#define AOWIS_MAP_SETTINGS_REPOSITORY_H

#include <aowis/map/map_cascade_config.h>
#include <aowis/map/map_provider.h>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>

namespace Aowis::Map
{
// MAP owns its schemas and the AOWIS-CONFIG records. GUI/Standalone must use
// this library API (or the MAP REST API), never inspect or write these records.
// Separate owners ('server-map' and 'standalone-map') have isolated storage.
// Empty revision means that no record exists. Every write uses optimistic
// concurrency: stale edits fail instead of silently overwriting another writer.
class SettingsRepository final
{
public:
    static std::unique_ptr<SettingsRepository> openForCurrentUser(
        const QString &owner, QString *error = nullptr);
    static std::unique_ptr<SettingsRepository> openAtOwnerRoot(
        const QString &owner, const QString &absolute_root, QString *error = nullptr);

    ~SettingsRepository();
    SettingsRepository(const SettingsRepository &) = delete;
    SettingsRepository &operator=(const SettingsRepository &) = delete;

    bool readCascade(MapCascadeConfig *value, QString *revision = nullptr,
                     QString *error = nullptr) const;
    bool saveCascade(const MapCascadeConfig &value, const QString &expected_revision,
                     QString *revision = nullptr, QString *error = nullptr) const;
    bool readProviders(QVector<MapProvider> *value, QString *revision = nullptr,
                       QString *error = nullptr) const;
    bool saveProviders(const QVector<MapProvider> &value, const QString &expected_revision,
                       QString *revision = nullptr, QString *error = nullptr) const;

private:
    struct Impl;
    explicit SettingsRepository(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

bool validateCascadeSettings(const MapCascadeConfig &value, QString *error = nullptr);

// The public MAP provider REST payload format is also used by the Standalone
// editor. In particular, an older editor may omit allow_lossy_transfer; the
// MAP library preserves that property from 'previous'.
QByteArray encodeProviderCatalog(const QVector<MapProvider> &providers);
bool decodeProviderCatalog(const QByteArray &payload, const QVector<MapProvider> &previous,
                           QVector<MapProvider> *providers, QString *error = nullptr);
}

#endif // AOWIS_MAP_SETTINGS_REPOSITORY_H
