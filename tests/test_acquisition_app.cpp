// Acquisition with Qt: sources on their threads, the controller, the list of serial ports. No hardware:
// a simulator, local TCP and UDP sockets and a fake SCPI instrument stand for the devices.
#include "qacq_test.h"

#include "qacq/acquisition/simulator.h"
#include "qacq/app/acquisition_controller.h"
#include "qacq/app/audio_format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/serial_port_list.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/model/csv.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <cmath>
#include <functional>
#include <memory>

using namespace qacq;
using namespace qacq::app;
using acquisition::AcquisitionSettings;

namespace
{
bool waitUntil(const std::function<bool()> &condition, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition())
    {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

void spin(int milliseconds)
{
    waitUntil([] { return false; }, milliseconds);
}

std::uint64_t total(const AcquisitionController &c)
{
    return c.recorder().totalFrames();
}

AcquisitionSettings simulated(double rate, acquisition::DecoderKind kind)
{
    AcquisitionSettings s;
    s.name = "Essai";
    s.source = acquisition::SourceKind::Simulated;
    s.declaredSampleRateHz = rate;
    s.simulated.channels = {{dsp::Waveform::Sine, 1.0, 50.0, 0.0, 0.5, 0.5, 0.0}, {dsp::Waveform::Square, 2.0, 7.0, 0.0, 0.0, 0.25, 0.0}};
    s.decoder.kind = kind;
    s.decoder.frame.channels = 2;
    s.decoder.frame.sample = {io::SampleType::Int16, io::ByteOrder::LittleEndian, 0.0001, 0.0};
    s.decoder.raw = s.decoder.frame.sample;
    s.decoder.rawChannels = 2;
    return s;
}

double quantise(double v, const io::SampleFormat &f)
{
    std::uint8_t b[8] = {};
    io::writeSample(v, f, b);
    return io::readSample(b, f);
}

// A TCP server on this PC only, that answers each connection with `onConnect`.
struct LocalServer
{
    QTcpServer server;
    std::function<void(QTcpSocket *)> onConnect;
    QList<QTcpSocket *> clients;

    LocalServer()
    {
        server.listen(QHostAddress(QHostAddress::LocalHost), 0);
        QObject::connect(&server, &QTcpServer::newConnection, [this] {
            while (QTcpSocket *s = server.nextPendingConnection())
            {
                clients.append(s);
                if (onConnect) onConnect(s);
            }
        });
    }
    int port() const { return server.serverPort(); }
};
} // namespace

TEST_CASE("Serial ports: board names, labels, a board plugged in", "[sources]")
{
    CHECK(SerialPortList::boardName(0x2341, 0x0043) == QStringLiteral("Arduino"));
    CHECK(SerialPortList::boardName(0x0483, 0x374B) == QStringLiteral("STM32 (ST-Link)"));
    CHECK(SerialPortList::boardName(0x303A, 0x1001) == QStringLiteral("ESP32 (USB natif)"));
    CHECK(SerialPortList::boardName(0x1234, 0x5678).isEmpty());
    SerialPortList list(nullptr, 0);
    SerialPortList::Port uno;
    uno.name = QStringLiteral("COM3");
    uno.description = QStringLiteral("Périphérique série USB");
    uno.vendorId = 0x2341;
    uno.productId = 0x0043;
    list.setPorts({uno});
    REQUIRE(list.rowCount() == 1);
    CHECK(list.data(list.index(0), SerialPortList::LabelRole).toString() == QStringLiteral("COM3 — Arduino (2341:0043)"));
    CHECK(list.data(list.index(0), SerialPortList::BoardRole).toString() == QStringLiteral("Arduino"));
    QStringList added;
    QObject::connect(&list, &SerialPortList::portAdded, [&added](const QString &p) { added << p; });
    SerialPortList::Port other;
    other.name = QStringLiteral("COM7");
    other.description = QStringLiteral("Silicon Labs CP210x");
    other.vendorId = 0x10C4;
    other.productId = 0xEA60;
    list.setPorts({uno, other});
    CHECK(list.count() == 2);
    CHECK(added == QStringList{QStringLiteral("COM7")});
    CHECK(list.rowOf(QStringLiteral("COM7")) == 1);
    CHECK(list.portAt(1) == QStringLiteral("COM7"));
    list.refresh(); // the real ports of this PC: whatever they are, no crash
    CHECK(list.rowCount() >= 0);
}

TEST_CASE("Sound card format and input list", "[sources]")
{
    acquisition::AudioSettings a;
    a.sampleRate = 44100;
    a.channels = 2;
    a.sampleType = acquisition::AudioSampleType::Float32;
    const QAudioFormat f = audioFormatFor(a);
    CHECK(f.sampleRate() == 44100);
    CHECK(f.channelCount() == 2);
    CHECK(f.sampleFormat() == QAudioFormat::Float);
    AcquisitionController controller;
    const QStringList inputs = controller.audioInputs(); // may be empty on a PC without microphone
    CHECK(inputs.size() >= 0);
}

TEST_CASE("Simulated source: frames flow, rate measured, signal kept at stop and undoable", "[sources]")
{
    Session session;
    AcquisitionController controller(&session);
    controller.setAcquisitionSettings(simulated(2000.0, acquisition::DecoderKind::Text));
    REQUIRE(controller.start());
    CHECK(controller.isRunning());
    REQUIRE(waitUntil([&] { return controller.stateValue() == AcquisitionController::State::Running; }, 3000));
    REQUIRE(waitUntil([&] { return total(controller) >= 2000 && controller.measuredRate() > 0.0; }, 8000));
    CHECK(controller.channelCount() == 2);
    CHECK(std::abs(controller.measuredRate() - 2000.0) < 100.0);
    CHECK(controller.statistics().value(QStringLiteral("rejected")).toLongLong() == 0);
    const qint64 id = controller.stop();
    CHECK(!controller.isRunning());
    REQUIRE(id > 0);
    const model::SignalPtr signal = session.signalList()->signalById(id);
    REQUIRE(signal != nullptr);
    CHECK(signal->origin == model::Origin::Acquired);
    CHECK(signal->sampleRateHz == 2000.0);
    CHECK(signal->channelCount() == 2);
    CHECK(signal->frameCount() == total(controller));
    CHECK(signal->name == "Essai");
    CHECK(signal->provenance["source"].toString() == "simulated");
    // Values: those of the simulated board, to the 4 decimals of its text.
    acquisition::SimulatedDevice reference(controller.acquisitionSettings().simulated, acquisition::effectiveDecoder(controller.acquisitionSettings()), 2000.0);
    double worst = 0.0;
    for (std::size_t f = 0; f < signal->frameCount(); f += 7)
        for (std::size_t c = 0; c < 2; ++c) worst = std::max(worst, std::abs(signal->data->channels[c][f] - reference.cleanValue(c, f)));
    CHECK(worst <= 0.5e-4 + 1e-9);
    CHECK(session.signalList()->undoText() == QStringLiteral("Acquisition « Essai »"));
    session.signalList()->undo();
    CHECK(session.signalList()->count() == 0);
}

TEST_CASE("Simulated frames with losses: lost frames counted and marked in the signal", "[sources]")
{
    Session session;
    AcquisitionController controller(&session);
    AcquisitionSettings s = simulated(5000.0, acquisition::DecoderKind::FramedBinary);
    s.decoder.frame.checksum = io::Checksum::Crc16Ccitt;
    s.decoder.frame.counter = io::CounterField::UInt16;
    s.simulated.frameLossRate = 0.02;
    s.simulated.byteErrorRate = 1e-4;
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return total(controller) >= 4000; }, 8000));
    const qint64 id = controller.stop();
    REQUIRE(id > 0);
    const model::SignalPtr signal = session.signalList()->signalById(id);
    CHECK(controller.statistics().value(QStringLiteral("lostFrames")).toLongLong() > 0);
    CHECK(!signal->data->discontinuities.empty());
    CHECK(signal->notes.find("perdue") != std::string::npos);
}

