#include "native_media.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <cmath>
#include "ffmpeg_export.h"
#include "image_files.h"
#include "render_workers.h"

// ---------------------------------------------------------------------------
// Footage Windows has no decoder for -- ProRes; HEVC without its extension --
// read through FFmpeg, the program export already uses: started at the frame
// asked for, it writes every picture after it down a pipe as raw BGRA, and is
// read on from there, started again only when the time jumps.
// ---------------------------------------------------------------------------
namespace {
QString footageFFmpeg() {
  QString found = findExportFFmpeg();
  if (found.isEmpty()) {
    // A build run from its build folder, with the tools beside the project.
    const QString beside = QCoreApplication::applicationDirPath() + "/../tools/ffmpeg/bin/ffmpeg.exe";
    if (QFileInfo(beside).isFile()) found = QFileInfo(beside).absoluteFilePath();
  }
  return found;
}
}  // namespace
class FFmpegFootage {
public:
  ~FFmpegFootage() { stop(); }
  // Its size and rate, read from what FFmpeg says of it. False when FFmpeg is
  // missing or finds no video in it.
  bool open(const QString &file) {
    if (file == path && width > 0) return true;
    stop();
    path = file; width = height = 0; fps = 0;
    program = footageFFmpeg();
    if (program.isEmpty()) return false;
    QProcess probe;
    probe.start(program, {"-hide_banner", "-nostdin", "-i", file});
    if (!probe.waitForFinished(15000)) { probe.kill(); probe.waitForFinished(1000); return false; }
    const QString said = QString::fromUtf8(probe.readAllStandardError());
    for (const QString &line : said.split('\n')) {
      if (!line.contains("Video:")) continue;
      const auto size = QRegularExpression("(\\d{2,5})x(\\d{2,5})").match(line.mid(line.indexOf("Video:")));
      const auto rate = QRegularExpression("([0-9.]+) (fps|tbr)").match(line);
      if (!size.hasMatch()) continue;
      width = size.captured(1).toInt(); height = size.captured(2).toInt();
      fps = rate.hasMatch() ? rate.captured(1).toDouble() : 30.;
      break;
    }
    if (width <= 0 || height <= 0 || quint64(width) * height > 100000000 || !(fps > 0)) { width = height = 0; return false; }
    return true;
  }
  // The picture on screen at `time` (100 ns), and where it starts and how long
  // it lasts.
  QSize size() const { return QSize(width, height); }
  // Pictures made at 1/shrink of the footage's size; started again to change.
  void setShrink(int divisor) {
    divisor = std::clamp(divisor, 1, 4);
    if (divisor == shrink) return;
    shrink = divisor;
    stop();
    last = QImage();
    lastIndex = -1;
  }
  QImage frame(qint64 time, qint64 *start, qint64 *length) {
    const double frameLength = 1e7 / fps;
    const qint64 wanted = std::max<qint64>(0, qint64(std::floor(time / frameLength + 1e-6)));
    if (!last.isNull() && wanted == lastIndex) return give(start, length);
    // A little ahead of where the pipe stands is read on to; anything else
    // starts it again where it is wanted.
    if (!running() || wanted < next || wanted - next > qint64(std::ceil(fps * 2))) restart(wanted);
    while (next <= wanted) {
      QImage picture = readPicture();
      if (picture.isNull()) {
        // The end of the footage: its last picture stays.
        if (last.isNull()) throw std::runtime_error("FFmpeg gave no picture of the footage");
        return give(start, length);
      }
      last = std::move(picture);
      lastIndex = next++;
    }
    return give(start, length);
  }
  // Every picture from `from` on, in order, until `picture` says stop.
  bool readOn(qint64 from, const std::function<bool(qint64, const QImage &)> &picture) {
    const double frameLength = 1e7 / fps;
    restart(std::max<qint64>(0, qint64(std::floor(from / frameLength + 1e-6))));
    for (;;) {
      QImage image = readPicture();
      if (image.isNull()) return true;
      const qint64 at = qint64(std::llround(next * frameLength));
      ++next;
      if (!picture(at, image)) return true;
    }
  }
private:
  QImage give(qint64 *start, qint64 *length) const {
    const double frameLength = 1e7 / fps;
    if (start) *start = qint64(std::llround(lastIndex * frameLength));
    if (length) *length = qint64(std::llround((lastIndex + 1) * frameLength)) - qint64(std::llround(lastIndex * frameLength));
    return last;
  }
  bool running() const { return process && process->state() != QProcess::NotRunning; }
  void stop() {
    if (!process) return;
    process->kill();
    process->waitForFinished(2000);
    process.reset();
  }
  void restart(qint64 index) {
    stop();
    process = std::make_unique<QProcess>();
    // A quarter of a picture before the one wanted: FFmpeg drops every picture
    // that starts before the time it is given, the one wanted included when
    // rounding puts its start a hair early.
    const double seconds = std::max(0., (index - .25) / fps);
    QStringList arguments{"-v", "error", "-nostdin", "-ss", QString::number(seconds, 'f', 6), "-i", path,
                          "-map", "0:v:0", "-an", "-sn", "-fps_mode", "passthrough"};
    if (shrink > 1) arguments << "-vf" << QString("scale=%1:%2:flags=bilinear").arg(outWidth()).arg(outHeight());
    arguments << "-f" << "rawvideo" << "-pix_fmt" << "bgra" << "pipe:1";
    process->start(program, arguments);
    if (!process->waitForStarted(10000)) throw std::runtime_error("FFmpeg did not start");
    next = index;
  }
  int outWidth() const { return std::max(2, (width / shrink) & ~1); }
  int outHeight() const { return std::max(2, (height / shrink) & ~1); }
  QImage readPicture() {
    const int width = outWidth(), height = outHeight();
    const qint64 bytes = qint64(width) * height * 4;
    while (process->bytesAvailable() < bytes) {
      if (process->state() == QProcess::NotRunning) {
        if (process->bytesAvailable() < bytes) return {};
        break;
      }
      process->waitForReadyRead(10000);
      if (process->state() == QProcess::NotRunning && process->bytesAvailable() < bytes) return {};
    }
    QImage picture(width, height, QImage::Format_ARGB32);
    if (picture.isNull()) throw std::runtime_error("Not enough memory for video frame");
    const qsizetype line = qsizetype(width) * 4;
    for (int y = 0; y < height; ++y)
      if (process->read(reinterpret_cast<char *>(picture.scanLine(y)), line) != line) return {};
    return picture;
  }
  QString path, program;
  int width = 0, height = 0, shrink = 1;
  double fps = 0;
  std::unique_ptr<QProcess> process;
  qint64 next = 0, lastIndex = -1;
  QImage last;
};
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <d3d11_4.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char *operation) {
  if(FAILED(hr)) throw std::runtime_error(QString("%1 (Windows error 0x%2)")
      .arg(operation).arg(quint32(hr),8,16,QChar('0')).toStdString());
}
ComPtr<IMFMediaType> mediaType(const GUID &major, const GUID &subtype) {
  ComPtr<IMFMediaType> type; check(MFCreateMediaType(&type),"Create media type");
  check(type->SetGUID(MF_MT_MAJOR_TYPE,major),"Set major type");
  check(type->SetGUID(MF_MT_SUBTYPE,subtype),"Set subtype"); return type;
}
ComPtr<IMFSample> sample(const QByteArray &bytes, LONGLONG time, LONGLONG duration) {
  ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(bytes.size()),&buffer),"Allocate media buffer");
  BYTE *data=nullptr; check(buffer->Lock(&data,nullptr,nullptr),"Lock media buffer");
  memcpy(data,bytes.constData(),size_t(bytes.size())); buffer->Unlock();
  check(buffer->SetCurrentLength(DWORD(bytes.size())),"Set buffer length");
  ComPtr<IMFSample> result; check(MFCreateSample(&result),"Create sample");
  check(result->AddBuffer(buffer.Get()),"Add media buffer");
  check(result->SetSampleTime(time),"Set sample time");
  check(result->SetSampleDuration(duration),"Set sample duration"); return result;
}
// A picture as Rec. 709 video: 4:2:0, luma 16-235, chroma 16-240, the way
// every player reads a movie tagged Rec. 709. Worked out here rather than left
// to Windows, whose converter used the Rec. 601 matrix for some sizes and
// encoders and the Rec. 709 one for others. Each 2x2 block shares one chroma
// sample, from the mean of its four pixels.
QByteArray rec709Nv12(const QImage &source) {
  const QImage image = source.convertToFormat(QImage::Format_RGB32);
  const int w = image.width() & ~1, h = image.height() & ~1;
  QByteArray bytes(qsizetype(w) * h * 3 / 2, Qt::Uninitialized);
  auto *luma = reinterpret_cast<uchar *>(bytes.data());
  uchar *chroma = luma + qsizetype(w) * h;
  // Kr .2126, Kb .0722, scaled for 8-bit video range and by 2^16.
  constexpr int yr = 11966, yg = 40254, yb = 4064;
  constexpr int ur = -6596, ug = -22189, ub = 28784;
  constexpr int vr = 28784, vg = -26145, vb = -2639;
  renderParallel(h / 2, [&](int from, int to) {
    for (int pair = from; pair < to; ++pair) {
      const auto *top = reinterpret_cast<const QRgb *>(image.constScanLine(pair * 2));
      const auto *bottom = reinterpret_cast<const QRgb *>(image.constScanLine(pair * 2 + 1));
      uchar *yTop = luma + qsizetype(pair * 2) * w, *yBottom = yTop + w;
      uchar *uv = chroma + qsizetype(pair) * w;
      for (int x = 0; x < w; x += 2) {
        int r = 0, g = 0, b = 0;
        for (const QRgb p : {top[x], top[x + 1], bottom[x], bottom[x + 1]}) { r += qRed(p); g += qGreen(p); b += qBlue(p); }
        auto y = [&](QRgb p) { return uchar(16 + ((yr * qRed(p) + yg * qGreen(p) + yb * qBlue(p) + 32768) >> 16)); };
        yTop[x] = y(top[x]); yTop[x + 1] = y(top[x + 1]);
        yBottom[x] = y(bottom[x]); yBottom[x + 1] = y(bottom[x + 1]);
        // Offset first, so the shift never meets a negative number.
        uv[x] = uchar(std::clamp((128 << 18) + (1 << 17) + ur * r + ug * g + ub * b, 16 << 18, 240 << 18) >> 18);
        uv[x + 1] = uchar(std::clamp((128 << 18) + (1 << 17) + vr * r + vg * g + vb * b, 16 << 18, 240 << 18) >> 18);
      }
    }
  });
  return bytes;
}
// The graphics card's video decoder, for a reader to decode on. Software
// decoding took 17 ms for a 1080p picture and 73 for a 4K one -- more than a
// frame's time, so footage beyond the first layer could not play in real time.
struct VideoCard {
  ComPtr<ID3D11Device> device;
  ComPtr<IMFDXGIDeviceManager> manager;
};
// WAVY_VIDEO_DECODE=software keeps every reader on the processor.
bool softwareDecodeWanted() { return qEnvironmentVariable("WAVY_VIDEO_DECODE") == "software"; }
std::unique_ptr<VideoCard> makeVideoCard() {
  auto card=std::make_unique<VideoCard>();
  const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
  if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,
                              UINT(sizeof(levels)/sizeof(levels[0])),D3D11_SDK_VERSION,&card->device,nullptr,nullptr)))
    return nullptr;
  // The decoder and the converter after it use the device from threads of
  // their own.
  ComPtr<ID3D11Multithread> threads;
  if(SUCCEEDED(card->device.As(&threads))) threads->SetMultithreadProtected(TRUE);
  UINT token=0;
  if(FAILED(MFCreateDXGIDeviceManager(&token,&card->manager)) || FAILED(card->manager->ResetDevice(card->device.Get(),token)))
    return nullptr;
  return card;
}
// The footage's size as shown: the decoded format's display aperture, or the
// stream's own, or else the whole decoded frame.
QSize displaySize(IMFSourceReader *source) {
  ComPtr<IMFMediaType> type;
  if(FAILED(source->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type))) return {};
  UINT32 w=0,h=0;
  if(FAILED(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h)) || !w || !h) return {};
  auto aperture=[&](IMFMediaType *from, QSize &size) {
    MFVideoArea area{}; UINT32 bytes=0;
    if(!from || FAILED(from->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,reinterpret_cast<UINT8 *>(&area),sizeof(area),&bytes)) ||
       bytes!=sizeof(area) || area.Area.cx<=0 || area.Area.cy<=0 || area.Area.cx>LONG(w) || area.Area.cy>LONG(h)) return false;
    size=QSize(int(area.Area.cx),int(area.Area.cy));
    return true;
  };
  QSize size{int(w),int(h)};
  if(!aperture(type.Get(),size)) {
    ComPtr<IMFMediaType> native;
    if(SUCCEEDED(source->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&native))) aperture(native.Get(),size);
  }
  return size;
}
// When a reader's first picture says it starts. An MP4 with B-frames carries
// its pictures' times shifted by the reordering delay and an edit list that
// takes the shift away; Windows does not apply the list, so its first picture
// of such a file says 0.133 s where FFmpeg -- the player, every other program
// -- says 0, and every picture came four frames late against the sound. The
// footage's own start is where its first picture is.
qint64 firstPictureTime(IMFSourceReader *source) {
  ComPtr<IMFSample> frame; DWORD flags=0; LONGLONG timestamp=0;
  for(int attempt=0;attempt<8 && !frame;++attempt) {
    if(FAILED(source->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&timestamp,&frame)) ||
       (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
  }
  PROPVARIANT position; PropVariantInit(&position); position.vt=VT_I8; position.hVal.QuadPart=0;
  source->SetCurrentPosition(GUID_NULL,position);
  return frame && timestamp>0 && timestamp<20000000 ? qint64(timestamp) : 0;
}
ComPtr<IMFSourceReader> reader(const QString &path, bool video, IMFDXGIDeviceManager *card=nullptr) {
  ComPtr<IMFAttributes> attrs; check(MFCreateAttributes(&attrs,4),"Reader attributes");
  if(video && card) {
    // Decoded and turned into RGB on the card; the picture is read back once.
    check(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,card),"Use the graphics card");
    check(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE),"Enable video conversion on the card");
    check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"Enable hardware decoders");
  } else if(video) check(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE),"Enable video conversion");
  ComPtr<IMFSourceReader> result;
  check(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(path.utf16()),attrs.Get(),&result),"Open source media");
  check(result->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Deselect source streams");
  return result;
}
void audioFormat(IMFMediaType *type, bool aac) {
  check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2),"Audio channels");
  check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000),"Audio sample rate");
  check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),"Audio bit depth");
  check(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,aac ? 24000 : 192000),"Audio rate");
  if(!aac) check(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4),"Audio alignment");
}
}
// A decoded RGB32 sample as an image.
static QImage decodedPicture(IMFSourceReader *video, IMFSample *frame) {
    ComPtr<IMFMediaType> type;
    check(video->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type),"Read decoded video format");
    UINT32 w=0,h=0; check(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h),"Read video dimensions");
    if(!w || !h || quint64(w)*h>100000000) throw std::runtime_error("Unsupported video dimensions");
    // A movie is coded in whole 16-pixel blocks, so one 1080 or 360 high comes
    // out of the decoder 1088 or 368 high: the rows below the picture are
    // padding. What is to be shown is the display aperture, said by the
    // decoded format or else by the stream's own.
    int cropX=0, cropY=0, cw=int(w), ch=int(h);
    auto aperture=[&](IMFMediaType *from) {
      MFVideoArea area{}; UINT32 bytes=0;
      if(!from || FAILED(from->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,reinterpret_cast<UINT8 *>(&area),sizeof(area),&bytes)) ||
         bytes!=sizeof(area)) return false;
      const int x=area.OffsetX.value, y=area.OffsetY.value, aw=int(area.Area.cx), ah=int(area.Area.cy);
      if(aw<=0 || ah<=0 || x<0 || y<0 || x+aw>int(w) || y+ah>int(h)) return false;
      cropX=x; cropY=y; cw=aw; ch=ah; return true;
    };
    if(!aperture(type.Get())) {
      ComPtr<IMFMediaType> native;
      if(SUCCEEDED(video->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&native))) aperture(native.Get());
    }
    ComPtr<IMFMediaBuffer> buffer; check(frame->ConvertToContiguousBuffer(&buffer),"Read video pixels");
    QImage image(cw,ch,QImage::Format_RGB32);
    if(image.isNull()) throw std::runtime_error("Not enough memory for video frame");
    // Rows copied across the cores, the fourth byte -- RGB32's padding, not
    // decoded alpha -- made opaque on the way.
    uchar *bits=image.bits();
    const qsizetype line=image.bytesPerLine();
    auto copy=[&](const BYTE *top, qint64 stride) {
      renderParallel(ch,[&](int from,int to) {
        for(int y=from;y<to;++y) {
          const auto *in=reinterpret_cast<const quint32 *>(top+qint64(y+cropY)*stride+qint64(cropX)*4);
          auto *out=reinterpret_cast<quint32 *>(bits+qsizetype(y)*line);
          for(int x=0;x<cw;++x) out[x]=in[x]|0xff000000u;
        }
      });
    };
    ComPtr<IMF2DBuffer> twoD;
    if(SUCCEEDED(buffer.As(&twoD))) {
      BYTE *data=nullptr; LONG pitch=0; check(twoD->Lock2D(&data,&pitch),"Lock video surface");
      copy(data,pitch);
      twoD->Unlock2D();
    } else {
      BYTE *data=nullptr; DWORD length=0; check(buffer->Lock(&data,nullptr,&length),"Lock video pixels");
      UINT32 rawStride=UINT32(-LONG(w*4)); type->GetUINT32(MF_MT_DEFAULT_STRIDE,&rawStride); LONG stride=LONG(rawStride);
      if(std::abs(qint64(stride))<qint64(w)*4 || quint64(std::abs(qint64(stride)))*h>length) { buffer->Unlock(); throw std::runtime_error("Invalid decoded video stride"); }
      BYTE *top=stride<0 ? data+(h-1)*std::abs(qint64(stride)) : data;
      copy(top,stride);
      buffer->Unlock();
    }
    return image;
}
// The decoder's pictures turned into BGRA and made the size wanted on the card
// itself, then read back through a staging texture of our own. Windows'
// converter, read back through its own lock, cost 5-9 ms a picture where the
// decoding took 1-2, and scaling 4K down through it took three times as long
// as not scaling: here a picture made a quarter the size moves a sixteenth of
// the bytes.
class CardConverter {
public:
  explicit CardConverter(ID3D11Device *d) : device(d) {
    device->GetImmediateContext(&context);
    if(FAILED(device.As(&video)) || FAILED(context.As(&videoContext))) throw std::runtime_error("No video processor on this card");
  }
  // One decoded picture (a slice of the decoder's texture array), the part of
  // it that is shown, at `out`, with its colours read as `matrix709` and
  // `fullRange` say.
  QImage convert(ID3D11Texture2D *texture, UINT slice, const QRect &shown, QSize out, bool matrix709, bool fullRange) {
    D3D11_TEXTURE2D_DESC in{}; texture->GetDesc(&in);
    prepare(in.Width,in.Height,UINT(out.width()),UINT(out.height()));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC viewDesc{};
    viewDesc.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.ArraySlice=slice;
    ComPtr<ID3D11VideoProcessorInputView> view;
    check(video->CreateVideoProcessorInputView(texture,enumerator.Get(),&viewDesc,&view),"Read the decoded picture on the card");
    const RECT source{shown.left(),shown.top(),shown.left()+shown.width(),shown.top()+shown.height()};
    const RECT target{0,0,LONG(out.width()),LONG(out.height())};
    videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext->VideoProcessorSetStreamSourceRect(processor.Get(),0,TRUE,&source);
    videoContext->VideoProcessorSetStreamDestRect(processor.Get(),0,TRUE,&target);
    videoContext->VideoProcessorSetOutputTargetRect(processor.Get(),TRUE,&target);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
    inSpace.YCbCr_Matrix=matrix709 ? 1 : 0;
    inSpace.Nominal_Range=fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext->VideoProcessorSetStreamColorSpace(processor.Get(),0,&inSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{}; // full-range RGB
    videoContext->VideoProcessorSetOutputColorSpace(processor.Get(),&outSpace);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable=TRUE;
    stream.pInputSurface=view.Get();
    check(videoContext->VideoProcessorBlt(processor.Get(),outputView.Get(),0,1,&stream),"Convert the picture on the card");
    context->CopyResource(staging.Get(),target_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read the picture back");
    QImage image(out,QImage::Format_RGB32);
    if(image.isNull()) { context->Unmap(staging.Get(),0); throw std::runtime_error("Not enough memory for video frame"); }
    uchar *bits=image.bits();
    const qsizetype line=image.bytesPerLine();
    const auto *from=static_cast<const uchar *>(mapped.pData);
    const qsizetype pitch=mapped.RowPitch;
    const int width=out.width();
    renderParallel(out.height(),[&](int first,int last) {
      for(int y=first;y<last;++y) {
        const auto *in=reinterpret_cast<const quint32 *>(from+qsizetype(y)*pitch);
        auto *row=reinterpret_cast<quint32 *>(bits+qsizetype(y)*line);
        for(int x=0;x<width;++x) row[x]=in[x]|0xff000000u;
      }
    });
    context->Unmap(staging.Get(),0);
    return image;
  }
private:
  void prepare(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight) {
    if(processor && inWidth==inW && inHeight==inH && outWidth==outW && outHeight==outH) return;
    processor.Reset(); enumerator.Reset(); outputView.Reset(); target_.Reset(); staging.Reset();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth=inWidth; desc.InputHeight=inHeight;
    desc.OutputWidth=outWidth; desc.OutputHeight=outHeight;
    desc.Usage=D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    check(video->CreateVideoProcessorEnumerator(&desc,&enumerator),"Set up the card's video converter");
    check(video->CreateVideoProcessor(enumerator.Get(),0,&processor),"Make the card's video converter");
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width=outWidth; texture.Height=outHeight; texture.MipLevels=1; texture.ArraySize=1;
    texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count=1;
    texture.Usage=D3D11_USAGE_DEFAULT; texture.BindFlags=D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&texture,nullptr,&target_),"Make the converted picture");
    texture.Usage=D3D11_USAGE_STAGING; texture.BindFlags=0; texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&texture,nullptr,&staging),"Make the picture to read back");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDesc{};
    viewDesc.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
    check(video->CreateVideoProcessorOutputView(target_.Get(),enumerator.Get(),&viewDesc,&outputView),"Point the converter at its picture");
    inW=inWidth; inH=inHeight; outW=outWidth; outH=outHeight;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11VideoContext> videoContext;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  ComPtr<ID3D11Texture2D> target_, staging;
  ComPtr<ID3D11VideoProcessorOutputView> outputView;
  UINT inW=0, inH=0, outW=0, outH=0;
};
// The decoder's picture, where it shows, and how its colours are coded.
struct CodedPicture { QRect shown; bool matrix709=true, fullRange=false; };
CodedPicture codedPicture(IMFSourceReader *source) {
  CodedPicture coded;
  ComPtr<IMFMediaType> type, native;
  source->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type);
  source->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&native);
  UINT32 w=0,h=0;
  if(type) MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h);
  coded.shown=QRect(0,0,int(w),int(h));
  for(IMFMediaType *from:{type.Get(),native.Get()}) {
    MFVideoArea area{}; UINT32 bytes=0;
    if(from && SUCCEEDED(from->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,reinterpret_cast<UINT8 *>(&area),sizeof(area),&bytes)) &&
       bytes==sizeof(area) && area.Area.cx>0 && area.Area.cy>0 && area.OffsetX.value+area.Area.cx<=LONG(w) &&
       area.OffsetY.value+area.Area.cy<=LONG(h)) {
      coded.shown=QRect(area.OffsetX.value,area.OffsetY.value,int(area.Area.cx),int(area.Area.cy));
      break;
    }
  }
  // As Windows' own converter reads them: the stream's matrix and range when
  // it says, else Rec. 709 for a picture 720 high or more and Rec. 601 below.
  UINT32 matrix=0, range=0;
  if(!native || FAILED(native->GetUINT32(MF_MT_YUV_MATRIX,&matrix))) matrix=0;
  coded.matrix709=matrix==MFVideoTransferMatrix_BT709 || (matrix!=MFVideoTransferMatrix_BT601 && coded.shown.height()>=720);
  coded.fullRange=native && SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,&range)) && range==MFNominalRange_0_255;
  return coded;
}
struct NativeMedia::State {
  bool com=false, mf=false;
  ComPtr<IMFSinkWriter> writer;
  ComPtr<IMFSourceReader> video;
  QString videoPath;
  // The picture the reader last decoded, and where the reader stands: a request
  // for it again, or for one a little later, needs no seek.
  QImage lastImage, earlierImage; // and the one before it: a blend reads one ahead, then comes back
  qint64 lastStart=-1, lastEnd=-1, earlierStart=-1, earlierEnd=-1;
  bool ended=false;
  DWORD videoStream=0, audioStream=0;
  bool withAudio=false;
  // The card the video reader decodes on, made the first time one is opened;
  // none when there is no card that decodes, or it has failed once.
  std::unique_ptr<VideoCard> card;
  bool cardTried=false, decodingOnCard=false;
  // The video reader hands over the decoder's own NV12 pictures, which the
  // converter makes into RGB on the card (CardConverter); false where Windows'
  // converter does it, which is also where it goes once ours has failed.
  // WAVY_VIDEO_CONVERTER=windows keeps Windows' converter.
  bool nv12=false, converterBroken=qEnvironmentVariable("WAVY_VIDEO_CONVERTER") == "windows";
  std::unique_ptr<CardConverter> converter;
  // The footage of videoPath read through FFmpeg, when Windows cannot.
  std::unique_ptr<FFmpegFootage> ffmpeg;
  // An animated picture (a GIF): its frames by their own times.
  std::unique_ptr<AnimatedImageFootage> animated;
  // Pictures at 1/shrink size (setShrink): made so by our converter or by
  // FFmpeg; Windows' converter makes them whole.
  int shrink=1;
  QSize footage;
  // Where the video reader's first picture stands (firstPictureTime): taken
  // off every time it gives, added to every time it is asked.
  qint64 timeOffset=0;
  // How long the footage runs on the reader's clock; 0 when it does not say.
  qint64 duration=0;
  State() {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(SUCCEEDED(hr)) com=true; else if(hr!=RPC_E_CHANGED_MODE) check(hr,"Initialize COM");
    hr=MFStartup(MF_VERSION);
    if(FAILED(hr)) { if(com) CoUninitialize(); check(hr,"Start Media Foundation"); }
    mf=true;
  }
  ~State() { writer.Reset(); video.Reset(); converter.reset(); card.reset(); if(mf) MFShutdown(); if(com) CoUninitialize(); }
  // A reader of the footage's pictures, on the card when there is one: the
  // decoder's NV12 for our converter, or Windows' RGB32.
  ComPtr<IMFSourceReader> openVideo(const QString &path, bool &onCard, bool &decoderPictures) {
    if(!cardTried) { cardTried=true; if(!softwareDecodeWanted()) card=makeVideoCard(); }
    auto open=[&](IMFDXGIDeviceManager *manager, const GUID &format) {
      auto source=reader(path,true,manager);
      auto type=mediaType(MFMediaType_Video,format);
      check(source->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.Get()),"Decode RGB32 video");
      check(source->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),"Select video stream");
      return source;
    };
    decoderPictures=false;
    if(card) {
      onCard=true;
      if(!converterBroken) {
        try {
          if(!converter) converter=std::make_unique<CardConverter>(card->device.Get());
          auto source=open(card->manager.Get(),MFVideoFormat_NV12);
          decoderPictures=true;
          return source;
        } catch(const std::exception &) {}
      }
      try { return open(card->manager.Get(),MFVideoFormat_RGB32); } catch(const std::exception &) {}
    }
    onCard=false;
    return open(nullptr,MFVideoFormat_RGB32);
  }
  ComPtr<IMFSourceReader> openVideo(const QString &path, bool &onCard) {
    bool decoderPictures=false;
    return openVideo(path,onCard,decoderPictures);
  }
  // A sample as a picture: through our converter when it is the decoder's
  // own, at 1/shrink of the footage's size; Windows' RGB32 as it comes.
  QImage picture(IMFSourceReader *source, IMFSample *sample, bool decoderPictures) {
    if(!decoderPictures) return decodedPicture(source,sample);
    ComPtr<IMFMediaBuffer> buffer; check(sample->GetBufferByIndex(0,&buffer),"Read video pixels");
    ComPtr<IMFDXGIBuffer> onCard; check(buffer.As(&onCard),"Read the decoded picture on the card");
    ComPtr<ID3D11Texture2D> texture; UINT slice=0;
    check(onCard->GetResource(IID_PPV_ARGS(&texture)),"Read the decoded picture on the card");
    check(onCard->GetSubresourceIndex(&slice),"Read the decoded picture on the card");
    const CodedPicture coded=codedPicture(source);
    const QSize out(std::max(2,(coded.shown.width()/shrink)&~1),std::max(2,(coded.shown.height()/shrink)&~1));
    return converter->convert(texture.Get(),slice,coded.shown,shrink>1 ? out : coded.shown.size(),coded.matrix709,coded.fullRange);
  }
};
NativeMedia::NativeMedia():state(std::make_unique<State>()) {}
NativeMedia::~NativeMedia()=default;
QImage NativeMedia::videoFrame(const QString &path, qint64 time) { return videoFrame(path, time, nullptr, nullptr); }
QImage NativeMedia::videoFrame(const QString &path, qint64 time, qint64 *start, qint64 *length) {
  try {
    return videoFrameAt(path, time, start, length);
  } catch(const std::exception &) {
    // Our converter failing: Windows' on the same card.
    if(state->nv12 && !state->converterBroken) {
      state->converterBroken=true;
      state->videoPath.clear();
      try { return videoFrameAt(path, time, start, length); } catch(const std::exception &) {}
    }
    // A card that cannot decode this footage after all: the processor can.
    if(!state->decodingOnCard) throw;
    state->card.reset();
    state->videoPath.clear();
    return videoFrameAt(path, time, start, length);
  }
}
QImage NativeMedia::videoFrameAt(const QString &path, qint64 time, qint64 *start, qint64 *length) {
  time=std::max<qint64>(0,time);
  if(state->videoPath!=path) {
    state->video.Reset();
    state->ffmpeg.reset();
    state->animated.reset();
    // An animated picture is read by Qt, frame by frame at its own times:
    // Windows' readers take a GIF for a still.
    if(isAnimatedImageName(path)) {
      auto footage=std::make_unique<AnimatedImageFootage>();
      if(!footage->open(path)) throw std::runtime_error("The animated picture cannot be read");
      footage->setShrink(state->shrink);
      state->footage=footage->size();
      state->animated=std::move(footage);
      state->decodingOnCard=false;
      state->videoPath=path;
      state->lastImage=state->earlierImage=QImage(); state->lastStart=state->lastEnd=state->earlierStart=state->earlierEnd=-1; state->ended=false;
      return state->animated->frame(time,start,length);
    }
    try {
      state->video=state->openVideo(path,state->decodingOnCard,state->nv12);
      state->footage=displaySize(state->video.Get());
      state->timeOffset=firstPictureTime(state->video.Get());
      PROPVARIANT length; PropVariantInit(&length);
      state->duration=SUCCEEDED(state->video->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&length)) &&
                      length.vt==VT_UI8 ? qint64(length.uhVal.QuadPart) : 0;
      PropVariantClear(&length);
    } catch(const std::exception &) {
      // No decoder of Windows' own for it: FFmpeg's, when there is one.
      auto footage=std::make_unique<FFmpegFootage>();
      if(!footage->open(path)) throw;
      state->ffmpeg=std::move(footage);
      state->ffmpeg->setShrink(state->shrink);
      state->footage=state->ffmpeg->size();
      state->decodingOnCard=false;
    }
    state->videoPath=path;
    state->lastImage=state->earlierImage=QImage(); state->lastStart=state->lastEnd=state->earlierStart=state->earlierEnd=-1; state->ended=false;
  }
  if(state->animated) return state->animated->frame(time,start,length);
  if(state->ffmpeg) return state->ffmpeg->frame(time,start,length);
  // On the reader's own clock from here.
  time+=state->timeOffset;
  auto give=[&](const QImage &image, qint64 from, qint64 until) {
    if(start) *start=from-state->timeOffset;
    if(length) *length=std::max<qint64>(0,until-from);
    return image;
  };
  // The same picture again: a slowed or frozen stretch asks for it many times.
  if(!state->lastImage.isNull() && time>=state->lastStart && time<state->lastEnd)
    return give(state->lastImage,state->lastStart,state->lastEnd);
  if(!state->earlierImage.isNull() && time>=state->earlierStart && time<state->earlierEnd)
    return give(state->earlierImage,state->earlierStart,state->earlierEnd);
  // A little ahead of where the reader stands -- the next frame of an export,
  // of playback, of a speed-up -- is read on to. Seeking goes back to a key
  // frame and decodes forward from there, which for footage with key frames
  // seconds apart meant decoding dozens of pictures for every one wanted.
  const bool onward=!state->lastImage.isNull() && !state->ended && time>=state->lastEnd && time-state->lastEnd<20000000;
  QImage previous;
  qint64 previousStart=-1, previousEnd=-1;
  if(onward) {
    previous=state->lastImage; previousStart=state->lastStart; previousEnd=state->lastEnd;
  } else {
    // Past the end, from inside it: Windows refuses a seek beyond the footage,
    // and the time asked for there is its last picture.
    PROPVARIANT position; PropVariantInit(&position); position.vt=VT_I8;
    position.hVal.QuadPart=state->duration>0 ? std::min(time,std::max<qint64>(0,state->duration-1)) : time;
    check(state->video->SetCurrentPosition(GUID_NULL,position),"Seek video");
    state->lastImage=state->earlierImage=QImage(); state->lastStart=state->lastEnd=state->earlierStart=state->earlierEnd=-1; state->ended=false;
  }
  // The last picture passed over on the way: a time after the footage's end
  // is given it.
  ComPtr<IMFSample> passed; LONGLONG passedTime=0, passedEnd=0;
  for(int attempt=0;attempt<10000;++attempt) {
    ComPtr<IMFSample> frame; DWORD flags=0; LONGLONG timestamp=0;
    check(state->video->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&timestamp,&frame),"Read video frame");
    if(flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      state->ended=true;
      if(!previous.isNull()) return give(previous,previousStart,std::max(previousEnd,time+1));
      if(passed) {
        const QImage last=state->picture(state->video.Get(),passed.Get(),state->nv12);
        state->lastImage=last; state->lastStart=passedTime; state->lastEnd=std::max<qint64>(passedEnd,time+1);
        return give(last,passedTime,state->lastEnd);
      }
      throw std::runtime_error("Video ended before requested frame");
    }
    if(!frame) continue;
    LONGLONG duration=0; frame->GetSampleDuration(&duration);
    if(duration>0 && timestamp+duration<=time) { passed=frame; passedTime=timestamp; passedEnd=timestamp+duration; continue; }
    const QImage image=state->picture(state->video.Get(),frame.Get(),state->nv12);
    const qint64 end=duration>0 ? timestamp+duration : timestamp+1;
    state->earlierImage=state->lastImage; state->earlierStart=state->lastStart; state->earlierEnd=state->lastEnd;
    state->lastImage=image; state->lastStart=timestamp; state->lastEnd=end;
    if(timestamp>time && !previous.isNull()) return give(previous,previousStart,timestamp);
    if(timestamp>=time || (duration>0 && end>time)) return give(image,timestamp,end);
    previous=image; previousStart=timestamp; previousEnd=end;
  }
  throw std::runtime_error("Video seek exceeded decode limit");
}
bool NativeMedia::decodesOnCard() const { return state->decodingOnCard; }
void NativeMedia::setShrink(int divisor) {
  divisor=std::clamp(divisor,1,4);
  if(divisor==state->shrink) return;
  state->shrink=divisor;
  // Pictures already made are another size now.
  state->lastImage=state->earlierImage=QImage();
  state->lastStart=state->lastEnd=state->earlierStart=state->earlierEnd=-1;
  if(state->ffmpeg) state->ffmpeg->setShrink(divisor);
  if(state->animated) state->animated->setShrink(divisor);
}
QSize NativeMedia::footageSize() const { return state->footage; }
bool NativeMedia::opensVideo(const QString &path) {
  if(isAnimatedImageName(path)) { AnimatedImageFootage footage; return footage.open(path); }
  try {
    bool onCard = false;
    return state->openVideo(path, onCard) != nullptr;
  } catch (const std::exception &) {
    FFmpegFootage footage;
    return footage.open(path);
  }
}
bool NativeMedia::decodesThroughFFmpeg() const { return state->ffmpeg != nullptr; }
bool NativeMedia::readVideo(const QString &path, qint64 from,
                            const std::function<bool(qint64, const QImage &)> &picture) {
  if(isAnimatedImageName(path)) {
    AnimatedImageFootage footage;
    return footage.open(path) && footage.readOn(from,picture);
  }
  // A reader of its own, so this never disturbs the one videoFrame keeps open.
  ComPtr<IMFSourceReader> source;
  bool onCard=false, decoderPictures=false;
  try { source=state->openVideo(path,onCard,decoderPictures); } catch(const std::exception &) {
    FFmpegFootage footage;
    if(!footage.open(path)) return false;
    return footage.readOn(from,picture);
  }
  const qint64 offset=firstPictureTime(source.Get());
  if(from>0) {
    PROPVARIANT position; PropVariantInit(&position); position.vt=VT_I8; position.hVal.QuadPart=from+offset;
    check(source->SetCurrentPosition(GUID_NULL,position),"Seek video");
  }
  for(;;) {
    ComPtr<IMFSample> frame; DWORD flags=0; LONGLONG timestamp=0;
    check(source->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&timestamp,&frame),"Read video frame");
    if(flags & MF_SOURCE_READERF_ENDOFSTREAM) return true;
    if(!frame) continue;
    if(!picture(timestamp-offset,state->picture(source.Get(),frame.Get(),decoderPictures))) return true;
  }
}
bool NativeMedia::decodeAudio(const QString &path,QIODevice &pcm,const std::function<bool()> &cancel) {
  if(isAnimatedImageName(path)) return false; // an animated picture has no sound
  auto source=reader(path,false);
  ComPtr<IMFMediaType> native;
  HRESULT hr=source->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,&native);
  if(hr==MF_E_INVALIDSTREAMNUMBER) return false;
  check(hr,"Inspect audio stream");
  auto type=mediaType(MFMediaType_Audio,MFAudioFormat_PCM); audioFormat(type.Get(),false);
  check(source->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,type.Get()),"Decode stereo PCM");
  check(source->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE),"Select audio stream");
  qint64 written=0;
  for(;;) {
    if(cancel()) throw std::runtime_error("Canceled");
    ComPtr<IMFSample> audio; DWORD flags=0; LONGLONG timestamp=0;
    check(source->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,&timestamp,&audio),"Read audio");
    if(audio) {
      ComPtr<IMFMediaBuffer> buffer; check(audio->ConvertToContiguousBuffer(&buffer),"Read PCM buffer");
      BYTE *data=nullptr; DWORD bytes=0; check(buffer->Lock(&data,nullptr,&bytes),"Lock PCM buffer");
      QByteArray copy(reinterpret_cast<char *>(data),int(bytes)); buffer->Unlock();
      qint64 target=std::max<qint64>(0,timestamp*48000/10000000)*4;
      while(written<target) {
        QByteArray silence(int(std::min<qint64>(target-written,65536)),0);
        if(pcm.write(silence)!=silence.size()) throw std::runtime_error("Cannot write audio cache");
        written+=silence.size(); if(cancel()) throw std::runtime_error("Canceled");
      }
      const qint64 skip=std::clamp<qint64>(written-target,0,copy.size());
      if(pcm.write(copy.constData()+skip,copy.size()-skip)!=copy.size()-skip) throw std::runtime_error("Cannot write audio cache");
      written+=copy.size()-skip;
    }
    if(flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  return true;
}
void NativeMedia::beginMovie(const QString &path,int w,int h,int fps,int bitrate,bool audio) {
  ComPtr<IMFAttributes> attrs; check(MFCreateAttributes(&attrs,2),"Writer attributes");
  check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"Enable hardware encoders");
  check(MFCreateSinkWriterFromURL(reinterpret_cast<LPCWSTR>(path.utf16()),nullptr,attrs.Get(),&state->writer),"Create MP4 writer");
  auto output=mediaType(MFMediaType_Video,MFVideoFormat_H264);
  check(output->SetUINT32(MF_MT_AVG_BITRATE,UINT32(bitrate)),"Set H.264 bitrate");
  check(output->SetUINT32(MF_MT_MPEG2_PROFILE,100),"Set H.264 High profile");
  check(output->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"Set progressive video");
  check(MFSetAttributeSize(output.Get(),MF_MT_FRAME_SIZE,w,h),"Set output size");
  check(MFSetAttributeRatio(output.Get(),MF_MT_FRAME_RATE,fps,1),"Set frame rate");
  check(MFSetAttributeRatio(output.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1),"Set aspect ratio");
  // Rec. 709, said out loud. Left unsaid, Windows turned the pictures of an HD
  // movie into video with the Rec. 601 matrix while every other player (VLC,
  // browsers, YouTube) reads an untagged HD movie as Rec. 709: reds came out
  // 204,10,33 for 220,30,30 and greens oversaturated. Tagged, the conversion
  // and every player agree, at every size.
  check(output->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT709),"Set Rec. 709 matrix");
  check(output->SetUINT32(MF_MT_VIDEO_PRIMARIES,MFVideoPrimaries_BT709),"Set Rec. 709 primaries");
  check(output->SetUINT32(MF_MT_TRANSFER_FUNCTION,MFVideoTransFunc_709),"Set Rec. 709 transfer");
  check(output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235),"Set video range");
  check(state->writer->AddStream(output.Get(),&state->videoStream),"Add H.264 stream");
  // The encoder is handed video already made (rec709Nv12), so nothing between
  // here and the file converts colour on its own.
  auto input=mediaType(MFMediaType_Video,MFVideoFormat_NV12);
  check(input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"Set NV12 interlace");
  check(MFSetAttributeSize(input.Get(),MF_MT_FRAME_SIZE,w,h),"Set NV12 size");
  check(MFSetAttributeRatio(input.Get(),MF_MT_FRAME_RATE,fps,1),"Set NV12 rate");
  check(MFSetAttributeRatio(input.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1),"Set NV12 aspect");
  check(input->SetUINT32(MF_MT_DEFAULT_STRIDE,UINT32(w)),"Set NV12 stride");
  check(input->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT709),"Set NV12 matrix");
  check(input->SetUINT32(MF_MT_VIDEO_PRIMARIES,MFVideoPrimaries_BT709),"Set NV12 primaries");
  check(input->SetUINT32(MF_MT_TRANSFER_FUNCTION,MFVideoTransFunc_709),"Set NV12 transfer");
  check(input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235),"Set NV12 range");
  check(state->writer->SetInputMediaType(state->videoStream,input.Get(),nullptr),"Configure H.264 input");
  state->withAudio=audio;
  if(audio) {
    auto out=mediaType(MFMediaType_Audio,MFAudioFormat_AAC); audioFormat(out.Get(),true);
    check(out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,0),"Set AAC payload");
    check(out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,0x29),"Set AAC profile");
    check(state->writer->AddStream(out.Get(),&state->audioStream),"Add AAC stream");
    auto in=mediaType(MFMediaType_Audio,MFAudioFormat_PCM); audioFormat(in.Get(),false);
    check(state->writer->SetInputMediaType(state->audioStream,in.Get(),nullptr),"Configure AAC input");
  }
  check(state->writer->BeginWriting(),"Start native encoding");
}
void NativeMedia::writeFrame(const QImage &source,int f,int fps) {
  const QByteArray bytes=rec709Nv12(source);
  const qint64 begin=qint64(f)*10000000/fps, end=qint64(f+1)*10000000/fps;
  auto frame=sample(bytes,begin,end-begin); check(state->writer->WriteSample(state->videoStream,frame.Get()),"Encode video frame");
}
void NativeMedia::writeAudio(const QByteArray &pcm,qint64 firstSample) {
  if(pcm.isEmpty() || !state->withAudio) return;
  const qint64 begin=firstSample*10000000/48000, end=(firstSample+pcm.size()/4)*10000000/48000;
  auto audio=sample(pcm,begin,end-begin); check(state->writer->WriteSample(state->audioStream,audio.Get()),"Encode audio");
}
void NativeMedia::finishMovie() { check(state->writer->Finalize(),"Finalize MP4"); state->writer.Reset(); }
#else
struct NativeMedia::State {};
NativeMedia::NativeMedia() { throw std::runtime_error("Native export requires Windows"); }
NativeMedia::~NativeMedia()=default;
QImage NativeMedia::videoFrame(const QString &,qint64) { return {}; }
QImage NativeMedia::videoFrame(const QString &,qint64,qint64 *,qint64 *) { return {}; }
bool NativeMedia::readVideo(const QString &,qint64,const std::function<bool(qint64, const QImage &)> &) { return false; }
bool NativeMedia::decodeAudio(const QString &,QIODevice &,const std::function<bool()> &) { return false; }
void NativeMedia::beginMovie(const QString &,int,int,int,int,bool) {}
void NativeMedia::writeFrame(const QImage &,int,int) {}
void NativeMedia::writeAudio(const QByteArray &,qint64) {}
void NativeMedia::finishMovie() {}
#endif

