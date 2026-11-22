#include "qacq/app/audio_player.h"

#include "qacq/app/app_settings.h"
#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"

#include <QAudioSink>
#include <QCoreApplication>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

namespace qacq::app
{
// The bytes to play, read by the sink (pull mode), from the start again in a loop. They do not change while
// they play; where the reading is stays atomic (a sink may read from a thread of its own).
class PlaybackDevice final : public QIODevice
{
public:
    PlaybackDevice(std::shared_ptr<const std::vector<std::uint8_t>> bytes, bool loop)
        : m_bytes(std::move(bytes)), m_loop(loop)
    {
    }

    void setLoop(bool loop) { m_loop.store(loop); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        const auto size = static_cast<qint64>(m_bytes->size());
        return (m_loop.load() ? size : size - m_at.load()) + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const auto size = static_cast<qint64>(m_bytes->size());
        qint64 done = 0;
        while (done < maxSize && size > 0)
        {
            qint64 at = m_at.load();
            if (at >= size)
            {
                if (!m_loop.load()) break;
                at = 0;
            }
            const qint64 chunk = std::min(maxSize - done, size - at);
            std::memcpy(data + done, m_bytes->data() + at, static_cast<std::size_t>(chunk));
            done += chunk;
            m_at.store(at + chunk);
        }
        return done; // 0 at the end: the sink goes idle
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes;
    std::atomic<qint64> m_at{0};
    std::atomic<bool> m_loop;
};

namespace
{
analysis::SampleFormat sampleFormatOf(QAudioFormat::SampleFormat format)
{
    switch (format)
    {
        case QAudioFormat::UInt8: return analysis::SampleFormat::UInt8;
        case QAudioFormat::Int16: return analysis::SampleFormat::Int16;
        case QAudioFormat::Int32: return analysis::SampleFormat::Int32;
        default: return analysis::SampleFormat::Float32;
    }
}
} // namespace

AudioPlayer::AudioPlayer(AppSettings *settings, QObject *parent) : QObject(parent), m_settings(settings)
{
    m_pool.setMaxThreadCount(1);
    m_clock.setInterval(50);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        if (!m_sink) return;
        const double played = static_cast<double>(m_sink->processedUSecs()) / 1e6;
        const double at = m_duration <= 0.0 ? 0.0 : m_loop ? std::fmod(played, m_duration) : std::min(played, m_duration);
        if (at == m_position) return;
        m_position = at;
        emit positionChanged();
    });
    if (settings)
    {
        m_device = settings->value(QStringLiteral("audio/output"), QString()).toString();
        m_loop = settings->value(QStringLiteral("audio/loop"), false).toBool();
        m_volume = std::clamp(settings->value(QStringLiteral("audio/volume"), 0.8).toDouble(), 0.0, 1.0);
        m_normalized = settings->value(QStringLiteral("audio/normalized"), true).toBool();
    }
}

AudioPlayer::~AudioPlayer()
{
    ++m_ticket;
    if (m_cancel) m_cancel->store(true);
    m_clock.stop();
    if (m_sink)
    {
        m_sink->disconnect(this);
        m_sink->stop();
    }
    m_sink.reset();
    m_bytes.reset();
    m_pool.waitForDone();
}

void AudioPlayer::setDevice(const QString &device)
{
    if (device == m_device) return;
    m_device = device;
    save();
    emit settingsChanged();
    restart();
}

void AudioPlayer::setLoop(bool loop)
{
    if (loop == m_loop) return;
    m_loop = loop;
    if (m_bytes) m_bytes->setLoop(loop);
    save();
    emit settingsChanged();
}

void AudioPlayer::setVolume(double volume)
{
    const double v = std::isfinite(volume) ? std::clamp(volume, 0.0, 1.0) : m_volume;
    if (v == m_volume) return;
    m_volume = v;
    if (m_sink) m_sink->setVolume(v);
    save();
    emit settingsChanged();
}

void AudioPlayer::setNormalized(bool normalized)
{
    if (normalized == m_normalized) return;
    m_normalized = normalized;
    save();
    emit settingsChanged();
    restart();
}

void AudioPlayer::refreshDevices()
{
    QStringList names;
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice &d : outputs) names << d.description();
    if (names == m_devices) return;
    m_devices = names;
    emit devicesChanged();
}

