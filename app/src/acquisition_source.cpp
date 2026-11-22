#include "qacq/app/acquisition_source.h"

#include "qacq/acquisition/simulator.h"
#include "qacq/app/audio_format.h"
#include "qacq/io/scpi.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QIODevice>
#include <QMediaDevices>
#include <QNetworkDatagram>
#include <QSerialPort>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <utility>

namespace qacq::app
{
using acquisition::AcquisitionSettings;

// ------------------------------------------------------------------------------------------- shared
std::shared_ptr<acquisition::SamplePipe> SourceShared::pipe() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pipe;
}

std::vector<std::string> SourceShared::channelNames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_names;
}

void SourceShared::setPipe(std::shared_ptr<acquisition::SamplePipe> pipe)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pipe = std::move(pipe);
}

void SourceShared::setChannelNames(std::vector<std::string> names)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_names = std::move(names);
}

std::size_t pipeCapacityFrames(std::size_t channels)
{
    // 512 Ki frames, at most 16 Mi values (128 MiB) whatever the channel count.
    const std::size_t maximumValues = std::size_t{1} << 24;
    return std::max<std::size_t>(1024, std::min<std::size_t>(std::size_t{1} << 19, maximumValues / std::max<std::size_t>(1, channels)));
}

// ------------------------------------------------------------------------------------------- base
SourceWorker::SourceWorker(const AcquisitionSettings &settings, std::shared_ptr<SourceShared> shared)
    : m_settings(settings), m_shared(std::move(shared))
{
    m_decoder = acquisition::makeDecoder(acquisition::effectiveDecoder(settings));
}

SourceWorker::~SourceWorker() = default;

void SourceWorker::open()
{
    if (!m_decoder)
    {
        emit failed(QStringLiteral("Format de décodage inutilisable."));
        return;
    }
    QString description, error;
    if (!openDevice(description, error))
    {
        emit failed(error);
        return;
    }
    m_open = true;
    if (!description.isEmpty()) emit opened(description);
}

void SourceWorker::close()
{
    if (!m_open) return;
    closeDevice();
    m_open = false;
    publishCounters();
}

void SourceWorker::consume(const char *data, qsizetype size)
{
    if (size <= 0 || !m_decoder) return;
    m_decoder->decode(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(data), static_cast<std::size_t>(size)), m_block);
    if (m_block.frameCount() > 0 && !m_pipe)
    {
        m_pipe = std::make_shared<acquisition::SamplePipe>(m_block.channelCount, pipeCapacityFrames(m_block.channelCount));
        m_shared->setPipe(m_pipe);
    }
    // A block without frames still goes when it begins a record (a header read, its samples to come): the
    // pipe marks the break before them.
    if (m_pipe && m_block.channelCount == m_pipe->channelCount() && (m_block.frameCount() > 0 || !m_block.discontinuities.empty()))
        m_pipe->push(m_block);
    if (!m_block.channelNames.empty() && m_block.channelNames != m_names)
    {
        m_names = m_block.channelNames;
        m_shared->setChannelNames(m_names);
    }
    m_block.clear();
    publishCounters();
}

void SourceWorker::publishCounters()
{
    if (!m_decoder) return;
    const io::DecoderStatistics &s = m_decoder->statistics();
    m_shared->bytes.store(s.bytes, std::memory_order_relaxed);
    m_shared->frames.store(s.frames, std::memory_order_relaxed);
    m_shared->rejected.store(s.rejected, std::memory_order_relaxed);
    m_shared->checksumErrors.store(s.checksumErrors, std::memory_order_relaxed);
    m_shared->lostFrames.store(s.lostFrames, std::memory_order_relaxed);
    m_shared->overflows.store(s.overflows, std::memory_order_relaxed);
    m_shared->skippedBytes.store(s.skippedBytes, std::memory_order_relaxed);
}

