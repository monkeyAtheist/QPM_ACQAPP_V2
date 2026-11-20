#pragma once

// Qt Multimedia format of the sound card settings (sample rate, channels, sample type).

#include "qacq/acquisition/settings.h"

#include <QAudioFormat>

namespace qacq::app
{
QAudioFormat audioFormatFor(const acquisition::AudioSettings &settings);
} // namespace qacq::app
