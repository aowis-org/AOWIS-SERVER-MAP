#ifndef COPERNICUS_RESUME_INTEGRATION_TEST_H
#define COPERNICUS_RESUME_INTEGRATION_TEST_H

#include <QString>
#include <QUrl>

namespace Aowis::Map::Internal
{
// Internal transport test seam; never used to change production source URLs.
bool fetchCopernicusSourceForIntegrationTest(const QString &directory,
                                             const QUrl &source_url,
                                             QString *error_message,
                                             int connect_timeout_ms = 5000,
                                             int inactivity_timeout_ms = 5000,
                                             int download_deadline_ms = 10000);
}

#endif