namespace
{
QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

// ------------------------------------------------------------------------------------------- serial
class SerialWorker final : public SourceWorker
{
public:
    using SourceWorker::SourceWorker;

protected:
    bool openDevice(QString &description, QString &error) override
    {
        const acquisition::SerialSettings &s = m_settings.serial;
        m_port = std::make_unique<QSerialPort>();
        m_port->setPortName(qs(s.port));
        m_port->setBaudRate(s.baudRate);
        m_port->setDataBits(static_cast<QSerialPort::DataBits>(std::clamp(s.dataBits, 5, 8)));
        switch (s.parity)
        {
            case acquisition::Parity::None: m_port->setParity(QSerialPort::NoParity); break;
            case acquisition::Parity::Even: m_port->setParity(QSerialPort::EvenParity); break;
            case acquisition::Parity::Odd: m_port->setParity(QSerialPort::OddParity); break;
            case acquisition::Parity::Mark: m_port->setParity(QSerialPort::MarkParity); break;
            case acquisition::Parity::Space: m_port->setParity(QSerialPort::SpaceParity); break;
        }
        m_port->setStopBits(s.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
        switch (s.flow)
        {
            case acquisition::FlowControl::None: m_port->setFlowControl(QSerialPort::NoFlowControl); break;
            case acquisition::FlowControl::Hardware: m_port->setFlowControl(QSerialPort::HardwareControl); break;
            case acquisition::FlowControl::Software: m_port->setFlowControl(QSerialPort::SoftwareControl); break;
        }
        if (!m_port->open(QIODevice::ReadWrite))
        {
            error = QStringLiteral("Impossible d'ouvrir %1 : %2").arg(qs(s.port), m_port->errorString());
            m_port.reset();
            return false;
        }
        m_port->setDataTerminalReady(s.dataTerminalReady);
        QSerialPort *port = m_port.get();
        connect(port, &QSerialPort::readyRead, this, [this, port] { consume(port->readAll()); });
        connect(port, &QSerialPort::errorOccurred, this, [this, port](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::NoError || e == QSerialPort::TimeoutError) return;
            if (e == QSerialPort::ResourceError || e == QSerialPort::PermissionError)
                emit failed(QStringLiteral("Port %1 perdu (carte débranchée ?) : %2").arg(port->portName(), port->errorString()));
            else
                emit warning(QStringLiteral("Port série : %1").arg(port->errorString()));
        });
        description = QStringLiteral("%1 à %2 bauds").arg(qs(s.port)).arg(s.baudRate);
        return true;
    }

    void closeDevice() override
    {
        if (!m_port) return;
        m_port->disconnect(this);
        m_port->close();
        m_port.reset();
    }

private:
    std::unique_ptr<QSerialPort> m_port;
};

// ------------------------------------------------------------------------------------------- TCP
class TcpWorker final : public SourceWorker
{
public:
    using SourceWorker::SourceWorker;

protected:
    bool openDevice(QString &, QString &) override
    {
        const acquisition::TcpSettings &s = m_settings.tcp;
        m_socket = std::make_unique<QTcpSocket>();
        QTcpSocket *socket = m_socket.get();
        const QString where = QStringLiteral("%1:%2").arg(qs(s.host)).arg(s.port);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { consume(socket->readAll()); });
        connect(socket, &QAbstractSocket::connected, this, [this, where] { emit opened(QStringLiteral("TCP %1").arg(where)); });
        connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError e) {
            if (e == QAbstractSocket::RemoteHostClosedError) emit failed(QStringLiteral("L'appareil a fermé la connexion."));
            else emit failed(QStringLiteral("Réseau : %1").arg(socket->errorString()));
        });
        socket->connectToHost(qs(s.host), static_cast<quint16>(s.port));
        QTimer::singleShot(5000, socket, [this, socket, where] {
            if (socket->state() != QAbstractSocket::ConnectedState)
                emit failed(QStringLiteral("Pas de réponse de %1 (adresse, port, pare-feu ?).").arg(where));
        });
        return true; // opened() when connected
    }

    void closeDevice() override
    {
        if (!m_socket) return;
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket.reset();
    }

private:
    std::unique_ptr<QTcpSocket> m_socket;
};

