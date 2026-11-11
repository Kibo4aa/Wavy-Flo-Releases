#pragma once
#include "engine.h"
#include <functional>
#include <memory>

// All GL work runs on the thread that first draws with it (the thread its
// context belongs to); GpuRenderService gives it a thread of its own.
// A null result means the caller must use the CPU renderer.
struct GpuResidentCacheStats {
  quint64 hits = 0, builds = 0;
  qint64 bytes = 0;
  quint64 particleLayers = 0; // particle layers the card has written itself
  quint64 imageUploads = 0, imageDraws = 0; // pictures sent to the card, and drawn from what it keeps
  quint64 litFaces = 0; // lit 3D faces painted and lit on the card
  quint64 blendedLayers = 0; // layers with a blend mode flattened and blended on the card
  quint64 mattedLayers = 0; // layers shaped by a track matte on the card
  quint64 effectLayers = 0, effectLayerReadbacks = 0; // Effect Layers worked here, and of those, on the processor in between
  quint64 blurredLayers = 0; // layers blurring themselves by their own motion, their samples averaged on the card
  quint64 shapeFills = 0; // shape fills the card covered by exact area, as the CPU's rasterizer does
  quint64 shapeFillsAddedUp = 0; // of those, huge ones whose area was added up in float targets, not summed in the fill
};
class GpuRenderer {
public:
  GpuRenderer();
  ~GpuRenderer();
  // region is in output pixels; empty renders the whole frame. Only the
  // region is allocated and read back, so zooming in does not cost a
  // magnified composition's worth of pixels.
  QImage render(const Scene &, double frame, QSize, QRect region = {});
  // Why the card would hand this frame back to the CPU, or empty when it takes
  // it. Asked before a frame is drawn, so a frame the card will not take goes
  // straight to the CPU's background workers instead of being drawn on the
  // interface thread after the card has said no.
  QString declineReason(const Scene &, double frame);
  // The same, without a card: `blendModesExact` is asked only when the frame
  // blends layers. For a caller on another thread than the card's.
  static QString declineReason(const Scene &, double frame, const std::function<bool()> &blendModesExact,
                               bool askAboutBlends = true);
  QString description() const;
  GpuResidentCacheStats residentCacheStats() const;
  // Whether this driver blends layers exactly as the CPU does. Qt draws a
  // blend mode correctly only when the driver offers the advanced blend
  // extension, and otherwise draws it as Normal without saying so, so it is
  // checked once against the CPU rather than assumed.
  bool blendModesExact();
private:
  friend class GpuRenderService; // lends its renderer the window it draws with
  struct State;
  std::unique_ptr<State> state;
};
// A card of its own on a thread of its own: frames are drawn there, so the
// thread asking for one -- the interface, a preview job -- waits only if it
// wants the picture now, and the interface never draws on the card itself.
// Drawn on the interface thread, every GPU frame held the editor up for as
// long as it took, where the CPU's frames are drawn in the background.
class GpuRenderService {
public:
  GpuRenderService();
  ~GpuRenderService();
  // Drawn on the service's thread; the caller waits for it. Null when the
  // card did not draw it, and the caller draws it on the CPU. `drawnMs` is
  // what drawing it took there, not counting any wait for the frames asked
  // for before it.
  QImage render(const Scene &, double frame, QSize, QRect region = {}, double *drawnMs = nullptr);
  // Answered here and now, without waiting for the card: a frame that blends
  // layers is declined until the card has been checked against the CPU, which
  // it is, once, as soon as the service starts.
  QString declineReason(const Scene &, double frame);
  QString description() const;
  GpuResidentCacheStats residentCacheStats() const;
  // Whether the card could be used at all, once it has been tried.
  bool usable();
private:
  struct Worker;
  std::unique_ptr<Worker> worker;
};
// A renderer draws with a hidden window that only the interface thread may
// make. One drawing on another thread -- an export frame's -- takes one made
// ahead; called on the interface thread before such renderers start, so their
// first frames are on the card too.
void reserveGpuSurfaces(int count);
int checkGpuRendering(QStringList &report);
int checkGpuSurfaces(QStringList &report);
