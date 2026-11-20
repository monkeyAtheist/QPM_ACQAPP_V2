#pragma once

// The sources of an acquisition, each living in its own thread (QThread), so that a slow device, a
// blocking driver or a burst of data never freezes the interface.
//
// A source worker reads its device (serial port, TCP or UDP socket, SCPI instrument, sound card,
// simulator), decodes the bytes in its own thread with the decoder of the settings, and pushes the frames
// into a lock-free SamplePipe that the AcquisitionController empties at the display rate. Counters are
// atomics; the pipe and the channel names are handed over through SourceShared under a mutex (once at
// the start, and when the stream names its channels).
//
// open() and close() run in the worker thread: the Qt devices are created and destroyed there.

#include "qacq/acquisition/recorder.h"
#include "qacq/acquisition/settings.h"
#include "qacq/io/decoder.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qacq::app
{
struct SourceShared
{
    // Written by the worker thread, read by the interface.
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> checksumErrors{0};
    std::atomic<std::uint64_t> lostFrames{0};
    std::atomic<std::uint64_t> overflows{0};
    std::atomic<std::uint64_t> skippedBytes{0};
    std::atomic<std::uint64_t> timeouts{0}; // SCPI: queries left without an answer

    std::shared_ptr<acquisition::SamplePipe> pipe() const;
    std::vector<std::string> channelNames() const;
    void setPipe(std::shared_ptr<acquisition::SamplePipe> pipe);
    void setChannelNames(std::vector<std::string> names);

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<acquisition::SamplePipe> m_pipe;
    std::vector<std::string> m_names;
};

class SourceWorker : public QObject
{
    Q_OBJECT

public:
    SourceWorker(const acquisition::AcquisitionSettings &settings, std::shared_ptr<SourceShared> shared);
    ~SourceWorker() override;

    // In the worker thread (queued or from QThread::started).
    void open();
    void close();

signals:
    void opened(const QString &description); // "COM3 à 115 200 bauds"
    void failed(const QString &message);     // the acquisition cannot go on
    void warning(const QString &message);    // it goes on
    void identified(const QString &identity); // SCPI *IDN? answer

protected:
    virtual bool openDevice(QString &description, QString &error) = 0;
    virtual void closeDevice() = 0;
    // Decodes received bytes and pushes the frames.
    void consume(const char *data, qsizetype size);
    void consume(const QByteArray &bytes) { consume(bytes.constData(), bytes.size()); }
    io::Decoder *decoder() const { return m_decoder.get(); }
    SourceShared &shared() const { return *m_shared; }

    acquisition::AcquisitionSettings m_settings;

private:
    void publishCounters();

    std::shared_ptr<SourceShared> m_shared;
    std::unique_ptr<io::Decoder> m_decoder;
    std::shared_ptr<acquisition::SamplePipe> m_pipe;
    io::SampleBlock m_block;
    std::vector<std::string> m_names;
    bool m_open = false;
};

// The worker for the source of the settings, or null with *error set (settings not valid, VISA not
// available yet...).
std::unique_ptr<SourceWorker> makeSourceWorker(const acquisition::AcquisitionSettings &settings,
                                               std::shared_ptr<SourceShared> shared, QString *error = nullptr);

// Frames the pipe can hold before the interface must have emptied it (about a second of a fast stream).
std::size_t pipeCapacityFrames(std::size_t channels);
} // namespace qacq::app