// ------------------------------------------------------------------------------------------- UDP
class UdpWorker final : public SourceWorker
{
public:
    using SourceWorker::SourceWorker;

protected:
    bool openDevice(QString &description, QString &error) override
    {
        const acquisition::UdpSettings &s = m_settings.udp;
        m_socket = std::make_unique<QUdpSocket>();
        const QHostAddress address = s.bindAddress.empty() ? QHostAddress(QHostAddress::AnyIPv4) : QHostAddress(qs(s.bindAddress));
        if (address.isNull())
        {
            error = QStringLiteral("Adresse d'écoute UDP invalide : %1").arg(qs(s.bindAddress));
            m_socket.reset();
            return false;
        }
        if (!m_socket->bind(address, static_cast<quint16>(s.port), QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint))
        {
            error = QStringLiteral("Impossible d'écouter le port UDP %1 : %2").arg(s.port).arg(m_socket->errorString());
            m_socket.reset();
            return false;
        }
        if (!s.multicastGroup.empty() && !m_socket->joinMulticastGroup(QHostAddress(qs(s.multicastGroup))))
            emit warning(QStringLiteral("Impossible de rejoindre le groupe %1 : %2").arg(qs(s.multicastGroup), m_socket->errorString()));
        QUdpSocket *socket = m_socket.get();
        connect(socket, &QUdpSocket::readyRead, this, [this, socket] {
            while (socket->hasPendingDatagrams())
            {
                const QNetworkDatagram datagram = socket->receiveDatagram();
                consume(datagram.data());
            }
        });
        description = QStringLiteral("UDP, port %1").arg(s.port);
        return true;
    }

    void closeDevice() override
    {
        if (!m_socket) return;
        m_socket->disconnect(this);
        m_socket->close();
        m_socket.reset();
    }

private:
    std::unique_ptr<QUdpSocket> m_socket;
};

// ------------------------------------------------------------------------------------------- SCPI
// *IDN?, the setup commands, SYST:ERR? until "0, no error", then the query again and again: each answer
// goes to the SCPI decoder; the next query leaves once the answer is complete (and the interval elapsed).
class ScpiWorker final : public SourceWorker
{
public:
    using SourceWorker::SourceWorker;

protected:
    bool openDevice(QString &, QString &error) override
    {
        const acquisition::ScpiSettings &s = m_settings.scpi;
        if (s.transport == acquisition::ScpiTransport::Visa)
        {
            error = QStringLiteral("La liaison VISA (USB) n'est pas encore disponible : utilisez la connexion réseau de l'instrument.");
            return false;
        }
        m_scpi = dynamic_cast<io::ScpiDecoder *>(decoder());
        m_socket = std::make_unique<QTcpSocket>();
        m_poll = std::make_unique<QTimer>();
        m_poll->setSingleShot(true);
        m_timeout = std::make_unique<QTimer>();
        m_timeout->setSingleShot(true);
        QTcpSocket *socket = m_socket.get();
        connect(socket, &QAbstractSocket::connected, this, [this] {
            m_phase = Phase::Identify;
            send("*IDN?");
            startTimeout();
        });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onData(socket->readAll()); });
        connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError e) {
            if (e == QAbstractSocket::RemoteHostClosedError) emit failed(QStringLiteral("L'instrument a fermé la connexion."));
            else emit failed(QStringLiteral("Instrument : %1").arg(socket->errorString()));
        });
        connect(m_poll.get(), &QTimer::timeout, this, [this] { sendQuery(); });
        connect(m_timeout.get(), &QTimer::timeout, this, [this] { onTimeout(); });
        socket->connectToHost(qs(s.host), static_cast<quint16>(s.port));
        m_phase = Phase::Connecting;
        startTimeout();
        return true; // opened() once identified and set up
    }

    void closeDevice() override
    {
        if (m_poll) m_poll->stop();
        if (m_timeout) m_timeout->stop();
        if (m_socket)
        {
            m_socket->disconnect(this);
            m_socket->abort();
        }
        m_socket.reset();
        m_poll.reset();
        m_timeout.reset();
        m_scpi = nullptr;
    }