int checkNativeMedia(QStringList &report) {
#ifndef Q_OS_WIN
  report << "SKIP: Windows required"; return 77;
#else
  try {
    QTemporaryDir dir;
    if(!dir.isValid()) throw std::runtime_error("Cannot create test directory");
    const QString path=dir.path()+"/test.mp4";
    NativeMedia media; media.beginMovie(path,160,96,12,2000000,true);
    for(int f=0;f<12;++f) {
      QImage image(160,96,QImage::Format_RGB32); image.fill(f<6 ? Qt::red : Qt::blue);
      { QPainter p(&image); p.fillRect(0,48,160,48,Qt::green); }
      media.writeFrame(image,f,12);
      QByteArray pcm(4000*4,0);
      for(int n=0;n<4000;++n) {
        qint16 value=qint16(std::sin((f*4000+n)*6.283185307179586*440/48000)*8000);
        memcpy(pcm.data()+n*4,&value,2); memcpy(pcm.data()+n*4+2,&value,2);
      }
      media.writeAudio(pcm,f*4000);
    }
    media.finishMovie();
    auto first=media.videoFrame(path,0), last=media.videoFrame(path,8000000);
    auto red=first.pixelColor(80,20), green=first.pixelColor(80,76), blue=last.pixelColor(80,20);
    if(!(red.red()>180 && red.blue()<80 && green.green()>180 && green.red()<80 && blue.blue()>180 && blue.red()<80))
      throw std::runtime_error("Decoded color, orientation or frame time mismatch");
    report << "PASS: MP4 encode/decode, frame time and orientation";
    {
      // On the card and on the processor, the same picture: colours from
      // the same Rec. 709 reading, nothing shifted or cropped.
      const QString smooth=dir.path()+"/smooth.mp4";
      NativeMedia writer; writer.beginMovie(smooth,640,360,12,6000000,false);
      for(int f=0;f<6;++f) {
        QImage image(640,360,QImage::Format_RGB32);
        for(int y=0;y<360;++y) for(int x=0;x<640;++x) image.setPixel(x,y,qRgb(x*255/639,y*255/359,(x+y+f*20)%256));
        writer.writeFrame(image,f,12);
      }
      writer.finishMovie();
      NativeMedia card;
      const QImage onCard=card.videoFrame(smooth,2*833333+1000).convertToFormat(QImage::Format_RGB32);
      qputenv("WAVY_VIDEO_DECODE","software");
      NativeMedia processor;
      const QImage onProcessor=processor.videoFrame(smooth,2*833333+1000).convertToFormat(QImage::Format_RGB32);
      qunsetenv("WAVY_VIDEO_DECODE");
      if(onCard.size()!=onProcessor.size() || onCard.size()!=QSize(640,360)) throw std::runtime_error("Card and processor decode different sizes");
      double sum=0;
      for(int y=0;y<360;++y) for(int x=0;x<640;++x) {
        const QRgb a=onCard.pixel(x,y), b=onProcessor.pixel(x,y);
        sum+=std::abs(qRed(a)-qRed(b))+std::abs(qGreen(a)-qGreen(b))+std::abs(qBlue(a)-qBlue(b));
      }
      const double mean=sum/(640.*360*3);
      if(mean>2) throw std::runtime_error(QString("Card decode differs from the processor's by %1 per channel").arg(mean).toStdString());
      report << QString("PASS: video decoded %1 matches the processor's (mean difference %2 per channel)")
                    .arg(card.decodesOnCard() ? "on the graphics card" : "on the processor (no card decoder here)").arg(mean,0,'f',2);
      // Made smaller for a viewer: half the size, the footage still known
      // whole, and the picture the whole one made smaller.
      NativeMedia half;
      half.setShrink(2);
      const QImage reduced=half.videoFrame(smooth,2*833333+1000).convertToFormat(QImage::Format_RGB32);
      const QImage expected=onCard.scaled(reduced.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
      double off=0;
      for(int y=0;y<reduced.height();++y) for(int x=0;x<reduced.width();++x) {
        const QRgb a=reduced.pixel(x,y), b=expected.pixel(x,y);
        off+=std::abs(qRed(a)-qRed(b))+std::abs(qGreen(a)-qGreen(b))+std::abs(qBlue(a)-qBlue(b));
      }
      off/=double(std::max(1,reduced.width()*reduced.height()))*3;
      const bool shrunk=half.decodesOnCard() ? reduced.size()==QSize(320,180) : reduced.size()==QSize(640,360);
      if(!shrunk || half.footageSize()!=QSize(640,360) || off>3)
        throw std::runtime_error(QString("A picture made smaller is %1x%2 of %3x%4, %5 off").arg(reduced.width()).arg(reduced.height())
                                     .arg(half.footageSize().width()).arg(half.footageSize().height()).arg(off).toStdString());
      report << QString("PASS: footage made half the size for a viewer, %1x%2 of %3x%4 (mean difference %5)")
                    .arg(reduced.width()).arg(reduced.height()).arg(half.footageSize().width()).arg(half.footageSize().height()).arg(off,0,'f',2);
    }
    QFile pcm(dir.path()+"/decoded.pcm");
    if(!pcm.open(QIODevice::ReadWrite) || !media.decodeAudio(path,pcm,[]{return false;})) throw std::runtime_error("Audio decode failed");
    if(pcm.size()<180000 || pcm.size()>220000) throw std::runtime_error("Unexpected decoded audio duration");
    pcm.seek(0); auto audio=pcm.readAll(); bool sound=false;
    for(qsizetype i=0;i+1<audio.size();i+=2) { qint16 v; memcpy(&v,audio.constData()+i,2); sound|=std::abs(int(v))>1000; }
    if(!sound) throw std::runtime_error("Decoded audio is silent");
    report << "PASS: AAC encode and PCM audio decode";
    {
      // Footage whose times Windows reads differently, or not at all, made by
      // FFmpeg when it is here: an MP4 with B-frames (its first picture said
      // 0.133 s, so every picture came four frames late) and ProRes (no
      // decoder in Windows; read through FFmpeg). Picture k is grey 23k, so
      // which picture came back is plain to see.
      const QString program=footageFFmpeg();
      if(program.isEmpty()) report << "SKIP: B-frame and ProRes footage (no FFmpeg here)";
      else {
        const QString pattern="color=c=black:s=320x240:r=30:d=0.4,format=yuv420p,geq=lum='16+N*20':cb=128:cr=128";
        auto make=[&](const QString &file, const QStringList &codec) {
          QProcess ffmpeg;
          ffmpeg.start(program,QStringList{"-v","error","-y","-f","lavfi","-i",pattern}+codec+QStringList{file});
          return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode()==0 && QFileInfo(file).size()>0;
        };
        const QString bframes=dir.path()+"/bframes.mp4", prores=dir.path()+"/count.mov";
        if(!make(prores,{"-c:v","prores","-profile:v","2"})) throw std::runtime_error("FFmpeg could not make ProRes footage");
        // H.264 with B-frames needs an encoder that makes them; FFmpeg's own
        // builds here have only the cards'. (Its MPEG-4 Part 2 has B-frames,
        // but Windows decodes those black.)
        bool withBframes=false;
        for(const char *encoder:{"h264_nvenc","h264_amf","h264_qsv"})
          if((withBframes=make(bframes,{"-c:v",encoder,"-bf","2"}))) break;
        if(!withBframes) report << "SKIP: H.264 with B-frames (no encoder here that makes them)";
        std::vector<std::pair<QString,QString>> files{{prores,"ProRes"}};
        if(withBframes) files.push_back({bframes,"an H.264 MP4 with B-frames"});
        for(const auto &[file,label]:files) {
          NativeMedia footage;
          QString wrong;
          for(int k:{0,1,2,3,7,11,4,10,0,6}) {
            qint64 from=0;
            const QImage picture=footage.videoFrame(file,qint64(k)*333333+1000,&from,nullptr);
            const int grey=qRed(picture.pixel(160,120));
            if(std::abs(grey-k*20*255/219)>8 || std::abs(from-qint64(k)*333333)>100) wrong+=QString(" %1 (grey %2 at %3 ms)").arg(k).arg(grey).arg(from/1e4);
          }
          if(!wrong.isEmpty()) throw std::runtime_error(QString("%1: pictures not where they belong:%2").arg(label,wrong).toStdString());
          report << QString("PASS: %1 gives each picture at its own time (%2)").arg(label,footage.decodesThroughFFmpeg() ? "through FFmpeg" : "Windows' decoder");
        }
      }
    }
    {
      // Pictures Qt here cannot read, and a GIF's frames by their own times;
      // then the same GIF as footage, as the editor reads it.
      if(checkImageFiles(report)!=0) throw std::runtime_error("Picture files: see above");
      const QString gif=dir.path()+"/moving.gif";
      QFile file(gif);
      if(!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write the GIF");
      file.write(solidAnimatedGif(QSize(16,8),{{qRgb(255,0,0),10},{qRgb(0,255,0),20},{qRgb(0,0,255),30}}));
      file.close();
      NativeMedia footage;
      if(!footage.opensVideo(gif)) throw std::runtime_error("An animated GIF does not open as footage");
      QString wrong;
      for(const auto &[ms,channel]:std::vector<std::pair<int,int>>{{50,0},{150,1},{450,2},{700,1},{30,0}}) {
        qint64 from=-1;
        const QImage picture=footage.videoFrame(gif,qint64(ms)*10000,&from,nullptr);
        const QRgb c=picture.isNull() ? 0 : picture.pixel(8,4);
        const int got=qRed(c)>180 ? 0 : qGreen(c)>180 ? 1 : qBlue(c)>180 ? 2 : -1;
        if(got!=channel) wrong+=QString(" %1 ms").arg(ms);
      }
      int read=0;
      footage.readVideo(gif,0,[&](qint64 time,const QImage &){ if(time>=6000000) return false; ++read; return true; });
      QBuffer sound; sound.open(QIODevice::ReadWrite);
      if(!wrong.isEmpty() || read!=3 || footage.footageSize()!=QSize(16,8) || footage.decodeAudio(gif,sound,[]{return false;}))
        throw std::runtime_error(QString("An animated GIF as footage: wrong frames at%1, %2 read").arg(wrong).arg(read).toStdString());
      report << "PASS: an animated GIF plays as footage, each frame at its own time, over and over, with no sound";
    }
    return 0;
  } catch(const std::exception &error) { report << "FAIL: "+QString::fromUtf8(error.what()); return 1; }
#endif
}
