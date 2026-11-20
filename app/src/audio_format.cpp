#include "qacq/app/audio_format.h"

namespace qacq::app
{
QAudioFormat audioFormatFor(const acquisition::AudioSettings &settings)
{
    QAudioFormat format;
    format.setSampleRate(settings.sampleRate);
    format.setChannelCount(settings.channels);
    switch (settings.sampleType)
    {
        case acquisition::AudioSampleType::Int16: format.setSampleFormat(QAudioFormat::Int16); break;
        case acquisition::AudioSampleType::Int32: format.setSampleFormat(QAudioFormat::Int32); break;
        case acquisition::AudioSampleType::Float32: format.setSampleFormat(QAudioFormat::Float); break;
    }
    return format;
}
} // namespace qacq::app
