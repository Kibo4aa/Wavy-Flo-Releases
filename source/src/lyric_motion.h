#pragma once
// Lyric Motion: sung lines laid out as moving type, in time with the song.
//
// A lyrics file is read as it is (an .lrc with [mm:ss.xx] times, or plain lines
// with none); plain lines are laid over the song's sung parts, bar by bar. Each
// line then becomes a handful of layers -- the words, a large key character on a
// highlight, a rule drawn on beside them, a small line number -- that arrive as
// the line is sung, drift while it lasts and leave before the next. Quiet lines
// are set small and written on by hand; lines over the loud parts of the song
// are set large and punch in, pulsing with the beat.
#include "engine.h"
#include <QByteArray>
#include <QString>
#include <vector>

struct AudioPeaks;
struct BeatAnalysis;

struct LyricCue {
  double start = 0, end = 0; // seconds into the song
  QString text;
};

struct LyricMotionOptions {
  QString font = "Yu Gothic";   // the words
  QString keyFont = "Yu Gothic"; // the large key character, and loud lines
  QColor ink = QColor("#fff4f8"), accent = QColor("#ff7fae");
  // 0 by the music (quiet lines written on, loud lines punched in), 1 always
  // written on, 2 always punched in.
  int style = 0;
  std::vector<double> beats;                     // seconds, for pulses and for "loud"
  std::vector<std::pair<double, double>> loud;   // spans of the song, in seconds, set large
  std::vector<QPointF> places;                   // where quiet lines may sit; empty for the thirds
  double fps = 30;
  int width = 1920, height = 1080;
  double songStartFrame = 0; // where second 0 of the song sits on the timeline
};

// Reads an .lrc (times kept, "[ti:...]" tags dropped) or plain text (one line
// each). `timed` says which it was. Ends are the next line's start, at most
// eight seconds on.
std::vector<LyricCue> parseLyricCues(const QString &content, bool *timed);
// Plain lines laid over the song: from where the singing is likely to begin to
// where the music ends, one line to every bar or two, on the downbeats, leaving
// the quiet stretches between parts empty. A first draft to be tapped in by ear.
void fitLyricCues(std::vector<LyricCue> &, const AudioPeaks &, const BeatAnalysis &);
// Where somebody is singing: a score every 20 ms from 16-bit stereo PCM at
// 48 kHz (the middle of the mix, holding a pitch), and the stretches of it.
std::vector<float> voiceScore(const QByteArray &pcm);
std::vector<std::pair<double, double>> voicePhrases(const std::vector<float> &score);
// Lines laid on the singing: each begins where a phrase of singing begins, the
// beginnings chosen so every line gets about the singing its letters need.
void fitLyricCuesToVoice(std::vector<LyricCue> &, const std::vector<std::pair<double, double>> &phrases);
// The loud stretches of the song, for LyricMotionOptions::loud.
std::vector<std::pair<double, double>> loudSpans(const AudioPeaks &);
// The layers for all the lines, the first line's layers first. Every line is
// named "Lyric 07 ..." so they read in order in the timeline.
std::vector<Layer> lyricMotionLayers(const std::vector<LyricCue> &, const LyricMotionOptions &);
// Times kept apart from the words: one per line, seconds or [mm:ss.xx], "#"
// lines ignored. The words are the user's; the times say when each is sung.
std::vector<double> parseLyricTimes(const QString &content);
// Gives each line its time, in order; a time beyond the last line is where the
// last line ends. False when there are fewer times than lines.
bool applyLyricTimes(std::vector<LyricCue> &, const std::vector<double> &times);
// Writes cues as an .lrc.
QString lyricCuesToLrc(const std::vector<LyricCue> &);
