#pragma once
// How loud a sound is over time, for drawing it on the timeline. A file is
// decoded once, in the background, into a peak and an RMS level for every
// five milliseconds -- a few hundred kilobytes for a whole song -- and every
// layer that plays that file draws from the same copy.
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

struct AudioPeaks {
  static constexpr int perSecond = 200;       // fine levels: one per 5 ms
  static constexpr int coarsePerSecond = 10;  // for a timeline zoomed far out
  std::vector<quint8> peak, rms;              // 0..255 = 0..full scale
  std::vector<quint8> coarsePeak, coarseRms;
  // Mean square per level, whole band and below about 150 Hz: what beat
  // detection reads, at a precision the drawing levels do not have.
  std::vector<float> energy, bass;
  double seconds() const { return double(peak.size()) / perSecond; }
  // The loudest peak and RMS between two times in the file, each 0..1.
  std::pair<double, double> range(double from, double to) const;
  // Fills in the coarse levels from the fine ones.
  void summarise();
};

// Reduces interleaved 16-bit stereo PCM at 48 kHz, as it is written, to peaks.
class AudioPeakSink : public QIODevice {
public:
  AudioPeakSink();
  AudioPeaks finish();
protected:
  qint64 readData(char *, qint64) override { return -1; }
  qint64 writeData(const char *data, qint64 size) override;
private:
  AudioPeaks peaks;
  QByteArray carry;
  double bucketPeak = 0, bucketSquares = 0, bucketBass = 0;
  double low1 = 0, low2 = 0; // two one-pole low-pass stages, for the bass
  int bucketFrames = 0;
  void flush();
};

// The levels of a file, or null while they are being worked out (and for a
// file with no sound in it). `repaint` is updated on the interface thread
// once they are ready; asking again while they are being made adds nothing.
class QWidget;
std::shared_ptr<const AudioPeaks> audioPeaks(const QString &path, QWidget *repaint = nullptr);
// Whether a file is still being read.
bool audioPeaksPending(const QString &path);
// Stops decodes that are still running; for shutting down.
void cancelAudioPeaks();
int checkWaveforms(QStringList &report);

// The beat of a piece of music, from its levels: the tempo, and a time in
// seconds for every beat, following the music where it drifts. Empty for
// anything too short or with no steady pulse.
struct BeatAnalysis {
  double bpm = 0;
  std::vector<double> beats;
  std::vector<double> strengths; // how sharp the sound is on each beat, in spreads of its onset
  // The tempos considered, best fit first: the beat and those related to it
  // (twice, half, one and a half...), with how well each fits, 0..1.
  std::vector<std::pair<double, double>> tempos;
};
// With a tempo given, only tempos within a few percent of it are considered:
// for choosing between a beat and twice or half of it, which music does not
// always settle.
BeatAnalysis detectBeats(const AudioPeaks &peaks, double tempoHint = 0);
