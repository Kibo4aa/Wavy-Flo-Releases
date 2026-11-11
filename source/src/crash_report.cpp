#include "crash_report.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace {
QByteArray reportPath;
QByteArray logPath;
bool alertsAllowed = true;
LONG reporting = 0;
DWORD reportingThread = 0;

// One report per run. Rendering runs on a pool of threads, so bad data faults
// several of them within microseconds of each other: the loser used to return
// straight into process teardown and cut the winner's report off mid-write,
// which is how a crash left a file of zero bytes behind. It waits instead --
// whoever owns the report ends the process when it is done.
bool claimReport() {
  const DWORD self = GetCurrentThreadId();
  if (InterlockedCompareExchange(&reporting, 1, 0) == 0) {
    reportingThread = self;
    return true;
  }
  // Unless the handler itself is what faulted. Dying beats hanging.
  if (reportingThread == self) return false;
  Sleep(INFINITE);
  return false;
}

// The last messages before the fault. A fixed ring, so recording one allocates
// nothing and still reads back when the heap is the thing that broke.
constexpr int TrailSlots = 32;
constexpr int TrailWidth = 400;
char trail[TrailSlots][TrailWidth];
volatile LONG trailWritten = 0;

void recordTrail(const char *text) {
  const LONG index = InterlockedIncrement(&trailWritten) - 1;
  strncpy_s(trail[index % TrailSlots], TrailWidth, text, _TRUNCATE);
}

// Everything below runs after memory may already be corrupt, so it allocates
// nothing it does not have to and never touches Qt's containers.
void appendLine(HANDLE file, const char *text) {
  DWORD written = 0;
  WriteFile(file, text, DWORD(strlen(text)), &written, nullptr);
  WriteFile(file, "\r\n", 2, &written, nullptr);
}

// Where an address lives: which module, how far into it, and -- where a .pdb
// sits beside the executable -- the function and the source line. The module
// and offset alone still match a report against the Windows event log, so the
// line degrades to something useful rather than nothing.
void describeAddress(HANDLE process, DWORD64 address, char *out, size_t size) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "";
  const char *leaf = "?";
  DWORD64 offset = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      GetModuleFileNameA(module, name, MAX_PATH)) {
    leaf = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    offset = address - DWORD64(module);
  }
  char symbol[sizeof(SYMBOL_INFO) + 256] = {};
  auto *info = reinterpret_cast<SYMBOL_INFO *>(symbol);
  info->SizeOfStruct = sizeof(SYMBOL_INFO);
  info->MaxNameLen = 255;
  DWORD64 displacement = 0;
  const bool named = SymFromAddr(process, address, &displacement, info) && info->NameLen;

  IMAGEHLP_LINE64 source = {};
  source.SizeOfStruct = sizeof(source);
  DWORD sourceOffset = 0;
  const bool located = SymGetLineFromAddr64(process, address, &sourceOffset, &source) &&
                       source.FileName;
  const char *sourceLeaf = nullptr;
  if (located)
    sourceLeaf = strrchr(source.FileName, '\\') ? strrchr(source.FileName, '\\') + 1
                                                : source.FileName;

  if (named && located)
    _snprintf_s(out, size, _TRUNCATE, "  %s+0x%llx  %s  (%s:%lu)", leaf, offset, info->Name,
                sourceLeaf, source.LineNumber);
  else if (named)
    _snprintf_s(out, size, _TRUNCATE, "  %s+0x%llx  %s+0x%llx", leaf, offset, info->Name,
                displacement);
  else
    _snprintf_s(out, size, _TRUNCATE, "  %s+0x%llx", leaf, offset);
}

// Walking from the faulting thread's own context names the code that failed.
// Capturing the current backtrace instead would name the handler.
void writeStack(HANDLE file, HANDLE process, CONTEXT *context) {
  char line[1024];
#ifdef _M_X64
  if (context) {
    CONTEXT walking = *context;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = walking.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = walking.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = walking.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 62; ++depth) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &walking,
                       nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
          !frame.AddrPC.Offset)
        break;
      describeAddress(process, frame.AddrPC.Offset, line, sizeof(line));
      appendLine(file, line);
    }
    return;
  }
#endif
  void *frames[62] = {};
  const USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    describeAddress(process, DWORD64(frames[i]), line, sizeof(line));
    appendLine(file, line);
  }
}