TEST_CASE("TCP source: a local server sends binary frames, decoded exactly", "[sources]")
{
    AcquisitionSettings s = simulated(1000.0, acquisition::DecoderKind::FramedBinary);
    acquisition::SimulatedDevice device(s.simulated, s.decoder, 1000.0);
    const std::vector<std::uint8_t> bytes = device.produce(3000);
    LocalServer server;
    REQUIRE(server.port() > 0);
    server.onConnect = [&bytes](QTcpSocket *socket) {
        socket->write(reinterpret_cast<const char *>(bytes.data()), static_cast<qint64>(bytes.size()));
        socket->flush();
    };
    Session session;
    AcquisitionController controller(&session);
    s.source = acquisition::SourceKind::TcpClient;
    s.tcp.host = "127.0.0.1";
    s.tcp.port = server.port();
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return total(controller) >= 3000; }, 8000));
    CHECK(controller.stateValue() == AcquisitionController::State::Running);
    CHECK(controller.statusText().contains(QStringLiteral("127.0.0.1")));
    const qint64 id = controller.stop();
    const model::SignalPtr signal = session.signalList()->signalById(id);
    REQUIRE(signal != nullptr);
    REQUIRE(signal->frameCount() == 3000);
    bool exact = true;
    for (std::size_t f = 0; f < 3000; ++f)
        for (std::size_t c = 0; c < 2; ++c)
            exact = exact && signal->data->channels[c][f] == quantise(device.cleanValue(c, f), s.decoder.frame.sample);
    CHECK(exact);
}

