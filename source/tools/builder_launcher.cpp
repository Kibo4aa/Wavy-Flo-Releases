#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <string>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t path[32768];
  DWORD size = GetModuleFileNameW(nullptr, path, 32768);
  if (!size || size >= 32768) return 1;
  std::wstring root(path, size);
  root = root.substr(0, root.find_last_of(L"\\/"));
  std::wstring project = root + L"\\Wavy-Flo";
  std::wstring app = project + L"\\Wavy-Flo Builder.exe";
  auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, project.c_str(), SW_SHOWNORMAL));
  if (result <= 32) {
    MessageBoxW(nullptr, L"Cannot open Wavy-Flo Builder. Keep this application beside the Wavy-Flo folder.", L"Wavy-Flo Builder", MB_OK | MB_ICONERROR);
    return 1;
  }
  return 0;
}
