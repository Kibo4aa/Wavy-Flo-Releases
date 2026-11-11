#pragma once
#include <QtGui>
#include <stdexcept>
#include <QtEndian>
#include <cstring>
#include <cmath>
#include <algorithm>
// PSD v1 RGB/grayscale composite, 8/16/32 bpc, raw/RLE/ZIP without prediction.
// Layer extraction, composite alpha and ZIP prediction are not implemented.
inline QImage decodePSD(const QByteArray &data, QString &error) {
  try {
    qsizetype pos = 0;
    auto take = [&](qsizetype n) {
      if (n < 0 || n > data.size() - pos)
        throw std::runtime_error("PSD terpotong");
      auto start = pos;
      pos += n;
      return start;
    };
    auto u8 = [&]() { return quint8(data[take(1)]); };
    auto u16 = [&]() {
      auto p = take(2);
      return (quint16(quint8(data[p])) << 8) | quint8(data[p + 1]);
    };
    auto u32 = [&]() {
      auto p = take(4);
      quint32 v = 0;
      for (int i = 0; i < 4; i++)
        v = (v << 8) | quint8(data[p + i]);
      return v;
    };
    if (data.mid(take(4), 4) != "8BPS" || u16() != 1)
      throw std::runtime_error("Gunakan PSD versi 1");
    take(6);
    int channels = u16();
    quint32 height = u32(), width = u32();
    int depth = u16(), mode = u16();
    if ((depth != 8 && depth != 16 && depth != 32) || (mode != 1 && mode != 3) ||
        channels < (mode == 3 ? 3 : 1) || channels > 56)
      throw std::runtime_error("Gunakan PSD RGB/grayscale 8/16/32-bit");
    if (!width || !height || quint64(width) * height > 32000000)
      throw std::runtime_error("PSD maksimum 32 megapixel");
    for (int i = 0; i < 3; i++) {
      quint32 n = u32();
      take(n);
    }
    int compression = u16();
    const int rowBytes = int(width) * (depth/8);
    int count = rowBytes * height;
    QVector<QByteArray> planes;
    if (compression == 0) {
      for (int c = 0; c < channels; c++) {
        auto p = take(count);
        if (c < 3)
          planes.push_back(data.mid(p, count));
      }
    } else if (compression == 1) {
      QVector<int> lengths;
      for (quint64 i = 0; i < quint64(channels) * height; i++)
        lengths.push_back(u16());
      for (int c = 0; c < channels; c++) {
        QByteArray plane(c < 3 ? count : 0, 0);
        for (quint32 y = 0; y < height; y++) {
          auto end = pos + lengths[c * height + y];
          if (end > data.size())
            throw std::runtime_error("PSD RLE terpotong");
          int x = 0;
          while (pos < end) {
            int n = u8();
            if (n == 128)
              continue;
            int len = n < 128 ? n + 1 : 257 - n;
            if (x + len > rowBytes)
              throw std::runtime_error("PSD RLE tidak valid");
            if (n < 128) {
              auto p = take(len);
              if (pos > end)
                throw std::runtime_error("PSD RLE tidak valid");
              if (c < 3)
                memcpy(plane.data() + y * rowBytes + x, data.constData() + p, len);
            } else {
              int value = u8();
              if (pos > end)
                throw std::runtime_error("PSD RLE tidak valid");
              if (c < 3)
                memset(plane.data() + y * rowBytes + x, value, len);
            }
            x += len;
          }
          if (x != rowBytes)
            throw std::runtime_error("PSD RLE tidak lengkap");
        }
        if (c < 3)
          planes.push_back(plane);
      }
    } else if (compression == 2) {
      const quint64 expected=quint64(count)*channels;
      if(expected>512*1024*1024) throw std::runtime_error("PSD decoded ZIP exceeds 512 MB");
      QByteArray packed(4,0); qToBigEndian<quint32>(quint32(expected),reinterpret_cast<uchar *>(packed.data()));
      packed.append(data.mid(pos));
      auto decoded=qUncompress(packed);
      if(quint64(decoded.size())!=expected) throw std::runtime_error("PSD ZIP data invalid");
      for(int c=0;c<std::min(channels,3);++c) planes.push_back(decoded.mid(qsizetype(c)*count,count));
    } else
      throw std::runtime_error(
          "PSD ZIP prediction belum didukung; simpan dengan RAW/RLE/ZIP tanpa prediction");
    QImage image(width, height, depth==32 ? QImage::Format_RGBA32FPx4 : depth==16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (image.isNull())
      throw std::runtime_error("Memori gambar tidak cukup");
    for (quint32 y = 0; y < height; y++)
      for (quint32 x = 0; x < width; x++) {
        int i = y * width + x;
        if (depth > 8) {
          float values[3]; quint16 integers[3]{};
          for(int c=0;c<3;++c) {
            const auto *src=reinterpret_cast<const uchar *>(planes[mode==3?c:0].constData())+qsizetype(i)*(depth/8);
            if(depth==16) integers[c]=qFromBigEndian<quint16>(src);
            else { auto bits=qFromBigEndian<quint32>(src); std::memcpy(&values[c],&bits,4);
              if(!std::isfinite(values[c])) throw std::runtime_error("PSD contains non-finite samples"); }
          }
          if(depth==16) reinterpret_cast<QRgba64 *>(image.scanLine(y))[x]=QRgba64::fromRgba64(integers[0],integers[1],integers[2],65535);
          else {auto *p=reinterpret_cast<float *>(image.scanLine(y))+x*4; p[0]=values[0];p[1]=values[1];p[2]=values[2];p[3]=1;}
          continue;
        }
        auto *p = image.scanLine(y) + x * 4;
        p[0] = quint8(planes[0][i]);
        p[1] = quint8(planes[mode == 3 ? 1 : 0][i]);
        p[2] = quint8(planes[mode == 3 ? 2 : 0][i]);
        p[3] = 255;
      }
    return image;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
