// Every public header of the Qt-free libraries must survive the keyword macros of Qt (signals, slots,
// emit), since the Qt layers include them after Qt headers. This file only has to compile.
#define signals public
#define slots
#define emit
#define foreach(variable, container) for (variable : container)
#include "qacq/acquisition/firmware_example.h"
#include "qacq/acquisition/rate_estimator.h"
#include "qacq/acquisition/recorder.h"
#include "qacq/acquisition/settings.h"
#include "qacq/acquisition/simulator.h"
#include "qacq/acquisition/stream_writer.h"
#include "qacq/acquisition/trigger.h"
#include "qacq/dsp/channel.h"
#include "qacq/dsp/constants.h"
#include "qacq/dsp/fft.h"
#include "qacq/dsp/filter_design.h"
#include "qacq/dsp/filtering.h"
#include "qacq/dsp/generator.h"
#include "qacq/dsp/modulation.h"
#include "qacq/dsp/random.h"
#include "qacq/dsp/receiver.h"
#include "qacq/dsp/spectrum.h"
#include "qacq/dsp/tone.h"
#include "qacq/dsp/window.h"
#include "qacq/io/binary_decoder.h"
#include "qacq/io/crc.h"
#include "qacq/io/decoder.h"
#include "qacq/io/scpi.h"
#include "qacq/io/spsc_ring.h"
#include "qacq/io/text_decoder.h"
#include "qacq/model/csv.h"
#include "qacq/model/document.h"
#include "qacq/model/json.h"
#include "qacq/model/session_file.h"
#include "qacq/model/signal.h"
#include "qacq/model/wav.h"
#undef signals
#undef slots
#undef emit
#undef foreach
