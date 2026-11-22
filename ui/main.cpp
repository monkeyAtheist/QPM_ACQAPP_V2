// QTacquisitionApp: the window (Qt Quick) over the application layer (qacq_app).
#include "qacq/app/acquisition_controller.h"
#include "qacq/app/analysis_controller.h"
#include "qacq/app/app_settings.h"
#include "qacq/app/audio_player.h"
#include "qacq/app/curves_controller.h"
#include "qacq/app/filter_controller.h"
#include "qacq/app/generator_controller.h"
#include "qacq/app/modulation_controller.h"
#include "qacq/app/serial_port_list.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/app/units.h"
#include "image_tools.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QVariantMap>
#include <QtQml/qqml.h>

int main(int argc, char *argv[])
{
    // Compact Material controls, suited to a desktop application (unless the user chose otherwise).
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT"))
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    QGuiApplication app(argc, argv);
    // Windows: "Unable to obtain clipboard" whenever another program holds the clipboard. Harmless, and it
    // would drown real messages in the log.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.mime.warning=false"));
    QGuiApplication::setOrganizationName(QStringLiteral("QTacquisitionApp"));
    QGuiApplication::setApplicationName(QStringLiteral("QTacquisitionApp"));
    QGuiApplication::setApplicationVersion(QStringLiteral(QACQ_VERSION));
    QQuickStyle::setStyle(QStringLiteral("Material"));

    // Application-layer types (library qacq_app): made here, handed to the window.
    const QString made = QStringLiteral("Créé par l'application");
    qmlRegisterUncreatableType<qacq::app::Session>("Qacq.App", 1, 0, "Session", made);
    qmlRegisterUncreatableType<qacq::app::SignalListModel>("Qacq.App", 1, 0, "SignalListModel", made);
    qmlRegisterUncreatableType<qacq::app::AcquisitionController>("Qacq.App", 1, 0, "AcquisitionController", made);
    qmlRegisterUncreatableType<qacq::app::AnalysisController>("Qacq.App", 1, 0, "AnalysisController", made);
    qmlRegisterUncreatableType<qacq::app::CurvesController>("Qacq.App", 1, 0, "CurvesController", made);
    qmlRegisterUncreatableType<qacq::app::GeneratorController>("Qacq.App", 1, 0, "GeneratorController", made);
    qmlRegisterUncreatableType<qacq::app::FilterController>("Qacq.App", 1, 0, "FilterController", made);
    qmlRegisterUncreatableType<qacq::app::ModulationController>("Qacq.App", 1, 0, "ModulationController", made);
    qmlRegisterUncreatableType<qacq::app::AudioPlayer>("Qacq.App", 1, 0, "AudioPlayer", made);
    qmlRegisterUncreatableType<qacq::app::SerialPortList>("Qacq.App", 1, 0, "SerialPortList", made);
    qmlRegisterUncreatableType<qacq::app::AppSettings>("Qacq.App", 1, 0, "AppSettings", made);
    qmlRegisterSingletonType<qacq::app::Units>("Qacq.App", 1, 0, "Units",
                                               [](QQmlEngine *, QJSEngine *) -> QObject * { return new qacq::app::Units; });
    qmlRegisterSingletonType<ImageTools>("Qacq.App", 1, 0, "ImageTools",
                                         [](QQmlEngine *, QJSEngine *) -> QObject * { return new ImageTools; });

    qacq::app::AppSettings settings;
    qacq::app::Session session(&settings);
    qacq::app::AcquisitionController acquisition(&session);
    qacq::app::AnalysisController analysis(&session);
    qacq::app::CurvesController curves(&session, &analysis);
    qacq::app::GeneratorController generator(&session);
    qacq::app::FilterController filters(&session);
    qacq::app::ModulationController modulations(&session);
    qacq::app::AudioPlayer player(&settings);
    qacq::app::SerialPortList serialPorts;

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("session"), QVariant::fromValue(&session)},
                                 {QStringLiteral("acquisition"), QVariant::fromValue(&acquisition)},
                                 {QStringLiteral("analysis"), QVariant::fromValue(&analysis)},
                                 {QStringLiteral("curves"), QVariant::fromValue(&curves)},
                                 {QStringLiteral("generator"), QVariant::fromValue(&generator)},
                                 {QStringLiteral("filters"), QVariant::fromValue(&filters)},
                                 {QStringLiteral("modulations"), QVariant::fromValue(&modulations)},
                                 {QStringLiteral("player"), QVariant::fromValue(&player)},
                                 {QStringLiteral("serialPorts"), QVariant::fromValue(&serialPorts)},
                                 {QStringLiteral("appSettings"), QVariant::fromValue(&settings)}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("Qacq", "Main");
    const int code = QGuiApplication::exec();
    // The acquisition stops (its thread joined) before the session it writes to goes away.
    acquisition.stop(false);
    return code;
}
