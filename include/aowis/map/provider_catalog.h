#ifndef AOWIS_MAP_PROVIDER_CATALOG_H
#define AOWIS_MAP_PROVIDER_CATALOG_H

#include <aowis/map/map_cascade_config.h>
#include <aowis/map/map_provider.h>

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QStringList>
#include <QVector>

// A read-only view of providers offered locally and by configured upstream MAP
// servers. The local provider repository is never modified by discovery.
namespace Aowis::Map
{
struct AvailableMapProvider
{
    MapProvider provider;
    bool inherited = false;
    QStringList upstream_names; // Immediate upstreams, in configured priority order.
};

class ProviderCatalog final : public QObject
{
    Q_OBJECT
public:
    explicit ProviderCatalog(const QString &cache_directory, QObject *parent = nullptr);

    void configure(const QVector<MapProvider> &local_providers,
                   const MapCascadeConfig &cascade);
    void setLocalProviders(const QVector<MapProvider> &local_providers);
    void setDisabledInheritedProviders(const QSet<QString> &ids);
    void refresh(); // Nonblocking; failed refreshes retain validated offline snapshots.
    QVector<AvailableMapProvider> available() const;
    QByteArray encodedAvailable() const;
    QHash<QString, QSet<QString>> inheritedUpstreams() const;

signals:
    void catalogChanged();

private:
    void rebuild();
    void fetch(const MapCascadeUpstream &upstream, bool legacy, quint64 generation);
    QString cachePath(const MapCascadeUpstream &upstream) const;
    bool decode(const QByteArray &payload, QVector<MapProvider> *providers) const;

    QString cache_directory_;
    QVector<MapProvider> local_providers_;
    MapCascadeConfig cascade_;
    QSet<QString> disabled_inherited_ids_;
    QHash<QString, QVector<MapProvider>> upstream_catalogs_;
    QVector<AvailableMapProvider> available_;
    QByteArray encoded_;
    QNetworkAccessManager network_;
    QTimer refresh_timer_;
    quint64 generation_ = 0;
};
}

#endif
