#include "ofx_internal.h"
#include "ofxImageEffect.h"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#endif

// ---------------------------------------------------------------------------
// OpenGL for OpenFX plug-ins that draw on the card (ofxGPURender.h). One
// context of its own on one thread of its own: every render on the card goes
// through it in turn -- the card draws one thing at a time anyway -- and a
// plug-in's GL calls always find the context current where they run. Plain
// WGL rather than Qt's: the plug-in host process has no GUI thread to make a
// Qt context on. Started the first time a plug-in asks for the card.
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {
constexpr GLenum kRgba8 = 0x8058, kRgba16 = 0x805B, kRgba32f = 0x8814;
constexpr GLenum kClampToEdge = 0x812F, kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;
constexpr GLenum kUnpackRowLength = 0x0CF2, kPackRowLength = 0x0D02;

using GenFramebuffers = void(APIENTRY *)(GLsizei, GLuint *);
using DeleteFramebuffers = void(APIENTRY *)(GLsizei, const GLuint *);
using BindFramebuffer = void(APIENTRY *)(GLenum, GLuint);
using FramebufferTexture2D = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using CheckFramebufferStatus = GLenum(APIENTRY *)(GLenum);

struct Card {
  std::mutex guard;
  std::condition_variable wake;
  std::deque<std::function<void()>> work;
  bool tried = false, ready = false;
  std::thread::id thread;
  HWND window = nullptr;
  HDC dc = nullptr;
  HGLRC context = nullptr;
  GenFramebuffers genFramebuffers = nullptr;
  DeleteFramebuffers deleteFramebuffers = nullptr;
  BindFramebuffer bindFramebuffer = nullptr;
  FramebufferTexture2D framebufferTexture2D = nullptr;
  CheckFramebufferStatus checkFramebufferStatus = nullptr;
};
// Never destroyed: the thread outlives static destruction.
Card &card() {
  static Card *c = new Card;
  return *c;
}

// The texture's layout for a depth: internal format, format, type and the
// bytes a pixel takes.
struct Layout {
  GLint internal;
  GLenum type;
  int bytes;
};
Layout layoutFor(const std::string &depth) {
  if (depth == kOfxBitDepthFloat) return {GLint(kRgba32f), GL_FLOAT, 16};
  if (depth == kOfxBitDepthShort) return {GLint(kRgba16), GL_UNSIGNED_SHORT, 8};
  return {GLint(kRgba8), GL_UNSIGNED_BYTE, 4};
}

template <class F> bool load(F &into, const char *name) {
  into = reinterpret_cast<F>(wglGetProcAddress(name));
  return into != nullptr;
}

void cardThread(Card &c, std::promise<bool> *started) {
  bool ok = false;
  WNDCLASSW kind{};
  kind.lpfnWndProc = DefWindowProcW;
  kind.hInstance = GetModuleHandleW(nullptr);
  kind.lpszClassName = L"WavyFloOfxCard";
  RegisterClassW(&kind);
  c.window = CreateWindowExW(0, kind.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                             kind.hInstance, nullptr);
  if (c.window) c.dc = GetDC(c.window);
  if (c.dc) {
    PIXELFORMATDESCRIPTOR want{};
    want.nSize = sizeof want;
    want.nVersion = 1;
    want.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    want.iPixelType = PFD_TYPE_RGBA;
    want.cColorBits = 32;
    want.cAlphaBits = 8;
    want.cDepthBits = 24;
    want.cStencilBits = 8;
    const int format = ChoosePixelFormat(c.dc, &want);
    if (format && SetPixelFormat(c.dc, format, &want)) c.context = wglCreateContext(c.dc);
    if (c.context && wglMakeCurrent(c.dc, c.context))
      ok = load(c.genFramebuffers, "glGenFramebuffers") && load(c.deleteFramebuffers, "glDeleteFramebuffers") &&
           load(c.bindFramebuffer, "glBindFramebuffer") && load(c.framebufferTexture2D, "glFramebufferTexture2D") &&
           load(c.checkFramebufferStatus, "glCheckFramebufferStatus");
  }
  c.thread = std::this_thread::get_id();
  started->set_value(ok);
  if (!ok) return;
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(c.guard);
      c.wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return !c.work.empty(); });
      if (!c.work.empty()) {
        task = std::move(c.work.front());
        c.work.pop_front();
      }
    }
    // The hidden window's messages, so it never looks hung.
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    if (task) task();
  }
}
} // namespace

