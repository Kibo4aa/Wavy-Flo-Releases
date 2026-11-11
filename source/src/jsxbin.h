#pragma once
// JSXBIN: the form Adobe's ExtendScript tools save a script in ("Export as
// Binary", .jsxbin, or an eval("@JSXBIN@ES@2.0@...") inside a .jsx). It is
// the script's syntax tree written out as text. Wavy-Flo reads it back into
// JavaScript that does what the script does and runs that: in memory, never
// shown or saved, the way After Effects runs it.
//
// The format (versions 1.0, 2.0 and 2.1) was learnt from the open-source
// decompiler jsxer (github.com/AngeloD2022/jsxer), which documents it; this
// reader is written on its own, and nothing of Adobe's is used.
#include <QString>
#include <vector>

struct JsxbinScript {
  QString code;           // JavaScript that does what the compiled script does
  std::vector<int> lines; // for each line of `code`, the line of the original script it comes from (0: none)
  QString error;          // why it could not be read
  bool ok() const { return error.isEmpty(); }
};

// Whether `text` is a compiled script (spaces and a byte-order mark before it allowed).
bool isJsxbin(const QString &text);
JsxbinScript decodeJsxbin(const QString &text);
// A plain script's eval("@JSXBIN@...") calls, each made an eval of the script
// it holds. The lines after one stay where they were.
QString inlineJsxbinEvals(const QString &text);