TEST_CASE("UDP source: text lines in datagrams", "[sources]")
{
    // A free port: let the system choose one, then release it.
    quint16 port = 0;
    {
        QUdpSocket probe;
        REQUIRE(probe.bind(QHostAddress(QHostAddress::LocalHost), 0));
        port = probe.localPort();
    }
    Session session;
    AcquisitionController controller(&session);
    AcquisitionSettings s = simulated(1000.0, acquisition::DecoderKind::Text);
    s.source = acquisition::SourceKind::Udp;
    s.udp.port = port;
    s.udp.bindAddress = "127.0.0.1";
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return controller.stateValue() == AcquisitionController::State::Running; }, 3000));
    acquisition::SimulatedDevice device(s.simulated, s.decoder, 1000.0);
    QUdpSocket sender;
    for (int k = 0; k < 10; ++k)
    {
        const std::vector<std::uint8_t> bytes = device.produce(100);
        sender.writeDatagram(reinterpret_cast<const char *>(bytes.data()), static_cast<qint64>(bytes.size()),
                             QHostAddress(QHostAddress::LocalHost), port);
        spin(5);
    }
    REQUIRE(waitUntil([&] { return total(controller) >= 1000; }, 5000));
    controller.stop(false);
    CHECK(total(controller) == 1000);
    CHECK(session.signalList()->count() == 0); // stop(false): nothing kept
}

TEST_CASE("SCPI source: identification, setup, polled measurements and waveform blocks", "[sources]")
{
    acquisition::DecoderSettings answers;
    answers.kind = acquisition::DecoderKind::Scpi;
    answers.scpi.channels = 1;
    answers.scpi.block = {io::SampleType::Int16, io::ByteOrder::LittleEndian, 0.001, 0.0};
    acquisition::SimulatedSettings sim;
    sim.channels = {{dsp::Waveform::Sine, 1.0, 5.0, 0.0, 0.0, 0.5, 0.0}};
    acquisition::SimulatedDevice instrument(sim, answers, 100.0);
    instrument.waveformPoints = 250;
    QStringList received;
    LocalServer server;
    server.onConnect = [&](QTcpSocket *socket) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
            buffer->append(socket->readAll());
            qsizetype end;
            while ((end = buffer->indexOf('\n')) >= 0)
            {
                const QByteArray line = buffer->left(end);
                buffer->remove(0, end + 1);
                received << QString::fromUtf8(line);
                const std::string answer = instrument.answer(line.toStdString());
                if (!answer.empty()) socket->write(answer.data(), static_cast<qint64>(answer.size()));
            }
        });
    };
    Session session;
    AcquisitionController controller(&session);
    AcquisitionSettings s;
    s.name = "Multimètre";
    s.source = acquisition::SourceKind::Scpi;
    s.scpi.host = "127.0.0.1";
    s.scpi.port = server.port();
    s.scpi.setupCommands = {"*RST", "CONF:VOLT:DC 10"};
    s.scpi.query = "MEAS:VOLT:DC?";
    s.scpi.intervalSeconds = 0.002;
    s.decoder = answers;
    s.declaredSampleRateHz = 0.0;
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return total(controller) >= 30; }, 8000));
    CHECK(controller.instrument() == QStringLiteral("QTacquisitionApp,Simulateur,0,2.0.0"));
    CHECK(controller.stateValue() == AcquisitionController::State::Running);
    const qint64 id = controller.stop();
    REQUIRE(received.size() >= 5);
    CHECK(received.at(0) == QStringLiteral("*IDN?"));
    CHECK(received.at(1) == QStringLiteral("*RST"));
    CHECK(received.at(2) == QStringLiteral("CONF:VOLT:DC 10"));
    CHECK(received.at(3) == QStringLiteral("SYST:ERR?"));
    CHECK(received.at(4) == QStringLiteral("MEAS:VOLT:DC?"));
    const model::SignalPtr polled = session.signalList()->signalById(id);
    REQUIRE(polled != nullptr);
    CHECK(polled->channelCount() == 1);
    CHECK(polled->data->discontinuities.empty()); // one value per answer: a continuous series
    CHECK_NEAR(polled->data->channels[0][3], instrument.cleanValue(0, 3), 0, 1e-6);

    // The same instrument, asked for waveforms: each answer is a record of its own.
    s.scpi.query = ":WAV:DATA?";
    s.scpi.setupCommands.clear();
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return total(controller) >= 1000; }, 8000));
    const qint64 waves = controller.stop();
    const model::SignalPtr blocks = session.signalList()->signalById(waves);
    REQUIRE(blocks != nullptr);
    // One record per answer (the last may be cut by the stop): a discontinuity at each start but the first.
    const std::size_t records = (blocks->frameCount() + 249) / 250;
    REQUIRE(records >= 4);
    REQUIRE(blocks->data->discontinuities.size() == records - 1);
    bool aligned = true;
    for (std::size_t d : blocks->data->discontinuities) aligned = aligned && d % 250 == 0;
    CHECK(aligned);
    CHECK(blocks->data->discontinuities.front() == 250);
}

