#pragma once
#include <QString>

// Turns a force close into something anyone can act on.
//
// When the process dies there is nothing left to ask: the window is gone, the
// log ends mid-sentence, and all that reaches the developer is "it closed by
// itself". This says so on screen while it happens, and writes the cause, the
// faulting address, the last messages before it and the call stack -- each
// frame resolved to a module, an offset, and a function and source line where
// a .pdb sits beside the executable -- into crash-report.txt.
//
// It covers the ways a process dies silently, not only the processor faults an
// exception filter sees: an unhandled C++ exception, abort(), a rejected
// argument to a system call, a pure virtual call, and qFatal(). Everything Qt
// reports also goes to wavy-flo-log.txt, which a window-subsystem build
// otherwise throws away for want of a console.
//
// Installed once, before anything else runs. Does nothing on platforms where
// there is no such hook.
void installCrashReporter();
// The report left by a previous run, if that run ended in one. Empty otherwise.
// Reading it also clears it, so the same crash is reported once.
QString takePreviousCrashReport();
