#include "remap_audio.h"
#include "native_media.h"
#include "video_frames.h"
#include <QBuffer>
#include <algorithm>
#include <cmath>
#include <cstring>

RemapSound::RemapSound(QObject *parent) : QIODevice(parent) {}

void RemapSound::load(const QString &path) {
  source = path;
  pcm.reset();
  decoding = std::async(std::launch::async, [path] {
    auto out = std::make_shared<std::vector<int16_t>>();
    try {
      NativeMedia media;
      QBuffer buffer;
      buffer.open(QIODevice::ReadWrite);
      if (media.decodeAudio(path, buffer, [] { return false; })) {
        const QByteArray &bytes = buffer.data();
        out->resize(size_t(bytes.size() / 2));
        std::memcpy(out->data(), bytes.constData(), out->size() * 2);
      }
    } catch (...) {
      out->clear(); // no sound, or none that can be read: the layer plays silent
    }
    return std::shared_ptr<const std::vector<int16_t>>(out);
  });
}

void RemapSound::setPcm(std::vector<int16_t> samples) {
  decoding = {};
  pcm = std::make_shared<const std::vector<int16_t>>(std::move(samples));
}

bool RemapSound::ready() {
  if (!pcm && decoding.valid() && decoding.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    pcm = decoding.get();
  return pcm && pcm->size() >= 4;
}

void RemapSound::begin(const Layer &playing, int framesPerSecond, int sceneFrames, double compFrame) {
  layer = playing;
  fps = std::max(1, framesPerSecond);
  frames = std::max(0, sceneFrames);
  nextSample = qint64(std::llround(std::max(0., compFrame) * 48000. / fps));
}

qint64 RemapSound::readData(char *data, qint64 maxSize) {
  const qint64 count = maxSize / 4;
  if (count <= 0) return 0;
  std::vector<qint64> sum(size_t(count) * 2, 0);
  if (ready()) {
    const qint64 pcmFrames = qint64(pcm->size() / 2);
    // Round the composition as playback goes round it.
    const qint64 loop = frames > 0 ? qint64(std::llround(double(frames) * 48000. / fps)) : 0;
    qint64 done = 0;
    while (done < count) {
      const qint64 at = loop > 0 ? (nextSample + done) % loop : nextSample + done;
      const qint64 piece = loop > 0 ? std::min(count - done, loop - at) : count - done;
      addRemappedSound(pcm->data(), pcmFrames, layer, fps, at, piece, sum.data() + size_t(done) * 2);
      done += piece;
    }
  }
  auto *out = reinterpret_cast<int16_t *>(data);
  for (size_t i = 0; i < sum.size(); ++i) out[i] = int16_t(std::clamp<qint64>(sum[i], -32768, 32767));
  nextSample += count;
  return count * 4;
}