bool ofx_gl::run(const std::function<void()> &task) {
  Card &c = card();
  {
    std::unique_lock<std::mutex> lock(c.guard);
    if (!c.tried) {
      c.tried = true;
      std::promise<bool> started;
      auto ready = started.get_future();
      std::thread(cardThread, std::ref(c), &started).detach();
      lock.unlock();
      c.ready = ready.get();
      lock.lock();
    }
    if (!c.ready) return false;
  }
  // Already on it -- a render on the card that draws an earlier effect on the
  // same layer -- it is simply done here.
  if (std::this_thread::get_id() == c.thread) {
    task();
    return true;
  }
  std::promise<void> done;
  auto finished = done.get_future();
  {
    std::lock_guard<std::mutex> lock(c.guard);
    c.work.push_back([&] {
      task();
      done.set_value();
    });
  }
  c.wake.notify_one();
  finished.wait();
  return true;
}

unsigned ofx_gl::upload(const void *bits, int width, int height, int rowBytes, const std::string &depth) {
  const Layout layout = layoutFor(depth);
  if (!bits || width <= 0 || height <= 0 || rowBytes % layout.bytes) return 0;
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(kUnpackRowLength, rowBytes / layout.bytes);
  glTexImage2D(GL_TEXTURE_2D, 0, layout.internal, width, height, 0, GL_RGBA, layout.type, bits);
  glPixelStorei(kUnpackRowLength, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &texture);
    return 0;
  }
  return texture;
}

void ofx_gl::drop(unsigned texture) {
  const GLuint id = texture;
  if (id) glDeleteTextures(1, &id);
}

ofx_gl::Target ofx_gl::target(int width, int height, const std::string &depth) {
  Card &c = card();
  Target t;
  const Layout layout = layoutFor(depth);
  GLuint texture = 0, framebuffer = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, layout.internal, width, height, 0, GL_RGBA, layout.type, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  c.genFramebuffers(1, &framebuffer);
  c.bindFramebuffer(kFramebuffer, framebuffer);
  c.framebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, texture, 0);
  if (c.checkFramebufferStatus(kFramebuffer) != kFramebufferComplete) {
    c.bindFramebuffer(kFramebuffer, 0);
    c.deleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    return t;
  }
  glViewport(0, 0, width, height);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  t.framebuffer = framebuffer;
  t.texture = texture;
  t.width = width;
  t.height = height;
  t.depth = depth;
  return t;
}

bool ofx_gl::read(const Target &t, void *bits, int rowBytes) {
  Card &c = card();
  const Layout layout = layoutFor(t.depth);
  if (!t || !bits || rowBytes % layout.bytes) return false;
  glFinish();
  c.bindFramebuffer(kFramebuffer, t.framebuffer);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(kPackRowLength, rowBytes / layout.bytes);
  glReadPixels(0, 0, t.width, t.height, GL_RGBA, layout.type, bits);
  glPixelStorei(kPackRowLength, 0);
  return glGetError() == GL_NO_ERROR;
}

void ofx_gl::release(Target &t) {
  Card &c = card();
  if (t.framebuffer) {
    const GLuint framebuffer = t.framebuffer;
    c.bindFramebuffer(kFramebuffer, 0);
    c.deleteFramebuffers(1, &framebuffer);
  }
  drop(t.texture);
  t = Target{};
}
#else
bool ofx_gl::run(const std::function<void()> &) { return false; }
unsigned ofx_gl::upload(const void *, int, int, int, const std::string &) { return 0; }
void ofx_gl::drop(unsigned) {}
ofx_gl::Target ofx_gl::target(int, int, const std::string &) { return {}; }
bool ofx_gl::read(const Target &, void *, int) { return false; }
void ofx_gl::release(Target &) {}
#endif
