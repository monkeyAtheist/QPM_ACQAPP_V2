#include "qacq/app/acquisition_controller.h"

#include "qacq/acquisition/firmware_example.h"
#include "qacq/app/acquisition_source.h"
#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"

#include <QAudioDevice>
#include <QDateTime>
#include <QMediaDevices>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace qacq::app
{
using acquisition::AcquisitionSettings;

AcquisitionController::AcquisitionController(Session *session, QObject *parent) : QObject(parent), m_session(session)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
    m_status = QStringLiteral("Prêt");
}

AcquisitionController::~AcquisitionController()
{
    // Leaving: nothing is added to a session that may already be gone.
    m_timer.stop();
    shutdownWorker();
    m_writer.close();
}

QString AcquisitionController::state() const
{
    switch (m_state)
    {
        case State::Idle: return QStringLiteral("idle");
        case State::Connecting: return QStringLiteral("connecting");
        case State::Running: return QStringLiteral("running");
        case State::Error: return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

QVariantMap AcquisitionController::settings() const
{
    return jsonToVariant(acquisition::toJson(m_settings)).toMap();
}

void AcquisitionController::setSettings(const QVariantMap &settings)
{
    AcquisitionSettings next = m_settings;
    std::string error;
    if (!acquisition::fromJson(variantToJson(settings), next, &error)) addWarning(toQString(error));
    // A name left from another source becomes that of this one (a voice recorded on the sound card is not an
    // « Essai du simulateur »), also in settings saved before.
    next.name = acquisition::nameForSource(next.name, next.source);
    setAcquisitionSettings(next);
}

void AcquisitionController::setAcquisitionSettings(const AcquisitionSettings &settings)
{
    m_settings = settings;
    emit settingsChanged();
    if (!isRunning()) updateChannelNames();
}

void AcquisitionController::setRecorderLimits(const acquisition::RecorderLimits &limits)
{
    m_limits = limits;
    m_recorder.setLimits(m_limits, effectiveRate());
}

void AcquisitionController::setPollInterval(int milliseconds)
{
    m_pollMilliseconds = std::max(1, milliseconds);
    if (m_timer.isActive()) m_timer.start(m_pollMilliseconds);
}

double AcquisitionController::measuredRate() const
{
    return m_rate.rateHz();
}

double AcquisitionController::effectiveRate() const
{
    const AcquisitionSettings &s = isRunning() || m_recorder.totalFrames() > 0 ? m_running : m_settings;
    // The sound card's own rate first: a rate announced for another source may be left in the settings.
    const double announced = acquisition::announcedRate(s);
    return announced > 0.0 ? announced : m_rate.rateHz();
}

QVariantMap AcquisitionController::statistics() const
{
    QVariantMap m;
    auto count = [](std::uint64_t v) { return QVariant::fromValue(static_cast<qint64>(v)); };
    if (m_shared)
    {
        m.insert(QStringLiteral("bytes"), count(m_shared->bytes.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("frames"), count(m_shared->frames.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("rejected"), count(m_shared->rejected.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("checksumErrors"), count(m_shared->checksumErrors.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("lostFrames"), count(m_shared->lostFrames.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("overflows"), count(m_shared->overflows.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("skippedBytes"), count(m_shared->skippedBytes.load(std::memory_order_relaxed)));
        m.insert(QStringLiteral("timeouts"), count(m_shared->timeouts.load(std::memory_order_relaxed)));
    }
    m.insert(QStringLiteral("droppedFrames"), count(m_pipe ? m_pipe->droppedFrames() : 0));
    m.insert(QStringLiteral("storedFrames"), count(m_recorder.storedFrames()));
    m.insert(QStringLiteral("totalFrames"), count(m_recorder.totalFrames()));
    m.insert(QStringLiteral("refusedFrames"), count(m_recorder.refusedFrames()));
    m.insert(QStringLiteral("memoryFull"), m_recorder.full());
    m.insert(QStringLiteral("memoryBytes"), count(m_recorder.byteSize()));
    m.insert(QStringLiteral("memoryText"), formatBytes(static_cast<qint64>(m_recorder.byteSize())));
    const double rate = effectiveRate();
    m.insert(QStringLiteral("sampleRate"), rate);
    m.insert(QStringLiteral("measuredRate"), m_rate.rateHz());
    m.insert(QStringLiteral("measuredRateUncertainty"), m_rate.relativeUncertainty());
    m.insert(QStringLiteral("bytesPerSecond"), m_byteRate.rateHz());
    m.insert(QStringLiteral("elapsedSeconds"), m_clock.isValid() ? static_cast<double>(m_clock.elapsed()) / 1000.0 : 0.0);
    m.insert(QStringLiteral("recordedSeconds"), rate > 0.0 ? static_cast<double>(m_recorder.storedFrames()) / rate : 0.0);
    m.insert(QStringLiteral("recordingFrames"), count(m_writer.framesWritten()));
    m.insert(QStringLiteral("rateText"), rate > 0.0 ? formatQuantity(rate, QStringLiteral("Hz")) : QStringLiteral("—"));
    return m;
}

void AcquisitionController::setState(State state, const QString &status)
{
    m_state = state;
    m_status = status;
    emit stateChanged();
}

void AcquisitionController::addWarning(const QString &message)
{
    if (message.isEmpty() || m_warnings.contains(message)) return;
    m_warnings.append(message);
    while (m_warnings.size() > 20) m_warnings.removeFirst();
    emit warningsChanged();
}

void AcquisitionController::updateChannelNames()
{
    const AcquisitionSettings &s = isRunning() || m_recorder.totalFrames() > 0 ? m_running : m_settings;
    std::size_t count = m_pipe ? m_pipe->channelCount() : acquisition::effectiveDecoder(s).channelCount();
    if (s.source == acquisition::SourceKind::Simulated) count = s.simulated.channels.size();
    const std::vector<std::string> streamNames = m_shared ? m_shared->channelNames() : std::vector<std::string>();
    QStringList names, units;
    for (std::size_t c = 0; c < count; ++c)
    {
        QString name;
        if (c < s.channels.size() && !s.channels[c].name.empty()) name = toQString(s.channels[c].name);
        else if (c < streamNames.size() && !streamNames[c].empty()) name = toQString(streamNames[c]);
        else name = QStringLiteral("Voie %1").arg(static_cast<int>(c + 1));
        names << name;
        units << (c < s.channels.size() ? toQString(s.channels[c].unit) : QString());
    }
    if (names != m_channelNames || units != m_channelUnits)
    {
        m_channelNames = names;
        m_channelUnits = units;
        emit channelsChanged();
    }
}

bool AcquisitionController::start()
{
    if (isRunning()) return false;
    shutdownWorker();
    m_lastError.clear();
    emit lastErrorChanged();
    m_warnings.clear();
    emit warningsChanged();

    auto shared = std::make_shared<SourceShared>();
    QString error;
    std::unique_ptr<SourceWorker> worker = makeSourceWorker(m_settings, shared, &error);
    if (!worker)
    {
        m_lastError = error;
        emit lastErrorChanged();
        setState(State::Error, error);
        return false;
    }
    m_running = m_settings;
    m_shared = std::move(shared);
    m_worker = std::move(worker);
    m_pipe.reset();
    m_recorder.reset(1);
    m_rate.reset();
    m_byteRate.reset();
    m_trigger.reset();
    m_haveCapture = false;
    m_captureCount = 0;
    emit captureCountChanged();
    m_pulled = 0;
    m_instrument.clear();
    emit instrumentChanged();
    m_lastSignalId = 0;
    m_startedUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("Acquisition"));
    SourceWorker *w = m_worker.get();
    w->moveToThread(m_thread);
    const quint64 generation = ++m_generation;
    connect(m_thread, &QThread::started, w, [w] { w->open(); });
    connect(w, &SourceWorker::opened, this, [this, generation](const QString &d) {
        if (generation == m_generation) onOpened(d);
    });
    connect(w, &SourceWorker::failed, this, [this, generation](const QString &m) {
        if (generation == m_generation) onFailed(m);
    });
    connect(w, &SourceWorker::warning, this, [this, generation](const QString &m) {
        if (generation == m_generation) addWarning(m);
    });
    connect(w, &SourceWorker::identified, this, [this, generation](const QString &identity) {
        if (generation != m_generation) return;
        m_instrument = identity;
        emit instrumentChanged();
    });

    m_clock.start();
    setState(State::Connecting, QStringLiteral("Connexion à la source…"));
    updateChannelNames();
    m_thread->start();
    m_timer.start(m_pollMilliseconds);
    return true;
}

void AcquisitionController::onOpened(const QString &description)
{
    setState(State::Running, QStringLiteral("Acquisition : %1").arg(description));
}

void AcquisitionController::onFailed(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
    stop(true); // what was received before the failure is kept
    setState(State::Error, message);
}

void AcquisitionController::shutdownWorker()
{
    ++m_generation;
    if (m_worker && m_thread && m_thread->isRunning())
    {
        SourceWorker *w = m_worker.get();
        QMetaObject::invokeMethod(w, [w] { w->close(); }, Qt::BlockingQueuedConnection);
    }
    if (m_thread)
    {
        m_thread->quit();
        m_thread->wait();
    }
    m_worker.reset(); // its devices were destroyed in its thread by close()
    delete m_thread;
    m_thread = nullptr;
}

qint64 AcquisitionController::stop(bool keep)
{
    if (!m_worker && !m_thread) return 0;
    m_timer.stop();
    shutdownWorker();
    poll(); // the last frames in the pipe
    stopRecording();
    const qint64 id = keep ? keepSignal() : 0;
    setState(State::Idle, m_recorder.totalFrames() > 0 ? QStringLiteral("Arrêtée : %1 trames reçues").arg(formatCount(static_cast<qint64>(m_recorder.totalFrames())))
                                                       : QStringLiteral("Arrêtée"));
    emit statisticsChanged();
    return id;
}

void AcquisitionController::clear()
{
    if (!m_pipe) return;
    m_recorder.reset(m_pipe->channelCount());
    m_recorder.setLimits(m_limits, effectiveRate());
    m_trigger.reset();
    m_pulled = 0;
    emit statisticsChanged();
}

void AcquisitionController::poll()
{
    if (!m_shared) return;
    if (!m_pipe)
    {
        m_pipe = m_shared->pipe();
        if (m_pipe)
        {
            m_recorder.reset(m_pipe->channelCount());
            m_recorder.setLimits(m_limits, effectiveRate());
            updateChannelNames();
        }
    }
    const double now = m_clock.isValid() ? static_cast<double>(m_clock.nsecsElapsed()) * 1e-9 : 0.0;
    if (m_pipe)
    {
        m_values.clear();
        m_breaks.clear();
        const std::size_t frames = m_pipe->pull(m_values, m_breaks);
        if (frames > 0)
        {
            if (m_recording)
            {
                const std::string error = m_writer.append(m_values);
                if (!error.empty())
                {
                    addWarning(toQString(error));
                    stopRecording();
                }
            }
            const bool hadRate = m_recorder.totalFrames() > 0;
            m_recorder.append(m_values, m_breaks);
            if (!hadRate) m_recorder.setLimits(m_limits, effectiveRate());
            if (m_triggerEnabled)
            {
                const std::size_t channels = m_pipe->channelCount();
                const std::size_t c = std::min(m_trigger.settings().channel, channels - 1);
                m_triggerSamples.resize(frames);
                for (std::size_t f = 0; f < frames; ++f) m_triggerSamples[f] = m_values[f * channels + c];
                if (!m_breaks.empty()) m_trigger.reset(); // a gap: the edge before it does not count
                for (const acquisition::TriggerCapture &capture : m_trigger.process(m_triggerSamples, m_pulled))
                {
                    m_lastCapture = capture;
                    m_haveCapture = true;
                    ++m_captureCount;
                    emit captureCountChanged();
                    emit triggered(captureToMap(capture, false));
                    if (m_trigger.settings().mode == acquisition::TriggerMode::Single) emit triggerChanged();
                }
            }
            m_pulled += frames;
            emit framesArrived(static_cast<qint64>(m_pulled));
        }
    }
    m_rate.add(now, m_shared->frames.load(std::memory_order_relaxed));
    m_byteRate.add(now, m_shared->bytes.load(std::memory_order_relaxed));
    const std::vector<std::string> names = m_shared->channelNames();
    if (!names.empty()) updateChannelNames();
    emit statisticsChanged();
}

qint64 AcquisitionController::keepSignal()
{
    if (!m_session || m_recorder.storedFrames() == 0) return 0;
    updateChannelNames();
    model::Signal signal;
    const QString base = m_running.name.empty() ? QStringLiteral("Acquisition") : toQString(m_running.name);
    signal.name = toUtf8(m_session->signalList()->uniqueName(base));
    signal.origin = model::Origin::Acquired;
    const double rate = effectiveRate();
    signal.sampleRateHz = rate > 0.0 ? rate : 1.0;
    signal.createdUtc = toUtf8(m_startedUtc);
    const std::size_t channels = m_recorder.channelCount();
    for (std::size_t c = 0; c < channels; ++c)
    {
        model::ChannelInfo info;
        info.name = c < static_cast<std::size_t>(m_channelNames.size()) ? toUtf8(m_channelNames.at(static_cast<qsizetype>(c)))
                                                                         : "Voie " + std::to_string(c + 1);
        if (c < m_running.channels.size()) info.unit = m_running.channels[c].unit;
        signal.channels.push_back(info);
    }
    signal.data = m_recorder.snapshot();
    signal.provenance = acquisition::toJson(m_running);
    // (A rate announced for the other sources does not apply to the sound card: not kept with what it recorded.)
    if (m_running.source == acquisition::SourceKind::Audio) signal.provenance.erase("declaredSampleRateHz");
    signal.provenance["statistics"] = variantToJson(statistics());
    if (!m_instrument.isEmpty()) signal.provenance["instrument"] = toUtf8(m_instrument);

    QStringList notes;
    const bool card = m_running.source == acquisition::SourceKind::Audio;
    const double announced = acquisition::announcedRate(m_running);
    const double measured = m_rate.rateHz();
    if (card)
    {
        // Its own rate: said only if the samples did not come at it.
        if (measured > 0.0 && announced > 0.0 && std::abs(measured / announced - 1.0) > 0.01)
            notes << QStringLiteral("Carte son à %1, cadence mesurée %2 : des échantillons ont pu manquer.")
                         .arg(formatQuantity(announced, QStringLiteral("Hz"), 6), formatQuantity(measured, QStringLiteral("Hz"), 6));
    }
    else if (announced > 0.0 && measured > 0.0)
        notes << QStringLiteral("Fréquence déclarée %1, mesurée %2.")
                     .arg(formatQuantity(announced, QStringLiteral("Hz"), 6), formatQuantity(measured, QStringLiteral("Hz"), 6));
    else if (!(rate > 0.0))
        notes << QStringLiteral("Fréquence d'échantillonnage inconnue : 1 Hz par défaut, à corriger.");
    else if (announced <= 0.0)
        notes << QStringLiteral("Fréquence mesurée sur le flux : %1.").arg(formatQuantity(rate, QStringLiteral("Hz"), 6));
    if (m_recorder.full())
        notes << QStringLiteral("Mémoire pleine : %1 trames reçues après la limite n'ont pas été gardées.")
                     .arg(formatCount(static_cast<qint64>(m_recorder.refusedFrames())));
    if (m_recorder.firstFrame() > 0)
        notes << QStringLiteral("Fenêtre glissante : les %1 premières trames ont été oubliées.")
                     .arg(formatCount(static_cast<qint64>(m_recorder.firstFrame())));
    const std::uint64_t lost = m_shared ? m_shared->lostFrames.load() : 0;
    const std::uint64_t dropped = m_pipe ? m_pipe->droppedFrames() : 0;
    if (lost + dropped > 0)
        notes << QStringLiteral("%1 trame(s) perdue(s) en route, marquées comme ruptures.")
                     .arg(formatCount(static_cast<qint64>(lost + dropped)));
    signal.notes = toUtf8(notes.join(QStringLiteral(" ")));

    QString error;
    const qint64 id = m_session->signalList()->addSignal(std::move(signal), QStringLiteral("Acquisition « %1 »").arg(base), &error);
    if (id == 0) addWarning(error);
    m_lastSignalId = id;
    emit lastSignalIdChanged();
    return id;
}

bool AcquisitionController::startRecording(const QUrl &file, const QVariantMap &options)
{
    if (m_recording) stopRecording();
    if (!isRunning() || !m_pipe)
    {
        addWarning(QStringLiteral("L'enregistrement continu démarre une fois les premières données reçues."));
        return false;
    }
    acquisition::StreamOptions o;
    o.format = options.value(QStringLiteral("format")).toString() == QStringLiteral("wav") ? acquisition::StreamFormat::Wav
                                                                                          : acquisition::StreamFormat::Csv;
    o.decimalComma = options.value(QStringLiteral("decimalComma"), false).toBool();
    o.timeColumn = options.value(QStringLiteral("timeColumn"), true).toBool();
    o.wavFloat = options.value(QStringLiteral("wavFloat"), true).toBool();
    o.fullScale = options.value(QStringLiteral("fullScale"), 1.0).toDouble();
    std::vector<model::ChannelInfo> channels;
    for (qsizetype c = 0; c < m_channelNames.size(); ++c)
    {
        model::ChannelInfo info{toUtf8(m_channelNames.at(c)), {}};
        if (static_cast<std::size_t>(c) < m_running.channels.size()) info.unit = m_running.channels[static_cast<std::size_t>(c)].unit;
        channels.push_back(info);
    }
    const QString local = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const std::string error = m_writer.open(toPath(local), channels, effectiveRate(), o);
    if (!error.empty())
    {
        addWarning(toQString(error));
        return false;
    }
    m_recording = true;
    emit recordingChanged();
    return true;
}

void AcquisitionController::stopRecording()
{
    if (!m_recording) return;
    const std::string error = m_writer.close();
    if (!error.empty()) addWarning(toQString(error));
    m_recording = false;
    emit recordingChanged();
}

void AcquisitionController::setTrigger(const QVariantMap &t)
{
    acquisition::TriggerSettings s = m_trigger.settings();
    m_triggerEnabled = t.value(QStringLiteral("enabled"), m_triggerEnabled).toBool();
    s.channel = static_cast<std::size_t>(std::max(0, t.value(QStringLiteral("channel"), static_cast<int>(s.channel)).toInt()));
    s.level = t.value(QStringLiteral("level"), s.level).toDouble();
    s.hysteresis = t.value(QStringLiteral("hysteresis"), s.hysteresis).toDouble();
    const QString edge = t.value(QStringLiteral("edge")).toString();
    if (edge == QStringLiteral("rising")) s.edge = acquisition::TriggerEdge::Rising;
    else if (edge == QStringLiteral("falling")) s.edge = acquisition::TriggerEdge::Falling;
    else if (edge == QStringLiteral("either")) s.edge = acquisition::TriggerEdge::Either;
    const QString mode = t.value(QStringLiteral("mode")).toString();
    if (mode == QStringLiteral("auto")) s.mode = acquisition::TriggerMode::Auto;
    else if (mode == QStringLiteral("normal")) s.mode = acquisition::TriggerMode::Normal;
    else if (mode == QStringLiteral("single")) s.mode = acquisition::TriggerMode::Single;
    s.length = static_cast<std::size_t>(std::max(2, t.value(QStringLiteral("length"), static_cast<int>(s.length)).toInt()));
    s.preTrigger = t.value(QStringLiteral("preTrigger"), s.preTrigger).toDouble();
    s.holdoff = static_cast<std::size_t>(std::max(0, t.value(QStringLiteral("holdoff"), static_cast<int>(s.holdoff)).toInt()));
    m_trigger.configure(s);
    m_haveCapture = false;
    emit triggerChanged();
}

void AcquisitionController::armTrigger()
{
    m_trigger.arm();
    emit triggerChanged();
}

QVariantMap AcquisitionController::trigger() const
{
    const acquisition::TriggerSettings &s = m_trigger.settings();
    QVariantMap t;
    t.insert(QStringLiteral("enabled"), m_triggerEnabled);
    t.insert(QStringLiteral("channel"), static_cast<int>(s.channel));
    t.insert(QStringLiteral("level"), s.level);
    t.insert(QStringLiteral("hysteresis"), s.hysteresis);
    t.insert(QStringLiteral("edge"), s.edge == acquisition::TriggerEdge::Rising    ? QStringLiteral("rising")
                                     : s.edge == acquisition::TriggerEdge::Falling ? QStringLiteral("falling")
                                                                                   : QStringLiteral("either"));
    t.insert(QStringLiteral("mode"), s.mode == acquisition::TriggerMode::Auto     ? QStringLiteral("auto")
                                     : s.mode == acquisition::TriggerMode::Normal ? QStringLiteral("normal")
                                                                                  : QStringLiteral("single"));
    t.insert(QStringLiteral("length"), static_cast<int>(s.length));
    t.insert(QStringLiteral("preTrigger"), s.preTrigger);
    t.insert(QStringLiteral("holdoff"), static_cast<int>(s.holdoff));
    t.insert(QStringLiteral("armed"), m_trigger.armed());
    return t;
}

QVariantMap AcquisitionController::captureToMap(const acquisition::TriggerCapture &c, bool withValues) const
{
    QVariantMap m;
    m.insert(QStringLiteral("firstFrame"), QVariant::fromValue(static_cast<qint64>(c.firstFrame)));
    m.insert(QStringLiteral("length"), static_cast<int>(c.length));
    m.insert(QStringLiteral("triggerFrame"), QVariant::fromValue(static_cast<qint64>(c.triggerFrame)));
    m.insert(QStringLiteral("triggerFraction"), c.triggerFraction);
    m.insert(QStringLiteral("forced"), c.forced);
    m.insert(QStringLiteral("rising"), c.rising);
    if (withValues)
    {
        QVariantList channels;
        std::vector<double> values;
        for (std::size_t ch = 0; ch < m_recorder.channelCount(); ++ch)
        {
            m_recorder.copy(ch, c.firstFrame, c.length, values);
            QVariantList list;
            list.reserve(static_cast<qsizetype>(values.size()));
            for (double v : values) list.append(v);
            channels.append(QVariant(list));
        }
        m.insert(QStringLiteral("channels"), channels);
    }
    return m;
}

QVariantMap AcquisitionController::lastCapture() const
{
    return m_haveCapture ? captureToMap(m_lastCapture, true) : QVariantMap();
}

QString AcquisitionController::validate(const QVariantMap &settings) const
{
    AcquisitionSettings s = m_settings;
    std::string error;
    if (!acquisition::fromJson(variantToJson(settings), s, &error)) return toQString(error);
    return toQString(acquisition::validateSettings(s));
}

QVariantMap AcquisitionController::defaultSettings() const
{
    return jsonToVariant(acquisition::toJson(AcquisitionSettings{})).toMap();
}

QString AcquisitionController::firmwareExample(const QString &target) const
{
    const acquisition::DecoderSettings format = acquisition::effectiveDecoder(m_settings);
    std::size_t channels = format.channelCount();
    if (channels == 0) channels = std::max<std::size_t>(1, m_settings.channels.size());
    const double rate = m_settings.declaredSampleRateHz > 0.0 ? m_settings.declaredSampleRateHz : 100.0;
    const auto kind = target == QStringLiteral("arduino") ? acquisition::FirmwareTarget::Arduino : acquisition::FirmwareTarget::PortableC;
    return toQString(acquisition::firmwareExample(format, kind, channels, rate, m_settings.serial.baudRate));
}

QStringList AcquisitionController::audioInputs() const
{
    QStringList names;
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    for (const QAudioDevice &d : inputs) names << d.description();
    return names;
}
} // namespace qacq::app
