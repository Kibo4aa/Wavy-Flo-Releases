#include "render_export.h"
#include <map>
#include <QColorSpace>
#include "native_media.h"
#include "video_frames.h"
#include "ffmpeg_export.h"
#include "video_timing.h"
#include <QStandardItemModel>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>
#include <future>
#include <thread>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
namespace {
// A layer's sound, decoded. A remapped one is read along its curve, from
// anywhere in the footage, so it is held whole in memory.
struct Track { Layer layer; std::shared_ptr<QFile> pcm; QByteArray whole; };
QByteArray mix(std::vector<Track> &tracks,qint64 first,qint64 count,int fps) {
  std::vector<qint64> sum(size_t(count)*2,0);
  for(auto &track:tracks) {
    const auto &l=track.layer;
    if(l.timeRemap) {
      // Slowed, sped up, run backwards or held with the picture.
      addRemappedSound(reinterpret_cast<const int16_t *>(track.whole.constData()),track.whole.size()/4,l,fps,first,count,sum.data());
      continue;
    }
    qint64 start=qRound64(l.start*48000/fps),stop=qRound64((l.start+l.duration)*48000/fps);
    qint64 begin=std::max(first,start),end=std::min(first+count,stop);
    if(begin>=end) continue;
    qint64 offset=qRound64(l.sourceOffsetMs*48)+(begin-start);
    if(offset<0 || !track.pcm->seek(offset*4)) throw std::runtime_error("Cannot seek audio cache");
    auto bytes=track.pcm->read((end-begin)*4);
    for(qint64 n=0;n<bytes.size()/2;++n)
      sum[size_t((begin-first)*2+n)]+=qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(bytes.constData()+n*2));
  }
  QByteArray result(count*4,0);
  for(qint64 n=0;n<count*2;++n) qToLittleEndian<qint16>(qint16(std::clamp<qint64>(sum[size_t(n)],-32768,32767)),reinterpret_cast<uchar *>(result.data()+n*2));
  return result;
}
void wavHeader(QFile &file,quint32 bytes) {
  QDataStream s(&file); s.setByteOrder(QDataStream::LittleEndian);
  s.writeRawData("RIFF",4); s<<quint32(bytes+36); s.writeRawData("WAVEfmt ",8);
  s<<quint32(16)<<quint16(1)<<quint16(2)<<quint32(48000)<<quint32(192000)<<quint16(4)<<quint16(16);
  s.writeRawData("data",4); s<<bytes;
  if(s.status()!=QDataStream::Ok) throw std::runtime_error("Cannot write WAV header");
}
}
QStringList renderFormatNames() {
  return {"MP4 — H.264 / AAC (Windows)", "PNG Sequence",
          "MOV — Animation (FFmpeg)", "MOV — PNG (FFmpeg)",
          "MOV — ProRes 4444 (FFmpeg)"};
}
void renderExport(QWidget *parent,Scene scene,std::function<QImage(Scene &)> render) {
#ifndef Q_OS_WIN
  QMessageBox::information(parent,"Render","Native export requires Windows."); return;
#endif
  QDialog dialog(parent);
  dialog.setWindowTitle("Render Main");
  QFormLayout layout(&dialog);
  QComboBox format;
  format.addItems(renderFormatNames());
  format.setCurrentIndex(std::clamp(QSettings().value("render/format",0).toInt(),0,4));
  // The project's depth unless a render is to be another (Project Settings
  // sets the project's).
  QComboBox depth;
  depth.addItem(QString("As the project (%1 bpc)").arg(scene.processingDepth),0);
  depth.addItem("8 bpc",8);
  depth.addItem("16 bpc",16);
  depth.addItem("32 bpc float",32);
  depth.setCurrentIndex(std::max(0,depth.findData(QSettings().value("render/depth",0).toInt())));
  QSpinBox bitrate;
  bitrate.setRange(1,2000);
  bitrate.setValue(std::clamp(QSettings().value("render/mp4BitrateMbps",50).toInt(),1,2000));
  bitrate.setSuffix(" Mbps");
  bitrate.setToolTip("Target MP4 bitrate. Actual output depends on the Windows encoder and image content.");
  QCheckBox audio("Include audio");
  audio.setChecked(QSettings().value("render/includeAudio",true).toBool());
  QCheckBox alpha("Preserve transparency");
  alpha.setChecked(QSettings().value("render/preserveTransparency",false).toBool());
  QLabel note;
  note.setWordWrap(true);
  QLineEdit ffmpeg(findExportFFmpeg());
  ffmpeg.setPlaceholderText("Choose an LGPL ffmpeg.exe");
  QWidget ffmpegControls;
  QHBoxLayout ffmpegLayout(&ffmpegControls);
  ffmpegLayout.setContentsMargins(0,0,0,0);
  QPushButton browse("Browse…"), licenses("Licenses…");
  ffmpegLayout.addWidget(&ffmpeg);
  ffmpegLayout.addWidget(&browse);
  ffmpegLayout.addWidget(&licenses);
  QObject::connect(&browse,&QPushButton::clicked,&dialog,[&] {
    const auto path=QFileDialog::getOpenFileName(&dialog,"Select LGPL FFmpeg",ffmpeg.text(),
                                                 "FFmpeg (ffmpeg.exe)");
    if(!path.isEmpty()) ffmpeg.setText(path);
  });
  QObject::connect(&licenses,&QPushButton::clicked,&dialog,[&] { showFFmpegLicenses(&dialog); });
  layout.addRow("Format",&format);
  layout.addRow("Color depth",&depth);
  layout.addRow("Video bitrate",&bitrate);
  layout.addRow(&audio);
  layout.addRow(&alpha);
  layout.addRow(&note);
  layout.addRow("FFmpeg",&ffmpegControls);
  auto update=[&] {
    const bool external=format.currentIndex()>=2;
    alpha.setEnabled(format.currentIndex()!=0);
    bitrate.setVisible(format.currentIndex()==0);
    if(auto *label=layout.labelForField(&bitrate)) label->setVisible(format.currentIndex()==0);
    ffmpegControls.setVisible(external);
    if(auto *label=layout.labelForField(&ffmpegControls)) label->setVisible(external);
    note.setText("16/32 bpc processing uses CPU. PNG saves 16-bit channels for 16/32 bpc processing (float HDR values are clipped to 0–1). Animation and MP4 output 8-bit. MOV PNG supports 16-bit; ProRes uses its codec precision. PNG audio is a separate WAV.");
    if(format.currentIndex()==0) alpha.setChecked(false);
  };
  QObject::connect(&format,&QComboBox::currentIndexChanged,&dialog,[&]{update();});
  update();
  QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
  layout.addRow(&buttons);
  QObject::connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
  QObject::connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
  if(dialog.exec()!=QDialog::Accepted) return;
  RenderSettings settings;
  settings.format=format.currentIndex();
  settings.processingDepth=depth.currentData().toInt();
  settings.bitrateMbps=bitrate.value();
  settings.includeAudio=audio.isChecked();
  settings.preserveTransparency=alpha.isChecked();
  settings.ffmpegPath=ffmpeg.text();
  QSettings saved;
  saved.setValue("render/format",settings.format);
  saved.setValue("render/depth",settings.processingDepth);
  saved.setValue("render/mp4BitrateMbps",settings.bitrateMbps);
  saved.setValue("render/includeAudio",settings.includeAudio);
  saved.setValue("render/preserveTransparency",settings.preserveTransparency);
  saved.setValue("render/ffmpegPath",settings.ffmpegPath);
  renderExport(parent,scene,settings,std::move(render));
}
QString renderToFile(Scene scene,const RenderSettings &settings,const QString &target,
                     std::function<QImage(Scene &)> render,
                     const std::function<bool(int,int,const QString &)> &progress,bool checkFFmpeg) {
#ifndef Q_OS_WIN
  return "Native export requires Windows.";
#endif
  const int format=std::clamp(settings.format,0,4);
  // Another depth than the project's only when the render asks for one.
  if(settings.processingDepth==8 || settings.processingDepth==16 || settings.processingDepth==32)
    scene.processingDepth=settings.processingDepth;
  bool movie=format==0, external=format>=2;
  auto codec=static_cast<FFmpegMovie>(std::max(0,format-2));
  QString ffmpegPath=QFileInfo(settings.ffmpegPath.trimmed()).absoluteFilePath();
  if(scene.width<=0 || scene.height<=0 || scene.fps<=0 || scene.frames<=0 || quint64(scene.width)*scene.height>33000000)
    return "Invalid Main size or duration.";
  const int firstFrame=std::clamp(settings.firstFrame,0,scene.frames-1);
  const int lastFrame=settings.lastFrame<0 ? scene.frames-1 : std::clamp(settings.lastFrame,firstFrame,scene.frames-1);
  const int count=lastFrame-firstFrame+1;
  const qint64 firstSample=qint64(firstFrame)*48000/scene.fps;
  int done=0; QString label="Preparing render\u2026";
  // Each check-in says how far it has got and whether to go on.
  auto canceled=[&] { return progress && !progress(done,count+1,label); };
  if(movie && (scene.width%2 || scene.height%2)) return "H.264 requires even width and height.";
  if(target.isEmpty()) return "No output was chosen.";
  QString error;
  try {
    if(external && checkFFmpeg) {
      label="Checking FFmpeg LGPL build\u2026";
      checkExportFFmpeg(ffmpegPath,codec,settings.includeAudio,canceled);
    }
    QTemporaryDir staging(QFileInfo(target).absolutePath()+"/.wavy-render-XXXXXX");
    if(!staging.isValid()) throw std::runtime_error("Cannot create staging folder.");
    NativeMedia media; std::vector<Track> tracks;
    std::map<QString,std::unique_ptr<NativeMedia>> footageReaders;
    if(settings.includeAudio) {
      int index=0;
      for(const auto &l:scene.layers) {
        if((l.type!="video" && l.type!="audio") || !l.audioEnabled || !l.visible || l.start>lastFrame || l.start+l.duration<=firstFrame) continue;
        label="Decoding audio: "+l.name;
        if(canceled()) throw std::runtime_error("Canceled");
        auto file=std::make_shared<QFile>(staging.path()+QString("/track-%1.pcm").arg(index++));
        if(!file->open(QIODevice::ReadWrite)) throw std::runtime_error("Cannot create audio cache");
        if(media.decodeAudio(l.path,*file,canceled)) {
          QByteArray whole;
          if(l.timeRemap && file->seek(0)) whole=file->readAll();
          tracks.push_back({l,file,whole});
        }
      }
    }
    qint64 totalSamples=qint64(lastFrame+1)*48000/scene.fps-firstSample;
    QString resultDir=staging.path()+"/sequence", encoded=staging.path()+(external ? "/output.mov" : "/output.mp4"); QFile wav;
    if(movie) media.beginMovie(encoded,scene.width,scene.height,scene.fps,
                               std::clamp(settings.bitrateMbps,1,2000)*1000000,!tracks.empty());
    else {
      if(!QDir().mkdir(resultDir)) throw std::runtime_error("Cannot create sequence staging folder");
      if(!tracks.empty()) {
        if(totalSamples*4>0xffffffffLL-36) throw std::runtime_error("WAV exceeds 4 GB; render a shorter Main");
        wav.setFileName(resultDir+"/audio.wav"); if(!wav.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create WAV");
        wavHeader(wav,quint32(totalSamples*4));
      }
    }
    std::unique_ptr<FFmpegPipe> pipe;
    if(external) {
      // The whole soundtrack first, so FFmpeg can read it from the start while
      // the frames come down the pipe.
      if(wav.isOpen()) {
        label="Mixing audio\u2026";
        for(int f=0;f<count;++f) {
          if(canceled()) throw std::runtime_error("Canceled");
          qint64 first=qint64(firstFrame+f)*48000/scene.fps,end=qint64(firstFrame+f+1)*48000/scene.fps;
          QByteArray pcm=mix(tracks,first,end-first,scene.fps);
          if(wav.write(pcm)!=pcm.size()) throw std::runtime_error("Cannot write WAV samples");
        }
        if(!wav.flush()) throw std::runtime_error("Cannot flush WAV output");
        wav.close();
      }
      pipe=std::make_unique<FFmpegPipe>(ffmpegPath,codec,tracks.empty()?QString():resultDir+"/audio.wav",encoded,
                                        scene.width,scene.height,scene.fps,count,settings.preserveTransparency,
                                        scene.processingDepth);
    }
    struct FrameResult { QImage image; QByteArray png; };
    const bool sequence=!movie && !external;
    std::vector<std::future<FrameResult>> frameJobs;
    const int maxConcurrentFrames = std::max(1, QThread::idealThreadCount() / 2);

    // f counts the output's frames; the Main's frame is firstFrame on from it.
    int nextDispatch = 0;
    for(int f=0;f<count;++f) {
      done=f; label=QString("Rendering frame %1 / %2").arg(f+1).arg(count);
      if(canceled()) throw std::runtime_error("Canceled");

      while (nextDispatch < count && frameJobs.size() < maxConcurrentFrames) {
        const int at = firstFrame + nextDispatch;
        Scene frameScene = scene;
        frameScene.frame = at;
        for(auto &l:frameScene.layers) {
          if(l.type!="video" || !videoTiming(l,at,frameScene.fps).active) continue;
          // Time Remap and Frame Mix included; the reader reads on from one
          // frame to the next rather than seeking for each. A reader for each
          // layer: one shared by two video layers reopened the file and sought
          // for every layer of every frame.
          auto &reader=footageReaders[l.id];
          if(!reader) reader=std::make_unique<NativeMedia>();
          l.image=footageFrame(*reader,l.path,layerFootageTime(l,at-l.start,frameScene.fps),l.frameBlend==1);
        }

        frameJobs.push_back(std::async(std::launch::async, [frameScene, render, movie, sequence, settings]() mutable {
          QImage image=render(frameScene); if(image.isNull()) throw std::runtime_error("Renderer returned an empty frame");
          if(movie || !settings.preserveTransparency) {
            QImage opaque(image.size(),processingFormat(frameScene.processingDepth)); opaque.fill(frameScene.background);
            QPainter painter(&opaque); painter.drawImage(0,0,image); painter.end(); image=opaque;
          }
          if(sequence) {
            // Packed here, beside the rendering, not one by one as the frames
            // are written: a 1080p PNG takes about a second to pack, so the
            // packing was the whole cost of a sequence -- 160 s for 150 frames.
            // Tagged sRGB, which is what the pixels are.
            QImage png=image.convertToFormat(processingFormat(frameScene.processingDepth>8?16:8));
            png.setColorSpace(QColorSpace(QColorSpace::SRgb));
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
            if(!png.save(&buffer,"PNG")) throw std::runtime_error("Cannot pack PNG frame");
            return FrameResult{QImage(),bytes};
          }
          return FrameResult{image,{}};
        }));
        nextDispatch++;
      }

      FrameResult result = frameJobs.front().get();
      frameJobs.erase(frameJobs.begin());

      if(movie) media.writeFrame(result.image,f,scene.fps);
      else if(pipe) pipe->write(result.image,canceled);
      else {
        QFile png(resultDir+QString("/frame-%1.png").arg(f,6,10,QChar('0')));
        if(!png.open(QIODevice::WriteOnly) || png.write(result.png)!=result.png.size()) throw std::runtime_error("Cannot write PNG frame");
      }
      if(!tracks.empty() && !pipe) {
        qint64 first=qint64(firstFrame+f)*48000/scene.fps,end=qint64(firstFrame+f+1)*48000/scene.fps;
        QByteArray pcm=mix(tracks,first,end-first,scene.fps);
        if(movie) media.writeAudio(pcm,first-firstSample);
        else if(wav.write(pcm)!=pcm.size()) throw std::runtime_error("Cannot write WAV samples");
      }
    }
    done=count; label="Finalizing output\u2026";
    if(canceled()) throw std::runtime_error("Canceled");
    if(wav.isOpen()) { if(!wav.flush()) throw std::runtime_error("Cannot flush WAV output"); wav.close(); }
    if(pipe) {
      label="Finishing MOV with FFmpeg\u2026";
      pipe->finish(canceled);
      pipe.reset();
    }
    if(movie || external) {
      if(movie) media.finishMovie();
      // The staging folder is beside the target, so the finished movie is moved
      // into place, not written a second time.
#ifdef Q_OS_WIN
      if(MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(encoded).utf16()),
                     reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
                     MOVEFILE_REPLACE_EXISTING)) { done=count+1; return error; }
#endif
      QSaveFile destination(target); QFile source(encoded);
      if(!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open final output");
      while(!source.atEnd()) {
        if(canceled()) throw std::runtime_error("Canceled"); auto bytes=source.read(1024*1024);
        if(bytes.isEmpty() || destination.write(bytes)!=bytes.size()) throw std::runtime_error("Cannot write final output");
      }
      if(!destination.commit()) throw std::runtime_error("Cannot commit movie output");
    } else {
      if(!QDir().rename(resultDir,target)) throw std::runtime_error("Cannot publish PNG sequence");
    }
    done=count+1;
  } catch(const std::exception &e) { error=QString::fromUtf8(e.what()); }
  return error;
}
void renderExport(QWidget *parent,Scene scene,const RenderSettings &settings,
                  std::function<QImage(Scene &)> render) {
#ifndef Q_OS_WIN
  QMessageBox::information(parent,"Render","Native export requires Windows."); return;
#endif
  const int format=std::clamp(settings.format,0,4);
  bool movie=format==0, external=format>=2;
  auto codec=static_cast<FFmpegMovie>(std::max(0,format-2));
  QString ffmpegPath=QFileInfo(settings.ffmpegPath.trimmed()).absoluteFilePath();
  if(external) {
    QProgressDialog checking("Checking FFmpeg LGPL build\u2026","Cancel",0,0,parent);
    checking.setWindowModality(Qt::ApplicationModal); checking.setMinimumDuration(0);
    try {
      checkExportFFmpeg(ffmpegPath,codec,settings.includeAudio,[&] { QApplication::processEvents(); return checking.wasCanceled(); });
      QSettings().setValue("render/ffmpegPath",ffmpegPath);
    } catch(const std::exception &e) {
      checking.close();
      if(QString::fromUtf8(e.what())!="Canceled") QMessageBox::warning(parent,"FFmpeg",QString::fromUtf8(e.what()));
      return;
    }
  }
  if(scene.width<=0 || scene.height<=0 || scene.fps<=0 || scene.frames<=0 || quint64(scene.width)*scene.height>33000000) {
    QMessageBox::warning(parent,"Render","Invalid Main size or duration."); return;
  }
  if(movie && (scene.width%2 || scene.height%2)) { QMessageBox::warning(parent,"Render","H.264 requires even width and height."); return; }
  QString target;
  if(movie || external) {
    QString extension=external ? ".mov" : ".mp4";
    target=QFileDialog::getSaveFileName(parent,"Render movie",{},external ? "MOV (*.mov)" : "MP4 (*.mp4)"); if(target.isEmpty()) return;
    if(!target.endsWith(extension,Qt::CaseInsensitive)) {
      target+=extension;
      if(QFileInfo::exists(target) && QMessageBox::question(parent,"Replace output?",target)!=QMessageBox::Yes) return;
    }
  } else {
    auto folder=QFileDialog::getExistingDirectory(parent,"Choose output parent folder"); if(folder.isEmpty()) return;
    target=folder+"/WavyFloRender-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
  }
  QProgressDialog progress("Preparing render\u2026","Cancel",0,scene.frames+1,parent);
  progress.setWindowModality(Qt::ApplicationModal); progress.setMinimumDuration(0); progress.setAutoClose(false);
  const QString error=renderToFile(scene,settings,target,std::move(render),[&](int done,int total,const QString &label) {
    progress.setRange(0,total); progress.setValue(done); progress.setLabelText(label);
    QApplication::processEvents();
    return !progress.wasCanceled();
  },false);
  progress.close(); if(error=="Canceled") return;
  if(!error.isEmpty()) QMessageBox::warning(parent,"Render failed",error);
  else QMessageBox::information(parent,"Render complete",target);
}