private:
    enum class Phase
    {
        Connecting,
        Identify,
        CheckErrors,
        Polling
    };

    void send(const std::string &command)
    {
        if (m_socket) m_socket->write(QByteArray::fromStdString(command + "\n"));
    }

    void startTimeout()
    {
        const double seconds = std::max(0.05, m_settings.scpi.timeoutSeconds);
        if (m_timeout) m_timeout->start(static_cast<int>(std::lround(seconds * 1000.0)));
    }

    void onData(const QByteArray &bytes)
    {
        if (m_phase == Phase::Polling)
        {
            consume(bytes);
            // The next query once this answer is wholly received: never two waveforms on the way.
            if (m_scpi && m_scpi->answers() > m_askedAt && m_scpi->betweenAnswers() && !m_poll->isActive())
            {
                m_timeout->stop();
                m_poll->start(static_cast<int>(std::lround(std::max(0.0, m_settings.scpi.intervalSeconds) * 1000.0)));
            }
            return;
        }
        m_line += bytes;
        for (;;)
        {
            const qsizetype end = m_line.indexOf('\n');
            if (end < 0) break;
            const QByteArray line = m_line.left(end).trimmed();
            m_line.remove(0, end + 1);
            handleLine(line);
            if (m_phase == Phase::Polling)
            {
                if (!m_line.isEmpty()) onData(std::exchange(m_line, QByteArray()));
                break;
            }
        }
    }

    void handleLine(const QByteArray &line)
    {
        if (m_phase == Phase::Identify)
        {
            m_timeout->stop();
            emit identified(QString::fromUtf8(line));
            for (const std::string &command : m_settings.scpi.setupCommands)
                if (!command.empty()) send(command);
            if (m_settings.scpi.checkErrors)
            {
                m_phase = Phase::CheckErrors;
                m_errorReads = 0;
                send("SYST:ERR?");
                startTimeout();
            }
            else
            {
                startPolling();
            }
        }
        else if (m_phase == Phase::CheckErrors)
        {
            m_timeout->stop();
            const auto error = io::parseScpiError(line.toStdString());
            if (error && error->code != 0 && ++m_errorReads < 20)
            {
                emit warning(QStringLiteral("L'instrument signale l'erreur %1 : %2").arg(error->code).arg(qs(error->message)));
                send("SYST:ERR?");
                startTimeout();
                return;
            }
            startPolling();
        }
    }

    void startPolling()
    {
        m_phase = Phase::Polling;
        emit opened(QStringLiteral("Instrument %1:%2").arg(qs(m_settings.scpi.host)).arg(m_settings.scpi.port));
        sendQuery();
    }

    void sendQuery()
    {
        m_askedAt = m_scpi ? m_scpi->answers() : 0;
        send(m_settings.scpi.query);
        startTimeout();
    }

    void onTimeout()
    {
        switch (m_phase)
        {
            case Phase::Connecting:
                emit failed(QStringLiteral("Pas de réponse de l'instrument %1:%2 (adresse, port 5025, pare-feu ?).")
                                .arg(qs(m_settings.scpi.host))
                                .arg(m_settings.scpi.port));
                break;
            case Phase::Identify:
                emit failed(QStringLiteral("L'instrument ne répond pas à *IDN? : est-ce bien un instrument SCPI ?"));
                break;
            case Phase::CheckErrors: startPolling(); break;
            case Phase::Polling:
            {
                const std::uint64_t count = ++m_timeouts;
                if (count == 1)
                    emit warning(QStringLiteral("L'instrument n'a pas répondu à « %1 » à temps : la requête est renvoyée.")
                                     .arg(qs(m_settings.scpi.query)));
                sendQuery();
                break;
            }
        }
        reportTimeouts();
    }

    void reportTimeouts();

    std::unique_ptr<QTcpSocket> m_socket;
    std::unique_ptr<QTimer> m_poll;
    std::unique_ptr<QTimer> m_timeout;
    io::ScpiDecoder *m_scpi = nullptr;
    Phase m_phase = Phase::Connecting;
    QByteArray m_line;
    int m_errorReads = 0;
    std::uint64_t m_timeouts = 0;
    std::uint64_t m_askedAt = 0; // answers begun when the last query left
};

void ScpiWorker::reportTimeouts()
{
    shared().timeouts.store(m_timeouts, std::memory_order_relaxed);
}

