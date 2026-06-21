#include <QCoreApplication>

#include <QDebug>

#include "server.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    
    Server server(&app);
    
    return app.exec();
}