bool AudioPlayer::play(QObject *sessionObject, qint64 signalId, const QVariantList &channels)
{
    stop();
    auto *session = qobject_cast<Session *>(sessionObject);
    const model::SignalPtr signal = session ? session->signalList()->signalById(signalId) : model::SignalPtr();
    if (!signal)
    {
        fail(QStringLiteral("Pas de signal à écouter."));
        return false;
    }
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!m_device.isEmpty())
    {
        bool found = false;
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice &d : outputs)
        {
            if (d.description() != m_device) continue;
            device = d;
            found = true;
            break;
        }
        if (!found)
        {
            fail(QStringLiteral("La sortie audio « %1 » n'est plus là : choisissez-en une autre (Son ▾).").arg(m_device));
            return false;
        }
    }
    if (device.isNull())
    {
        fail(QStringLiteral("Aucune sortie audio sur ce PC."));
        return false;
    }
    QAudioFormat format = device.preferredFormat();
    if (format.sampleFormat() == QAudioFormat::Unknown || format.sampleRate() <= 0 || format.channelCount() <= 0)
    {
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
    }
    analysis::PlaybackRequest request;
    request.channels.clear();
    for (const QVariant &v : channels)
    {
        bool ok = false;
        const int c = v.toInt(&ok);
        if (ok && c >= 0) request.channels.push_back(static_cast<std::size_t>(c));
    }
    request.outputRateHz = format.sampleRate();
    request.outputChannels = static_cast<std::size_t>(format.channelCount());
    request.normalized = m_normalized;
    request.periodic = m_loop;

    m_source = sessionObject;
    m_sourceId = signalId;
    m_signalName = toQString(signal->name);
    m_channels = channels;
    m_error.clear();
    m_preparing = true;
    const quint64 ticket = ++m_ticket;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_cancel = cancel;
    emit stateChanged();
    const analysis::SampleFormat sampleFormat = sampleFormatOf(format.sampleFormat());
    m_pool.start([this, signal, request, sampleFormat, device, format, ticket, cancel] {
        model::Progress progress;
        progress.cancelled = cancel.get();
        auto buffer = std::make_shared<analysis::PlaybackBuffer>(analysis::preparePlayback(*signal, request, progress));
        auto bytes = std::make_shared<const std::vector<std::uint8_t>>(
            buffer->ok() ? analysis::encodeSamples(buffer->samples, sampleFormat) : std::vector<std::uint8_t>());
        buffer->samples = std::vector<float>();
        QMetaObject::invokeMethod(
            this, [this, buffer, bytes, device, format, ticket] { prepared(buffer, bytes, device, format, ticket); },
            Qt::QueuedConnection);
    });
    return true;
}

void AudioPlayer::prepared(const std::shared_ptr<analysis::PlaybackBuffer> &buffer,
                           const std::shared_ptr<const std::vector<std::uint8_t>> &bytes, const QAudioDevice &device,
                           const QAudioFormat &format, quint64 ticket)
{
    if (ticket != m_ticket) return; // stopped, or something else asked meanwhile
    m_cancel.reset();
    m_preparing = false;
    if (!buffer->ok())
    {
        m_source = nullptr;
        m_sourceId = 0;
        fail(toQString(buffer->error));
        return;
    }
    m_bytes = std::make_unique<PlaybackDevice>(bytes, m_loop);
    m_bytes->open(QIODevice::ReadOnly);
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setVolume(m_volume);
    connect(m_sink.get(), &QAudioSink::stateChanged, this, [this](QAudio::State) { sinkStateChanged(); });
    m_sink->start(m_bytes.get());
    if (m_sink->error() != QAudio::NoError)
    {
        const QString name = device.description();
        stop();
        fail(QStringLiteral("La sortie audio « %1 » ne s'ouvre pas.").arg(name));
        return;
    }
    m_duration = buffer->seconds;
    m_position = 0.0;
    m_playing = true;
    const QString level = m_normalized ? QStringLiteral("normalisé (%1)").arg(formatQuantity(buffer->gainDb, QStringLiteral("dB")))
                                       : QStringLiteral("tel quel");
    m_description = QStringLiteral("« %1 » sur « %2 » · %3 · %4 · %5%6")
                        .arg(m_signalName, device.description(), formatQuantity(format.sampleRate(), QStringLiteral("Hz")),
                             m_channels.size() >= 2 ? QStringLiteral("stéréo") : QStringLiteral("mono"), level,
                             m_loop ? QStringLiteral(" · en boucle") : QString());
    m_clock.start();
    emit stateChanged();
    emit positionChanged();
    QString text = QStringLiteral("Écoute : %1.").arg(m_description);
    for (const std::string &w : buffer->warnings) text += QStringLiteral(" ") + toQString(w);
    emit started(text);
}

void AudioPlayer::sinkStateChanged()
{
    if (!m_sink) return;
    const QAudio::State state = m_sink->state();
    if (state == QAudio::IdleState && !m_loop)
    {
        stop(); // the end
        return;
    }
    if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError)
    {
        stop();
        fail(QStringLiteral("La sortie audio s'est arrêtée (périphérique retiré ?)."));
    }
}

void AudioPlayer::stop()
{
    ++m_ticket;
    if (m_cancel)
    {
        m_cancel->store(true); // a preparation in progress stops early: the next one does not wait for it
        m_cancel.reset();
    }
    const bool was = m_playing || m_preparing || m_sourceId != 0;
    m_clock.stop();
    if (m_sink)
    {
        // Deleted later: this may be called from a signal of the sink.
        m_sink->disconnect(this);
        m_sink->stop();
        m_sink.release()->deleteLater();
    }
    if (m_bytes)
    {
        m_bytes->close();
        m_bytes.release()->deleteLater();
    }
    m_playing = false;
    m_preparing = false;
    m_position = 0.0;
    m_description.clear();
    m_signalName.clear();
    m_source = nullptr;
    m_sourceId = 0;
    if (!was) return;
    emit stateChanged();
    emit positionChanged();
}

void AudioPlayer::fail(const QString &error)
{
    m_error = error;
    emit stateChanged();
    emit failed(error);
}

void AudioPlayer::restart()
{
    if (!m_playing && !m_preparing) return;
    const QPointer<QObject> source = m_source;
    const qint64 id = m_sourceId;
    const QVariantList channels = m_channels;
    play(source.data(), id, channels);
}

void AudioPlayer::save()
{
    if (!m_settings) return;
    m_settings->setValue(QStringLiteral("audio/output"), m_device);
    m_settings->setValue(QStringLiteral("audio/loop"), m_loop);
    m_settings->setValue(QStringLiteral("audio/volume"), m_volume);
    m_settings->setValue(QStringLiteral("audio/normalized"), m_normalized);
}

void AudioPlayer::waitForPrepared()
{
    m_pool.waitForDone();
    QCoreApplication::processEvents(); // the result, sent back to this thread
}
} // namespace qacq::app