// ------------------------------------------------------------------------------------------- sound card
class AudioWorker final : public SourceWorker
{
public:
    using SourceWorker::SourceWorker;

protected:
    bool openDevice(QString &description, QString &error) override
    {
        const acquisition::AudioSettings &a = m_settings.audio;
        QAudioDevice device = QMediaDevices::defaultAudioInput();
        const QString wanted = qs(a.device);
        if (!wanted.isEmpty())
        {
            bool found = false;
            const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
            for (const QAudioDevice &d : inputs)
            {
                if (d.description() == wanted || QString::fromUtf8(d.id()) == wanted)
                {
                    device = d;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                error = QStringLiteral("Entrée audio « %1 » introuvable.").arg(wanted);
                return false;
            }
        }
        if (device.isNull())
        {
            error = QStringLiteral("Aucune entrée audio sur ce PC.");
            return false;
        }
        const QAudioFormat format = audioFormatFor(a);
        if (!device.isFormatSupported(format))
        {
            const QAudioFormat preferred = device.preferredFormat();
            error = QStringLiteral("« %1 » n'accepte pas %2 Hz, %3 voie(s) dans ce format ; il propose %4 Hz, %5 voie(s).")
                        .arg(device.description())
                        .arg(a.sampleRate)
                        .arg(a.channels)
                        .arg(preferred.sampleRate())
                        .arg(preferred.channelCount());
            return false;
        }
        m_audio = std::make_unique<QAudioSource>(device, format);
        m_io = m_audio->start();
        if (!m_io)
        {
            error = QStringLiteral("Impossible de démarrer l'entrée audio « %1 ».").arg(device.description());
            m_audio.reset();
            return false;
        }
        QIODevice *io = m_io;
        connect(io, &QIODevice::readyRead, this, [this, io] { consume(io->readAll()); });
        QAudioSource *audio = m_audio.get();
        connect(audio, &QAudioSource::stateChanged, this, [this, audio](QAudio::State state) {
            if (state == QAudio::StoppedState && audio->error() != QAudio::NoError)
                emit failed(QStringLiteral("L'entrée audio s'est arrêtée (périphérique retiré ?)."));
        });
        description = QStringLiteral("%1, %2 Hz, %3 voie(s)").arg(device.description()).arg(a.sampleRate).arg(a.channels);
        return true;
    }

    void closeDevice() override
    {
        if (m_audio)
        {
            m_audio->disconnect(this);
            m_audio->stop();
        }
        m_io = nullptr;
        m_audio.reset();
    }

private:
    std::unique_ptr<QAudioSource> m_audio;
    QIODevice *m_io = nullptr; // owned by m_audio
};

// ------------------------------------------------------------------------------------------- simulator
class SimulatedWorker final : public SourceWorker
{
public:
    using SourceWorker::SourceWorker;

protected:
    bool openDevice(QString &description, QString &) override
    {
        m_device = std::make_unique<acquisition::SimulatedDevice>(m_settings.simulated, acquisition::effectiveDecoder(m_settings),
                                                                  m_settings.declaredSampleRateHz);
        m_timer = std::make_unique<QTimer>();
        m_timer->setTimerType(Qt::PreciseTimer);
        m_timer->setInterval(10);
        m_clock.start();
        m_last = 0;
        connect(m_timer.get(), &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.nsecsElapsed();
            const double seconds = static_cast<double>(now - m_last) * 1e-9;
            m_last = now;
            const std::vector<std::uint8_t> bytes = m_device->produceFor(seconds);
            consume(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size()));
        });
        m_timer->start();
        description = QStringLiteral("Simulateur, %1 voie(s) à %2 Hz")
                          .arg(static_cast<int>(m_device->channelCount()))
                          .arg(m_settings.declaredSampleRateHz);
        return true;
    }

    void closeDevice() override
    {
        if (m_timer) m_timer->stop();
        m_timer.reset();
        m_device.reset();
    }

private:
    std::unique_ptr<acquisition::SimulatedDevice> m_device;
    std::unique_ptr<QTimer> m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
};
} // namespace

std::unique_ptr<SourceWorker> makeSourceWorker(const AcquisitionSettings &settings, std::shared_ptr<SourceShared> shared,
                                               QString *error)
{
    const std::string problem = acquisition::validateSettings(settings);
    if (!problem.empty())
    {
        if (error) *error = qs(problem);
        return nullptr;
    }
    switch (settings.source)
    {
        case acquisition::SourceKind::Serial: return std::make_unique<SerialWorker>(settings, shared);
        case acquisition::SourceKind::TcpClient: return std::make_unique<TcpWorker>(settings, shared);
        case acquisition::SourceKind::Udp: return std::make_unique<UdpWorker>(settings, shared);
        case acquisition::SourceKind::Scpi: return std::make_unique<ScpiWorker>(settings, shared);
        case acquisition::SourceKind::Audio: return std::make_unique<AudioWorker>(settings, shared);
        case acquisition::SourceKind::Simulated: return std::make_unique<SimulatedWorker>(settings, shared);
    }
    if (error) *error = QStringLiteral("Source inconnue.");
    return nullptr;
}
} // namespace qacq::app
