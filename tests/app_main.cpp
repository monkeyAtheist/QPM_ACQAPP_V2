// Application-layer tests need a QCoreApplication (settings, file names, event loop).
#include "qacq_test.h"

#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("QTacquisitionApp-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("qacq_app_tests"));
    return qacq::test::runAll(argc, argv);
}