TEST_CASE("Acquisition errors: bad settings, refused connection, missing serial port", "[sources]")
{
    AcquisitionController controller;
    AcquisitionSettings s;
    s.source = acquisition::SourceKind::Serial; // no port
    controller.setAcquisitionSettings(s);
    CHECK(!controller.start());
    CHECK(!controller.lastError().isEmpty());
    CHECK(controller.state() == QStringLiteral("error"));

    // A port nobody listens to.
    quint16 closed = 0;
    {
        QTcpServer probe;
        REQUIRE(probe.listen(QHostAddress(QHostAddress::LocalHost), 0));
        closed = probe.serverPort();
    }
    s.source = acquisition::SourceKind::TcpClient;
    s.tcp.host = "127.0.0.1";
    s.tcp.port = closed;
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return controller.stateValue() == AcquisitionController::State::Error; }, 8000));
    CHECK(!controller.isRunning());
    CHECK(!controller.lastError().isEmpty());

    s.source = acquisition::SourceKind::Serial;
    s.serial.port = "QACQ_PORT_INEXISTANT";
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return controller.stateValue() == AcquisitionController::State::Error; }, 5000));
    CHECK(controller.lastError().contains(QStringLiteral("Impossible d'ouvrir")));

    s.source = acquisition::SourceKind::Scpi;
    s.scpi.transport = acquisition::ScpiTransport::Visa;
    s.scpi.visaResource = "USB0::0x0957::0x1796::MY123::INSTR";
    controller.setAcquisitionSettings(s);
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return controller.stateValue() == AcquisitionController::State::Error; }, 5000));
    CHECK(controller.lastError().contains(QStringLiteral("VISA")));
    CHECK(!controller.validate({{QStringLiteral("source"), QStringLiteral("serial")}, {QStringLiteral("serial"), QVariantMap{{QStringLiteral("port"), QString()}}}}).isEmpty());
}

TEST_CASE("While acquiring: trigger captures and continuous recording to disk", "[sources]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Session session;
    AcquisitionController controller(&session);
    AcquisitionSettings s = simulated(5000.0, acquisition::DecoderKind::RawBinary);
    s.decoder.raw = {io::SampleType::Float32, io::ByteOrder::LittleEndian, 1.0, 0.0};
    controller.setAcquisitionSettings(s);
    int captures = 0;
    QObject::connect(&controller, &AcquisitionController::triggered, [&captures](const QVariantMap &) { ++captures; });
    controller.setTrigger({{QStringLiteral("enabled"), true}, {QStringLiteral("channel"), 0}, {QStringLiteral("level"), 0.5},
                           {QStringLiteral("edge"), QStringLiteral("rising")}, {QStringLiteral("mode"), QStringLiteral("normal")},
                           {QStringLiteral("length"), 200}, {QStringLiteral("preTrigger"), 0.25}});
    REQUIRE(controller.start());
    REQUIRE(waitUntil([&] { return controller.channelCount() == 2 && total(controller) > 0; }, 5000));
    const QString file = dir.filePath(QStringLiteral("flux.csv"));
    REQUIRE(controller.startRecording(QUrl::fromLocalFile(file), {{QStringLiteral("format"), QStringLiteral("csv")}}));
    CHECK(controller.isRecording());
    REQUIRE(waitUntil([&] { return captures >= 10; }, 8000));
    const QVariantMap last = controller.lastCapture();
    CHECK(last.value(QStringLiteral("length")).toInt() == 200);
    CHECK(last.value(QStringLiteral("channels")).toList().size() == 2);
    CHECK(last.value(QStringLiteral("channels")).toList().at(0).toList().size() == 200);
    const qint64 written = controller.statistics().value(QStringLiteral("recordingFrames")).toLongLong();
    controller.stop();
    CHECK(!controller.isRecording());
    model::CsvImportOptions in;
    in.firstColumnIsTime = true;
    const model::ImportResult csv = model::importCsv(toPath(file), in);
    REQUIRE(csv.ok());
    CHECK(static_cast<qint64>(csv.signal.frameCount()) >= written);
    CHECK(csv.signal.sampleRateHz == 5000.0);
    CHECK(csv.signal.channels.at(0).name == "Voie 1");
}