void writeReport(const char *cause, const char *detail, EXCEPTION_POINTERS *exception) {
  const HANDLE file = CreateFileA(reportPath.constData(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  char line[1024];
  SYSTEMTIME now;
  GetLocalTime(&now);
  _snprintf_s(line, _TRUNCATE, "Wavy-Flo closed unexpectedly at %04d-%02d-%02d %02d:%02d:%02d",
              now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  appendLine(file, line);
  _snprintf_s(line, _TRUNCATE, "Cause: %s", cause);
  appendLine(file, line);
  if (detail && *detail) {
    _snprintf_s(line, _TRUNCATE, "Detail: %s", detail);
    appendLine(file, line);
  }

  const auto *record = exception ? exception->ExceptionRecord : nullptr;
  if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    _snprintf_s(line, _TRUNCATE, "Tried to %s address 0x%llx",
                record->ExceptionInformation[0] ? "write" : "read",
                quint64(record->ExceptionInformation[1]));
    appendLine(file, line);
  }

  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);
  if (record) {
    appendLine(file, "");
    appendLine(file, "Faulting instruction");
    describeAddress(process, DWORD64(record->ExceptionAddress), line, sizeof(line));
    appendLine(file, line);
  }

  appendLine(file, "");
  appendLine(file, "What the program reported just before");
  const LONG written = trailWritten;
  if (written == 0) {
    appendLine(file, "  nothing was logged");
  } else {
    for (LONG i = written > TrailSlots ? written - TrailSlots : 0; i < written; ++i) {
      _snprintf_s(line, _TRUNCATE, "  %s", trail[i % TrailSlots]);
      appendLine(file, line);
    }
  }

  appendLine(file, "");
  appendLine(file, "Call stack, innermost first");
  writeStack(file, process, exception ? exception->ContextRecord : nullptr);

  appendLine(file, "");
  _snprintf_s(line, _TRUNCATE, "The full message log is at %s", logPath.constData());
  appendLine(file, line);
  appendLine(file, "Please send both files. They name the code that failed.");
  CloseHandle(file);
}

char alertText[2048];

DWORD WINAPI showAlert(LPVOID) {
  MessageBoxA(nullptr, alertText, "Wavy-Flo closed unexpectedly",
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
  return 0;
}

// Saying so on screen is the whole point: a window that just vanishes leaves
// the person in front of it with nothing to act on. The dialog runs on a fresh
// thread because a stack overflow leaves the faulting one no room to open one.
void alert(const char *cause, const char *detail) {
  if (!alertsAllowed) return;
  _snprintf_s(alertText, _TRUNCATE,
              "Wavy-Flo had to close.\r\n\r\nWhat happened:\r\n%s\r\n%s\r\n\r\n"
              "A report naming the code that failed was saved to:\r\n%s\r\n\r\n"
              "Please send that file.",
              cause, detail && *detail ? detail : "", reportPath.constData());
  const HANDLE thread = CreateThread(nullptr, 0, showAlert, nullptr, 0, nullptr);
  if (!thread) return;
  WaitForSingleObject(thread, 120000);
  CloseHandle(thread);
}

LONG WINAPI onException(EXCEPTION_POINTERS *exception) {
  if (!claimReport()) return EXCEPTION_EXECUTE_HANDLER;
  const auto *record = exception ? exception->ExceptionRecord : nullptr;
  const DWORD code = record ? record->ExceptionCode : 0;
  const char *cause =
      code == EXCEPTION_ACCESS_VIOLATION ? "the program read or wrote memory it does not own"
      // An unhandled C++ throw leaves the process as this vendor code, and the
      // frame under the two runtime ones below is where it was thrown.
      : code == 0xE06D7363 ? "a C++ error was thrown and no part of the program caught it"
      : code == EXCEPTION_STACK_OVERFLOW ? "the program ran out of stack, usually endless recursion"
      : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "the program divided a whole number by zero"
      : code == EXCEPTION_ILLEGAL_INSTRUCTION
          ? "the processor was handed an instruction it does not have"
          : "the processor hit a fault it could not continue past";
  char detail[128];
  _snprintf_s(detail, _TRUNCATE, "Exception code 0x%08lx", code);
  writeReport(cause, detail, exception);
  alert(cause, detail);
  return EXCEPTION_EXECUTE_HANDLER;
}

// The paths below never reach the exception filter. Each one ends the process
// on its own, which is exactly the silent close there was no note for.
void finish(const char *cause, const char *detail) {
  if (claimReport()) {
    writeReport(cause, detail, nullptr);
    alert(cause, detail);
  }
  TerminateProcess(GetCurrentProcess(), 3);
}

void onTerminate() {
  char detail[512] = "";
  if (std::current_exception()) {
    try {
      std::rethrow_exception(std::current_exception());
    } catch (const std::exception &error) {
      _snprintf_s(detail, _TRUNCATE, "Unhandled C++ exception: %s", error.what());
    } catch (...) {
      strncpy_s(detail, "Unhandled C++ exception of an unrecognised type", _TRUNCATE);
    }
  }
  finish("the program hit an error no part of it was written to recover from", detail);
}

void onAbort(int) { finish("the program asked to stop immediately (abort)", ""); }

void onInvalidParameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file,
                        unsigned line, uintptr_t) {
  char detail[512];
  _snprintf_s(detail, _TRUNCATE, "%ls in %ls, %ls line %u",
              expression && *expression ? expression : L"An argument was rejected",
              function && *function ? function : L"a system function",
              file && *file ? file : L"unknown file", line);
  finish("a system call was given a value it refuses to accept", detail);
}

void onPureCall() {
  finish("an object was called while it was still being built or already destroyed", "");
}

const char *severityOf(QtMsgType type) {
  switch (type) {
    case QtDebugMsg: return "debug   ";
    case QtInfoMsg: return "info    ";
    case QtWarningMsg: return "warning ";
    case QtCriticalMsg: return "CRITICAL";
    case QtFatalMsg: return "FATAL   ";
  }
  return "        ";
}

// A window-subsystem build has no console, so everything Qt says about itself
// -- and it usually says something right before it gives up -- went nowhere.
void appendToLog(const QByteArray &text) {
  static QMutex mutex;
  QMutexLocker locked(&mutex);
  static QFile file(QString::fromLocal8Bit(logPath));
  static QByteArray previous;
  static qint64 repeats = 0;
  static qint64 written = 0;
  constexpr qint64 limit = 4 * 1024 * 1024;
  if (written > limit) return;
  if (!file.isOpen() && !file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
  // Painting warns from inside the frame loop, so the same line can arrive
  // sixty times a second. Counting a repeat costs nothing; writing one would
  // cost a disk write per frame and bury every other line in the log.
  if (text == previous) {
    ++repeats;
    return;
  }
  if (repeats) {
    written += file.write(repeats == 1 ? QByteArray("             the line above repeated once\r\n")
                                       : "             the line above repeated " +
                                             QByteArray::number(repeats) + " times\r\n");
    repeats = 0;
  }
  previous = text;
  written += file.write(QDateTime::currentDateTime().toString("hh:mm:ss.zzz ").toLocal8Bit() +
                        text + "\r\n");
  if (written > limit) file.write("log stopped here: it reached its size limit\r\n");
  // Every line, not just the alarming ones: the buffer these sit in belongs to
  // this process, and a force close is exactly when it does not get emptied.
  // The repeat counting above is what keeps the cost of that off the frame loop.
  file.flush();
}

void logMessage(QtMsgType type, const QMessageLogContext &context, const QString &text) {
  QByteArray line = QByteArray(severityOf(type)) + " " + text.toLocal8Bit();
  if (context.file && context.line)
    line += " [" + QByteArray(context.file) + ":" + QByteArray::number(context.line) + "]";
  recordTrail(line.constData());
  // A console run -- the self-tests -- still wants these where it always saw them.
  fprintf(stderr, "%s\n", line.constData());
  if (type != QtDebugMsg && type != QtInfoMsg) appendToLog(line);
  if (type == QtFatalMsg) finish("Qt reported an error it treats as fatal", text.toLocal8Bit().constData());
}
}  // namespace

void installCrashReporter() {
  const QString directory = QCoreApplication::applicationDirPath();
  reportPath = QDir::toNativeSeparators(directory + "/crash-report.txt").toLocal8Bit();
  logPath = QDir::toNativeSeparators(directory + "/wavy-flo-log.txt").toLocal8Bit();
  // A modal dialog would hang a run with nobody watching it, and those runs
  // read their answer out of a file anyway.
  for (const QString &argument : QCoreApplication::arguments())
    if (argument.contains("self-test") || argument.contains("benchmark") ||
        argument.contains("screenshot") || argument == "--crash-report")
      alertsAllowed = false;

  // Keep the previous run's log: a crash is often explained by what the run
  // before it was already complaining about.
  const QString log = directory + "/wavy-flo-log.txt";
  const QString previous = directory + "/wavy-flo-log-previous.txt";
  if (QFile::exists(log)) {
    QFile::remove(previous);
    QFile::rename(log, previous);
  }

  SetUnhandledExceptionFilter(onException);
  // A fault the CRT handles itself would skip the filter above, so refuse that too.
  SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
  std::set_terminate(onTerminate);
  signal(SIGABRT, onAbort);
  _set_invalid_parameter_handler(onInvalidParameter);
  _set_purecall_handler(onPureCall);
  qInstallMessageHandler(logMessage);
}

#else
void installCrashReporter() {}
#endif

QString takePreviousCrashReport() {
  QFile file(QCoreApplication::applicationDirPath() + "/crash-report.txt");
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) return {};
  const auto text = QString::fromLocal8Bit(file.readAll());
  file.close();
  file.remove();
  return text;
}
