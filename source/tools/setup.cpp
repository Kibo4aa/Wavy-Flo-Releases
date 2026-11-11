// Wavy-Flo Setup: the installer, one .exe with nothing to unpack first and no
// DLL it needs -- plain Win32 and the C++ runtime linked in, so it runs on a
// machine that has nothing of Wavy-Flo yet.
//
// The program it installs rides at the end of this same file: every file of
// the program folder, compressed with Windows' own LZMS, then a footer saying
// where they start. `Wavy-Flo Setup.exe --pack <folder> <out.exe> <version>`
// makes such a file from the bare installer.
//
// Installed, Wavy-Flo is
//   * in a folder of the person's own (%LOCALAPPDATA%\Programs\Wavy-Flo by default,
//     so neither installing nor updating ever asks for an administrator);
//   * in the Start menu -- which is what makes it come up in Windows search --
//     and, if wanted, on the desktop;
//   * in Settings > Apps, with an uninstaller;
//   * what opens .wavyflo projects, if wanted.
//
// Run as `--update --dir <folder> --wait-pid <pid>` (what Wavy-Flo's updater
// does) it waits for Wavy-Flo to close, puts the new files over the old ones
// with a small progress window, and opens Wavy-Flo again. `--silent` does the
// same with no window at all. `--uninstall` takes away exactly what was
// installed: the files listed when they were put there, never anything else in
// the folder.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {
const wchar_t *kAppName = L"Wavy-Flo";
const wchar_t *kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WavyFlo";
const wchar_t *kUninstaller = L"Uninstall Wavy-Flo.exe";
const wchar_t *kManifest = L"installed-files.txt";
const char kMagic[8] = {'W', 'A', 'V', 'Y', 'S', 'E', 'T', 'P'};

#pragma pack(push, 1)
struct Footer {
  uint64_t payloadOffset = 0; // where the files start in this exe
  uint64_t totalRaw = 0;      // their size once unpacked
  uint32_t fileCount = 0;
  char version[32] = {};
  char magic[8] = {};
};
#pragma pack(pop)

struct Entry {
  std::wstring path; // relative, with backslashes
  uint64_t raw = 0, packed = 0, offset = 0; // offset of the packed bytes in the exe
};

std::wstring widen(const std::string &s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
std::string narrow(const std::wstring &w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring selfPath() {
  std::wstring path(32768, L'\0');
  path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
  return path;
}
std::wstring join(std::wstring a, const std::wstring &b) {
  if (!a.empty() && a.back() != L'\\') a += L'\\';
  return a + b;
}
std::wstring parentOf(const std::wstring &path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}
bool makeFolders(const std::wstring &folder) {
  const int r = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
  return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}
bool readFile(const std::wstring &path, std::vector<uint8_t> &out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  out.resize(size_t(in.tellg()));
  in.seekg(0);
  return bool(in.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size()))) || out.empty();
}
std::wstring knownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::wstring path;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) path = raw;
  CoTaskMemFree(raw);
  return path;
}
std::wstring regString(HKEY root, const wchar_t *key, const wchar_t *name) {
  wchar_t buffer[2048];
  DWORD size = sizeof(buffer);
  if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
  return buffer;
}
void regSet(HKEY root, const std::wstring &key, const wchar_t *name, const std::wstring &value) {
  HKEY h;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
  RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(h);
}
void regSetDword(HKEY root, const std::wstring &key, const wchar_t *name, DWORD value) {
  HKEY h;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
  RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
  RegCloseKey(h);
}

// ---- A digital signature -------------------------------------------------------------------
// A signed exe carries its signature at its very end, recorded in its PE
// header's security directory. The files Setup carries end just before it.
struct PeSecurity { uint64_t entryAt = 0; uint32_t offset = 0, size = 0; };
PeSecurity peSecurity(const std::vector<uint8_t> &bytes) {
  PeSecurity found;
  auto u16 = [&](size_t at) { return at + 2 <= bytes.size() ? uint32_t(bytes[at] | bytes[at + 1] << 8) : 0u; };
  auto u32 = [&](size_t at) {
    return at + 4 <= bytes.size() ? uint32_t(bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | uint32_t(bytes[at + 3]) << 24) : 0u;
  };
  if (u16(0) != 0x5A4D) return found;                     // "MZ"
  const size_t pe = u32(0x3C);
  if (u32(pe) != 0x00004550) return found;                // "PE\0\0"
  const size_t optional = pe + 24;
  const uint32_t magic = u16(optional);
  const size_t directories = optional + (magic == 0x20B ? 112 : 96); // PE32+ or PE32
  found.entryAt = directories + 4 * 8;                    // IMAGE_DIRECTORY_ENTRY_SECURITY
  found.offset = u32(size_t(found.entryAt));
  found.size = u32(size_t(found.entryAt) + 4);
  return found;
}
// Where an exe's own bytes end: before its signature, when it has one.
uint64_t contentEnd(const std::wstring &exe) {
  std::ifstream in(exe, std::ios::binary | std::ios::ate);
  if (!in) return 0;
  const auto size = uint64_t(in.tellg());
  std::vector<uint8_t> head(size_t(std::min<uint64_t>(size, 4096)));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(head.data()), std::streamsize(head.size()));
  const PeSecurity security = peSecurity(head);
  if (security.offset && security.size && uint64_t(security.offset) + security.size == size) return security.offset;
  return size;
}
// The bare exe cut from a signed one has no signature of its own: the
// header must not point at one that is no longer there.
void dropSignature(std::vector<uint8_t> &bytes) {
  const PeSecurity security = peSecurity(bytes);
  if (security.entryAt && security.entryAt + 8 <= bytes.size())
    std::fill(bytes.begin() + ptrdiff_t(security.entryAt), bytes.begin() + ptrdiff_t(security.entryAt + 8), uint8_t(0));
}

// ---- The payload ----------------------------------------------------------------------
struct Payload {
  Footer footer;
  std::vector<Entry> entries;
  std::wstring version;
  bool read(const std::wstring &exe) {
    std::ifstream in(exe, std::ios::binary);
    if (!in) return false;
    // The footer ends where the exe's own bytes do: before a signature.
    // A signature starts on an eight-byte boundary: up to seven bytes of
    // padding may lie between the footer and it.
    const uint64_t end = contentEnd(exe);
    uint64_t size = 0;
    for (uint64_t pad = 0; pad < 8 && end >= sizeof(Footer) + pad; ++pad) {
      in.clear();
      in.seekg(std::streamoff(end - pad - sizeof(Footer)));
      in.read(reinterpret_cast<char *>(&footer), sizeof(footer));
      if (in && std::memcmp(footer.magic, kMagic, 8) == 0) { size = end - pad; break; }
    }
    if (!size || footer.payloadOffset >= size) return false;
    version = widen(std::string(footer.version, strnlen(footer.version, sizeof(footer.version))));
    in.seekg(std::streamoff(footer.payloadOffset));
    for (uint32_t i = 0; i < footer.fileCount; ++i) {
      uint32_t length = 0;
      Entry e;
      in.read(reinterpret_cast<char *>(&length), 4);
      if (!in || length > 4096) return false;
      std::string path(length, '\0');
      in.read(path.data(), length);
      in.read(reinterpret_cast<char *>(&e.raw), 8);
      in.read(reinterpret_cast<char *>(&e.packed), 8);
      if (!in) return false;
      e.path = widen(path);
      // Nothing that would land outside the folder it is unpacked into.
      if (e.path.find(L"..") != std::wstring::npos || e.path.find(L':') != std::wstring::npos ||
          (!e.path.empty() && (e.path[0] == L'\\' || e.path[0] == L'/')))
        return false;
      e.offset = uint64_t(in.tellg());
      in.seekg(std::streamoff(e.packed), std::ios::cur);
      entries.push_back(e);
    }
    return bool(in);
  }
};

// Every file under `folder`, relative to it.
void listFiles(const std::wstring &folder, const std::wstring &relative, std::vector<std::wstring> &out) {
  WIN32_FIND_DATAW found;
  HANDLE h = FindFirstFileW(join(join(folder, relative), L"*").c_str(), &found);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring name = found.cFileName;
    if (name == L"." || name == L"..") continue;
    const std::wstring rel = relative.empty() ? name : join(relative, name);
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) listFiles(folder, rel, out);
    else out.push_back(rel);
  } while (FindNextFileW(h, &found));
  FindClose(h);
}

int pack(const std::wstring &folder, const std::wstring &out, const std::string &version) {
  std::vector<uint8_t> self;
  // Only the bare installer: an installer that already carries files is cut back to itself.
  if (!readFile(selfPath(), self)) return 2;
  Payload carried;
  if (carried.read(selfPath())) self.resize(size_t(carried.footer.payloadOffset));
  else self.resize(size_t(contentEnd(selfPath())));
  dropSignature(self); // signed again, if at all, once packed
  std::vector<std::wstring> files;
  listFiles(folder, L"", files);
  std::sort(files.begin(), files.end());
  if (files.empty()) { fwprintf(stderr, L"Nothing to pack in %ls\n", folder.c_str()); return 3; }
  std::ofstream o(out, std::ios::binary | std::ios::trunc);
  if (!o) return 4;
  o.write(reinterpret_cast<const char *>(self.data()), std::streamsize(self.size()));
  Footer footer;
  footer.payloadOffset = self.size();
  COMPRESSOR_HANDLE compressor = nullptr;
  if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) return 5;
  for (const auto &rel : files) {
    std::vector<uint8_t> raw;
    if (!readFile(join(folder, rel), raw)) { fwprintf(stderr, L"Cannot read %ls\n", rel.c_str()); return 6; }
    std::vector<uint8_t> packed;
    if (!raw.empty()) {
      SIZE_T needed = 0;
      Compress(compressor, raw.data(), raw.size(), nullptr, 0, &needed);
      packed.resize(needed);
      SIZE_T written = 0;
      if (!Compress(compressor, raw.data(), raw.size(), packed.data(), packed.size(), &written)) return 7;
      packed.resize(written);
    }
    const std::string path = narrow(rel);
    const uint32_t length = uint32_t(path.size());
    const uint64_t rawSize = raw.size(), packedSize = packed.size();
    o.write(reinterpret_cast<const char *>(&length), 4);
    o.write(path.data(), length);
    o.write(reinterpret_cast<const char *>(&rawSize), 8);
    o.write(reinterpret_cast<const char *>(&packedSize), 8);
    o.write(reinterpret_cast<const char *>(packed.data()), std::streamsize(packed.size()));
    footer.totalRaw += rawSize;
    ++footer.fileCount;
  }
  CloseCompressor(compressor);
  strncpy_s(footer.version, version.c_str(), sizeof(footer.version) - 1);
  std::memcpy(footer.magic, kMagic, 8);
  o.write(reinterpret_cast<const char *>(&footer), sizeof(footer));
  if (!o) return 8;
  wprintf(L"Packed %u files (%.1f MB) into %ls\n", footer.fileCount, footer.totalRaw / 1048576.0, out.c_str());
  return 0;
}

// ---- The newest version ------------------------------------------------------------------
// Setup installs the newest Wavy-Flo, not only the one it carries: it asks the
// place Wavy-Flo asks for updates, and when something newer is out there it
// fetches that version's Setup, checks it arrived whole, and hands over to it.
// With no network, or nothing newer, it installs what it carries.
int compareVersions(std::wstring a, std::wstring b) {
  auto parts = [](std::wstring v) {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
    std::vector<int> numbers;
    size_t start = 0;
    while (start <= v.size()) {
      size_t end = v.find(L'.', start);
      if (end == std::wstring::npos) end = v.size();
      numbers.push_back(_wtoi(v.substr(start, end - start).c_str()));
      start = end + 1;
    }
    return numbers;
  };
  const auto x = parts(a), y = parts(b);
  for (size_t i = 0; i < (std::max)(x.size(), y.size()); ++i) {
    const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}
// GET over HTTP(S): into `body`, or into the file `saveTo` with `progress` as it comes.
bool httpGet(const std::wstring &url, std::string *body, const std::wstring &saveTo, DWORD timeoutMs,
             const std::function<void(uint64_t got, uint64_t total)> &progress = {}) {
  wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
  URL_COMPONENTS parts{sizeof(parts)};
  parts.lpszHostName = host; parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path; parts.dwUrlPathLength = DWORD(std::size(path));
  parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = DWORD(std::size(extra));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
  bool ok = false;
  HINTERNET session = WinHttpOpen(L"Wavy-Flo-Setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, int(timeoutMs), int(timeoutMs), int(timeoutMs), int(timeoutMs));
  HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", (std::wstring(path) + extra).c_str(), nullptr,
                                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                 : nullptr;
  if (request && body) WinHttpAddRequestHeaders(request, L"Accept: application/vnd.github+json, application/json\r\n", DWORD(-1),
                                                WINHTTP_ADDREQ_FLAG_ADD);
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    DWORD length = 0;
    size = sizeof(length);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &length, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      std::ofstream file;
      if (!saveTo.empty()) file.open(saveTo, std::ios::binary | std::ios::trunc);
      ok = saveTo.empty() || bool(file);
      uint64_t got = 0;
      std::vector<char> chunk(1 << 16);
      for (DWORD read = 0; ok && WinHttpReadData(request, chunk.data(), DWORD(chunk.size()), &read) && read > 0;) {
        if (body) body->append(chunk.data(), read);
        if (file.is_open()) file.write(chunk.data(), read);
        got += read;
        if (progress) progress(got, length);
      }
      ok = ok && (length == 0 || got == length);
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}
std::wstring sha256Of(const std::wstring &path) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::wstring hex;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chunk(1 << 16);
    while (in) {
      in.read(chunk.data(), std::streamsize(chunk.size()));
      if (in.gcount() > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()), ULONG(in.gcount()), 0);
    }
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
      const wchar_t *digits = L"0123456789abcdef";
      for (unsigned char b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}
struct Newest {
  bool found = false;
  std::wstring version, url, sha256;
};
// What the update address says is the newest Setup: a GitHub release (its
// tag, and its asset ending in Setup.exe) or Wavy-Flo's own small JSON.
Newest newestSetup(DWORD timeoutMs) {
  Newest newest;
  std::wstring address = regString(HKEY_CURRENT_USER, L"Software\\Wavy-Flo\\Wavy-Flo\\update", L"source");
  if (address.empty()) address = L"https://api.github.com/repos/Kibo4aa/Wavy-Flo-Releases/releases/latest";
  std::string json;
  if (!httpGet(address, &json, L"", timeoutMs) || json.empty()) return newest;
  std::smatch m;
  auto first = [&](const std::string &text, const char *pattern) {
    std::smatch found;
    return std::regex_search(text, found, std::regex(pattern)) ? found[1].str() : std::string();
  };
  std::string version = first(json, R"re("tag_name"\s*:\s*"v?([^"]+)")re"), url, sha;
  if (!version.empty()) {
    // The Setup among the release's files: from its name to its download address.
    const std::regex name(R"re("name"\s*:\s*"[^"]*Setup\.exe")re");
    if (std::regex_search(json, m, name)) {
      const std::string rest = json.substr(size_t(m.position(0)));
      const size_t end = rest.find("\"browser_download_url\"");
      if (end != std::string::npos) {
        url = first(rest.substr(end), R"re("browser_download_url"\s*:\s*"([^"]+)")re");
        sha = first(rest.substr(0, end), R"re("digest"\s*:\s*"sha256:([0-9a-fA-F]{64})")re");
      }
    }
    if (sha.empty()) sha = first(json, R"re(Setup SHA-256:\s*([0-9a-fA-F]{64}))re");
  } else {
    version = first(json, R"re("version"\s*:\s*"v?([^"]+)")re");
    url = first(json, R"re("url"\s*:\s*"([^"]+\.exe)")re");
    sha = first(json, R"re("sha256"\s*:\s*"([0-9a-fA-F]{64})")re");
  }
  if (version.empty() || url.empty()) return newest;
  newest.found = true;
  newest.version = widen(version);
  newest.url = widen(url);
  for (char &c : sha) c = char(tolower(c));
  newest.sha256 = widen(sha);
  return newest;
}

// ---- Installing --------------------------------------------------------------------
struct Options {
  std::wstring dir;
  bool desktop = true, associate = true, launch = true;
  DWORD waitPid = 0;
};
using Progress = std::function<void(double fraction, const std::wstring &what)>;

void shortcut(const std::wstring &link, const std::wstring &target, const std::wstring &folder) {
  IShellLinkW *shell = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return;
  shell->SetPath(target.c_str());
  shell->SetWorkingDirectory(folder.c_str());
  shell->SetDescription(L"Wavy-Flo: motion graphics and music videos");
  shell->SetIconLocation(target.c_str(), 0);
  IPersistFile *file = nullptr;
  if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) {
    file->Save(link.c_str(), TRUE);
    file->Release();
  }
  shell->Release();
}
// Whether a shortcut points into `dir`: only those are ours to take away.
bool linkInto(const std::wstring &link, const std::wstring &dir) {
  IShellLinkW *shell = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
  bool inside = false;
  IPersistFile *file = nullptr;
  if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) {
    wchar_t target[MAX_PATH * 4] = {};
    if (SUCCEEDED(file->Load(link.c_str(), STGM_READ)) && SUCCEEDED(shell->GetPath(target, int(std::size(target)), nullptr, 0))) {
      const std::wstring prefix = join(dir, L"");
      inside = _wcsnicmp(target, prefix.c_str(), prefix.size()) == 0;
    }
    file->Release();
  }
  shell->Release();
  return inside;
}
std::wstring startMenuLink() { return join(knownFolder(FOLDERID_Programs), L"Wavy-Flo.lnk"); }
std::wstring desktopLink() { return join(knownFolder(FOLDERID_Desktop), L"Wavy-Flo.lnk"); }

// Writes one file in place, waiting a little for a file Windows still holds.
bool writeOver(const std::wstring &path, const std::vector<uint8_t> &bytes) {
  const std::wstring temp = path + L".new";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    if (!o) return false;
  }
  for (int attempt = 0; attempt < 40; ++attempt) {
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    Sleep(250);
  }
  DeleteFileW(temp.c_str());
  return false;
}

std::wstring install(const Payload &payload, const Options &options, const Progress &progress) {
  if (options.waitPid) {
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, options.waitPid)) {
      progress(0, L"Waiting for Wavy-Flo to close...");
      WaitForSingleObject(process, 120000);
      CloseHandle(process);
    }
  }
  const std::wstring dir = options.dir;
  if (!makeFolders(dir)) return L"Cannot make the folder " + dir + L". Choose another one.";
  {
    // A folder that can be written to, before anything is put in it.
    const std::wstring probe = join(dir, L".wavy-flo-setup-probe");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return L"Setup cannot write to " + dir + L".\nChoose a folder of your own, or run Setup as administrator.";
    CloseHandle(h);
  }
  std::ifstream in(selfPath(), std::ios::binary);
  if (!in) return L"Setup cannot read itself.";
  DECOMPRESSOR_HANDLE decompressor = nullptr;
  if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) return L"Windows cannot unpack the files.";
  uint64_t done = 0;
  std::wstring list;
  for (const auto &e : payload.entries) {
    progress(payload.footer.totalRaw ? double(done) / payload.footer.totalRaw : 0, e.path);
    std::vector<uint8_t> packed(size_t(e.packed)), raw(size_t(e.raw));
    in.seekg(std::streamoff(e.offset));
    in.read(reinterpret_cast<char *>(packed.data()), std::streamsize(packed.size()));
    SIZE_T got = 0;
    if (e.raw && (!Decompress(decompressor, packed.data(), packed.size(), raw.data(), raw.size(), &got) || got != e.raw)) {
      CloseDecompressor(decompressor);
      return L"A file of the program is damaged: " + e.path + L". Download Setup again.";
    }
    const std::wstring target = join(dir, e.path);
    makeFolders(parentOf(target));
    if (!writeOver(target, raw)) {
      CloseDecompressor(decompressor);
      return L"Cannot write " + target + L".\nIs Wavy-Flo still open? Close it and try again.";
    }
    list += e.path + L"\n";
    done += e.raw;
  }
  CloseDecompressor(decompressor);
  progress(1, L"Finishing...");
  // The uninstaller: this installer without the files it carries.
  {
    std::vector<uint8_t> self;
    readFile(selfPath(), self);
    self.resize(size_t(std::min<uint64_t>(self.size(), payload.footer.payloadOffset)));
    dropSignature(self);
    writeOver(join(dir, kUninstaller), self);
    list += std::wstring(kUninstaller) + L"\n" + kManifest + L"\n";
  }
  // What the version before installed and this one no longer has (a file that
  // was renamed, an effect taken out) goes, so nothing old is left loading
  // beside its replacement. Only files from that list, never anything else.
  {
    std::vector<uint8_t> before;
    if (readFile(join(dir, kManifest), before)) {
      std::wstring now = L"\n" + list;
      std::wstring text = widen(std::string(before.begin(), before.end()));
      size_t start = 0;
      while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring rel = text.substr(start, end - start);
        start = end + 1;
        if (rel.empty() || rel.find(L"..") != std::wstring::npos || rel.find(L':') != std::wstring::npos) continue;
        if (now.find(L"\n" + rel + L"\n") == std::wstring::npos) DeleteFileW(join(dir, rel).c_str());
      }
    }
  }
  {
    std::ofstream manifest(join(dir, kManifest), std::ios::binary | std::ios::trunc);
    const std::string utf8 = narrow(list);
    manifest.write(utf8.data(), std::streamsize(utf8.size()));
  }
  const std::wstring exe = join(dir, L"Wavy-Flo.exe");
  // In the Start menu: what Windows search finds.
  shortcut(startMenuLink(), exe, dir);
  if (options.desktop) shortcut(desktopLink(), exe, dir);
  // Settings > Apps.
  const HKEY user = HKEY_CURRENT_USER;
  regSet(user, kUninstallKey, L"DisplayName", kAppName);
  regSet(user, kUninstallKey, L"DisplayVersion", payload.version);
  regSet(user, kUninstallKey, L"Publisher", L"Wavy-Flo");
  regSet(user, kUninstallKey, L"DisplayIcon", exe + L",0");
  regSet(user, kUninstallKey, L"InstallLocation", dir);
  regSet(user, kUninstallKey, L"URLInfoAbout", L"https://github.com/Kibo4aa/Wavy-Flo-Releases");
  regSet(user, kUninstallKey, L"UninstallString", L"\"" + join(dir, kUninstaller) + L"\" --uninstall");
  regSet(user, kUninstallKey, L"QuietUninstallString", L"\"" + join(dir, kUninstaller) + L"\" --uninstall --silent");
  regSetDword(user, kUninstallKey, L"NoModify", 1);
  regSetDword(user, kUninstallKey, L"NoRepair", 1);
  regSetDword(user, kUninstallKey, L"EstimatedSize", DWORD(payload.footer.totalRaw / 1024));
  regSetDword(user, kUninstallKey, L"DesktopShortcut", options.desktop ? 1 : 0);
  // .wavyflo projects open in Wavy-Flo, and so do those saved under the old name's .anjimn.
  if (options.associate) {
    regSet(user, L"Software\\Classes\\.wavyflo", nullptr, L"WavyFlo.Project");
    regSet(user, L"Software\\Classes\\.anjimn", nullptr, L"WavyFlo.Project");
    regSet(user, L"Software\\Classes\\WavyFlo.Project", nullptr, L"Wavy-Flo Project");
    regSet(user, L"Software\\Classes\\WavyFlo.Project\\DefaultIcon", nullptr, exe + L",0");
    regSet(user, L"Software\\Classes\\WavyFlo.Project\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
    regSetDword(user, kUninstallKey, L"Associated", 1);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  }
  return {};
}

// ---- Uninstalling ------------------------------------------------------------------------
int uninstall(const std::wstring &dir, bool silent, DWORD waitPid) {
  if (!silent && MessageBoxW(nullptr, L"Remove Wavy-Flo from this computer?\n\nYour projects are not touched.",
                             L"Uninstall Wavy-Flo", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return 1;
  if (waitPid)
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) { WaitForSingleObject(process, 30000); CloseHandle(process); }
  // Exactly the files that were installed, and the folders they leave empty.
  std::vector<uint8_t> bytes;
  std::vector<std::wstring> files;
  if (readFile(join(dir, kManifest), bytes)) {
    std::wstring text = widen(std::string(bytes.begin(), bytes.end()));
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find(L'\n', start);
      if (end == std::wstring::npos) end = text.size();
      std::wstring line = text.substr(start, end - start);
      if (!line.empty() && line.find(L"..") == std::wstring::npos && line.find(L':') == std::wstring::npos) files.push_back(line);
      start = end + 1;
    }
  }
  const std::wstring exe = join(dir, L"Wavy-Flo.exe");
  for (int attempt = 0; attempt < 20; ++attempt) {
    // Wavy-Flo still open holds its own exe.
    HANDLE h = CreateFileW(exe.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE || GetLastError() == ERROR_FILE_NOT_FOUND) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); break; }
    if (attempt == 19) {
      if (!silent) MessageBoxW(nullptr, L"Close Wavy-Flo first, then uninstall again.", L"Uninstall Wavy-Flo", MB_ICONWARNING);
      return 2;
    }
    Sleep(500);
  }
  // What Wavy-Flo itself wrote beside its exe: its logs and crash reports.
  for (const wchar_t *pattern : {L"wavy-flo-log*.txt", L"crash-report-*.txt"}) {
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(join(dir, pattern).c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(found.cFileName);
    } while (FindNextFileW(h, &found));
    FindClose(h);
  }
  std::vector<std::wstring> folders;
  for (const auto &rel : files) {
    DeleteFileW(join(dir, rel).c_str());
    for (std::wstring parent = parentOf(rel); !parent.empty(); parent = parentOf(parent)) folders.push_back(parent);
  }
  std::sort(folders.begin(), folders.end(), [](const std::wstring &a, const std::wstring &b) { return a.size() > b.size(); });
  for (const auto &folder : folders) RemoveDirectoryW(join(dir, folder).c_str()); // only if empty
  RemoveDirectoryW(dir.c_str());                                                   // likewise
  for (const std::wstring &link : {startMenuLink(), desktopLink()})
    if (linkInto(link, dir)) DeleteFileW(link.c_str());
  if (regString(HKEY_CURRENT_USER, L"Software\\Classes\\.wavyflo", nullptr) == L"WavyFlo.Project") {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.wavyflo");
    if (regString(HKEY_CURRENT_USER, L"Software\\Classes\\.anjimn", nullptr) == L"WavyFlo.Project")
      RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.anjimn");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\WavyFlo.Project");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
  if (!silent) MessageBoxW(nullptr, L"Wavy-Flo has been removed.", L"Uninstall Wavy-Flo", MB_ICONINFORMATION);
  return 0;
}

// ---- The window ----------------------------------------------------------------------------
enum : UINT { kProgressMessage = WM_APP + 1, kDoneMessage = WM_APP + 2, kHandedOverMessage = WM_APP + 3 };
enum : int { idPath = 100, idBrowse, idDesktop, idAssociate, idInstall, idCancel, idProgress, idStatus, idLaunch };

struct Ui {
  HWND window = nullptr, path = nullptr, browse = nullptr, desktop = nullptr, associate = nullptr, install = nullptr,
       cancel = nullptr, progress = nullptr, status = nullptr, launch = nullptr, label = nullptr;
  HFONT font = nullptr, title = nullptr, subtitle = nullptr;
  Payload payload;
  Options options;
  bool updateMode = false, installing = false, finished = false;
  bool askNewest = true;   // look for a newer Setup before installing (not when one handed over to this one)
  bool online = false;     // carries no program: the Wavy-Flo Installer, which always fetches the newest
  bool startAtOnce = false; // handed over from an older Setup: install with the choices it was given
  std::wstring error, current;
  double fraction = 0;
  int dpi = 96;
  int s(int v) const { return MulDiv(v, dpi, 96); }
} ui;

void setStatus(const std::wstring &text) { SetWindowTextW(ui.status, text.c_str()); }

void startInstall() {
  wchar_t buffer[MAX_PATH * 4] = {};
  GetWindowTextW(ui.path, buffer, int(std::size(buffer)));
  ui.options.dir = buffer;
  while (!ui.options.dir.empty() && (ui.options.dir.back() == L'\\' || ui.options.dir.back() == L' ')) ui.options.dir.pop_back();
  if (ui.options.dir.size() < 3) { MessageBoxW(ui.window, L"Choose a folder to install into.", kAppName, MB_ICONWARNING); return; }
  ui.options.desktop = SendMessageW(ui.desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
  ui.options.associate = SendMessageW(ui.associate, BM_GETCHECK, 0, 0) == BST_CHECKED;
  ui.installing = true;
  for (HWND w : {ui.path, ui.browse, ui.desktop, ui.associate, ui.install}) EnableWindow(w, FALSE);
  ShowWindow(ui.progress, SW_SHOW);
  ShowWindow(ui.status, SW_SHOW);
  std::thread([] {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto report = [](double f, const std::wstring &what) {
      ui.fraction = f;
      ui.current = what;
      PostMessageW(ui.window, kProgressMessage, 0, 0);
    };
    if (ui.askNewest) {
      report(0, L"Looking for the newest Wavy-Flo...");
      const Newest newest = newestSetup(6000);
      if (newest.found && (ui.online || compareVersions(newest.version, ui.payload.version) > 0)) {
        const std::wstring saved = join(knownFolder(FOLDERID_LocalAppData), L"Temp\\Wavy-Flo-" + newest.version + L"-Setup.exe");
        report(0, L"Downloading Wavy-Flo " + newest.version + L"...");
        bool whole = httpGet(newest.url, nullptr, saved, 30000, [&](uint64_t got, uint64_t total) {
          if (total) report(double(got) / double(total), L"Downloading Wavy-Flo " + newest.version + L"...  " +
                                                             std::to_wstring(got / 1048576) + L" of " +
                                                             std::to_wstring(total / 1048576) + L" MB");
        });
        if (whole && !newest.sha256.empty()) whole = sha256Of(saved) == newest.sha256;
        if (whole) {
          // The newer Setup carries on with the same choices; this one steps aside.
          std::wstring command = L"\"" + saved + L"\" --continue --dir \"" + ui.options.dir + L"\" --desktop " +
                                 (ui.options.desktop ? L"1" : L"0") + L" --associate " + (ui.options.associate ? L"1" : L"0");
          STARTUPINFOW si{sizeof(si)};
          PROCESS_INFORMATION pi{};
          if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CoUninitialize();
            PostMessageW(ui.window, kHandedOverMessage, 0, 0);
            return;
          }
        }
        DeleteFileW(saved.c_str());
        if (!ui.online) {
          report(0, L"Wavy-Flo " + newest.version + L" could not be fetched; installing " + ui.payload.version + L".");
          Sleep(1500);
        }
      }
    }
    // The Installer has nothing of its own to fall back on.
    if (ui.online) {
      ui.error = L"Wavy-Flo could not be downloaded.\nCheck the internet connection and press Install again.";
      CoUninitialize();
      PostMessageW(ui.window, kDoneMessage, 0, 0);
      return;
    }
    ui.error = install(ui.payload, ui.options, report);
    CoUninitialize();
    PostMessageW(ui.window, kDoneMessage, 0, 0);
  }).detach();
}

void launchApp() {
  const std::wstring exe = join(ui.options.dir, L"Wavy-Flo.exe");
  ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, ui.options.dir.c_str(), SW_SHOWNORMAL);
}

void browseFolder() {
  IFileOpenDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
  DWORD flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Install Wavy-Flo into");
  if (SUCCEEDED(dialog->Show(ui.window))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR chosen = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen))) {
        std::wstring folder = chosen;
        // A folder of its own inside the one chosen, unless it already is one.
        const std::wstring last = folder.substr(folder.find_last_of(L'\\') + 1);
        if (_wcsicmp(last.c_str(), L"Wavy-Flo") != 0) folder = join(folder, L"Wavy-Flo");
        SetWindowTextW(ui.path, folder.c_str());
        CoTaskMemFree(chosen);
      }
      item->Release();
    }
  }
  dialog->Release();
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
  case WM_CTLCOLORSTATIC: {
    HDC dc = HDC(wParam);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(40, 44, 56));
    return LRESULT(GetSysColorBrush(COLOR_WINDOW));
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    // The band at the top: the name, the version, and a line of the accent.
    RECT band{0, 0, client.right, ui.s(86)};
    HBRUSH dark = CreateSolidBrush(RGB(22, 24, 32));
    FillRect(dc, &band, dark);
    DeleteObject(dark);
    RECT accent{0, band.bottom - ui.s(3), client.right, band.bottom};
    HBRUSH teal = CreateSolidBrush(RGB(0, 212, 160));
    FillRect(dc, &accent, teal);
    DeleteObject(teal);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, ui.title);
    SetTextColor(dc, RGB(78, 224, 181));
    RECT t{ui.s(24), ui.s(14), client.right - ui.s(24), ui.s(52)};
    DrawTextW(dc, L"Wavy-Flo", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, ui.subtitle);
    SetTextColor(dc, RGB(174, 180, 196));
    RECT st{ui.s(26), ui.s(50), client.right - ui.s(24), ui.s(76)};
    const std::wstring sub = ui.online ? std::wstring(L"Installer \x00B7 always installs the newest version")
                                       : std::wstring(ui.updateMode ? L"Updating to version " : L"Setup \x00B7 version ") + ui.payload.version;
    DrawTextW(dc, sub.c_str(), -1, &st, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case idBrowse: browseFolder(); return 0;
    case idInstall:
      if (ui.finished) {
        if (SendMessageW(ui.launch, BM_GETCHECK, 0, 0) == BST_CHECKED) launchApp();
        DestroyWindow(hwnd);
      } else startInstall();
      return 0;
    case idCancel:
      if (!ui.installing || ui.finished) DestroyWindow(hwnd);
      return 0;
    }
    break;
  case kProgressMessage:
    SendMessageW(ui.progress, PBM_SETPOS, WPARAM(ui.fraction * 1000), 0);
    setStatus(ui.current);
    return 0;
  case kDoneMessage:
    ui.finished = true;
    if (!ui.error.empty()) {
      ui.installing = false;
      ui.finished = false;
      setStatus(L"Not installed.");
      MessageBoxW(hwnd, ui.error.c_str(), kAppName, MB_ICONERROR);
      if (ui.updateMode) { DestroyWindow(hwnd); return 0; }
      for (HWND w : {ui.path, ui.browse, ui.desktop, ui.associate, ui.install}) EnableWindow(w, TRUE);
      return 0;
    }
    SendMessageW(ui.progress, PBM_SETPOS, 1000, 0);
    if (ui.updateMode) {
      setStatus(L"Updated. Opening Wavy-Flo...");
      launchApp();
      SetTimer(hwnd, 1, 900, nullptr);
      return 0;
    }
    setStatus(L"Wavy-Flo is installed. Find it in the Start menu, or search for \"Wavy-Flo\".");
    ShowWindow(ui.launch, SW_SHOW);
    SetWindowTextW(ui.install, L"Finish");
    EnableWindow(ui.install, TRUE);
    EnableWindow(ui.cancel, FALSE);
    return 0;
  case kHandedOverMessage:
    // A newer Setup has taken over; it has its own window.
    ui.finished = true;
    DestroyWindow(hwnd);
    return 0;
  case WM_TIMER:
    DestroyWindow(hwnd);
    return 0;
  case WM_CLOSE:
    if (ui.installing && !ui.finished) return 0; // not halfway through
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, ui.s(x), ui.s(y), ui.s(w), ui.s(h), ui.window,
                           HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, WPARAM(ui.font), TRUE);
  return c;
}

int runWindow(HINSTANCE instance) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = windowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"WavyFloSetup";
  RegisterClassExW(&wc);
  ui.dpi = int(GetDpiForSystem());
  const int width = ui.s(560), height = ui.updateMode ? ui.s(230) : ui.s(360);
  RECT frame{0, 0, width, height};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRectExForDpi(&frame, style, FALSE, 0, UINT(ui.dpi));
  const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  ui.window = CreateWindowExW(0, wc.lpszClassName, ui.updateMode ? L"Updating Wavy-Flo" : ui.online ? L"Wavy-Flo Installer" : L"Wavy-Flo Setup", style,
                              (GetSystemMetrics(SM_CXSCREEN) - fw) / 2, (GetSystemMetrics(SM_CYSCREEN) - fh) / 2, fw, fh,
                              nullptr, nullptr, instance, nullptr);
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, UINT(ui.dpi));
  ui.font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW big = metrics.lfMessageFont;
  big.lfHeight = -ui.s(28);
  big.lfWeight = FW_BOLD;
  ui.title = CreateFontIndirectW(&big);
  LOGFONTW caption = metrics.lfMessageFont;
  caption.lfHeight = -ui.s(14);
  ui.subtitle = CreateFontIndirectW(&caption);
  if (ui.updateMode) {
    ui.progress = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 24, 112, 512, 14, idProgress);
    ui.status = control(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS, 24, 136, 512, 20, idStatus);
    SendMessageW(ui.progress, PBM_SETRANGE32, 0, 1000);
    // Out of sight: the controls the install reads and the buttons it greys out.
    ui.path = control(L"EDIT", ui.options.dir.c_str(), 0, 0, 0, 0, 0, idPath);
    ShowWindow(ui.path, SW_HIDE);
    for (HWND *w : {&ui.browse, &ui.desktop, &ui.associate, &ui.install, &ui.cancel, &ui.launch}) {
      *w = control(L"BUTTON", L"", BS_AUTOCHECKBOX, 0, 0, 0, 0, 0);
      ShowWindow(*w, SW_HIDE);
    }
    SendMessageW(ui.desktop, BM_SETCHECK, ui.options.desktop ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ui.associate, BM_SETCHECK, ui.options.associate ? BST_CHECKED : BST_UNCHECKED, 0);
  } else {
    ui.label = control(L"STATIC", L"Install Wavy-Flo into:", 0, 24, 104, 512, 18, 0);
    ui.path = control(L"EDIT", ui.options.dir.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 24, 126, 410, 26, idPath);
    ui.browse = control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 444, 125, 92, 28, idBrowse);
    ui.desktop = control(L"BUTTON", L"Put a shortcut on the desktop", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 168, 512, 22, idDesktop);
    ui.associate = control(L"BUTTON", L"Open .wavyflo projects with Wavy-Flo", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 194, 512, 22, idAssociate);
    SendMessageW(ui.desktop, BM_SETCHECK, ui.options.desktop ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ui.associate, BM_SETCHECK, BST_CHECKED, 0);
    ui.progress = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 24, 236, 512, 14, idProgress);
    ui.status = control(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS, 24, 258, 512, 20, idStatus);
    ui.launch = control(L"BUTTON", L"Open Wavy-Flo now", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 288, 300, 22, idLaunch);
    SendMessageW(ui.launch, BM_SETCHECK, BST_CHECKED, 0);
    ShowWindow(ui.progress, SW_HIDE);
    ShowWindow(ui.status, SW_HIDE);
    ShowWindow(ui.launch, SW_HIDE);
    SendMessageW(ui.progress, PBM_SETRANGE32, 0, 1000);
    ui.cancel = control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 436, 318, 100, 30, idCancel);
    ui.install = control(L"BUTTON", L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, 326, 318, 100, 30, idInstall);
  }
  ShowWindow(ui.window, SW_SHOW);
  UpdateWindow(ui.window);
  if (ui.updateMode || ui.startAtOnce) startInstall();
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(ui.window, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return ui.error.empty() && ui.finished ? 0 : 1;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args(argv + 1, argv + argc);
  LocalFree(argv);
  auto has = [&](const wchar_t *flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
  auto valueOf = [&](const wchar_t *flag) {
    auto it = std::find(args.begin(), args.end(), flag);
    return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::wstring();
  };
  // Making an installer: --pack <folder> <out.exe> <version>. Printed to a console when there is one.
  if (has(L"--pack")) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    FILE *f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    auto it = std::find(args.begin(), args.end(), L"--pack");
    if (std::distance(it, args.end()) < 4) { fwprintf(stderr, L"--pack <folder> <out.exe> <version>\n"); return 2; }
    return pack(*(it + 1), *(it + 2), narrow(*(it + 3)));
  }
  const bool silent = has(L"--silent");
  const DWORD waitPid = DWORD(_wtoi(valueOf(L"--wait-pid").c_str()));
  if (has(L"--uninstall")) {
    const std::wstring dir = parentOf(selfPath());
    // Run from a copy in the temporary folder, so this one can go too.
    const std::wstring temp = join(knownFolder(FOLDERID_LocalAppData), L"Temp\\Wavy-Flo-Uninstall.exe");
    if (_wcsicmp(selfPath().c_str(), temp.c_str()) != 0) {
      if (!CopyFileW(selfPath().c_str(), temp.c_str(), FALSE)) return uninstall(dir, silent, waitPid);
      std::wstring command = L"\"" + temp + L"\" --uninstall-from \"" + dir + L"\" --wait-pid " +
                             std::to_wstring(GetCurrentProcessId()) + (silent ? L" --silent" : L"");
      STARTUPINFOW si{sizeof(si)};
      PROCESS_INFORMATION pi{};
      if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
      }
      return uninstall(dir, silent, waitPid);
    }
  }
  if (has(L"--uninstall-from")) {
    const int result = uninstall(valueOf(L"--uninstall-from"), silent, waitPid);
    // This copy in the temporary folder, gone a moment after it closes.
    std::wstring command = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /q \"" + selfPath() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
    return result;
  }
  // Carrying no program, this is the Wavy-Flo Installer: made once, it always
  // fetches and installs the newest Wavy-Flo, so it never needs replacing.
  if (!ui.payload.read(selfPath())) {
    if (has(L"--silent") || has(L"--update") || has(L"--continue")) {
      // Never a window when asked for none.
      if (!has(L"--silent"))
        MessageBoxW(nullptr, L"This copy of Wavy-Flo Setup carries no program. Download Setup again.", kAppName, MB_ICONERROR);
      return 3;
    }
    ui.online = true;
  }
  // Where it goes: where it went last time, else a folder of the person's own.
  std::wstring dir = valueOf(L"--dir");
  if (dir.empty()) dir = regString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
  if (dir.empty()) dir = join(knownFolder(FOLDERID_UserProgramFiles), L"Wavy-Flo");
  ui.options.dir = dir;
  ui.options.waitPid = waitPid;
  // An update keeps the choices made when it was installed.
  DWORD value = 1, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DesktopShortcut", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
    ui.options.desktop = value != 0;
  size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"Associated", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
    ui.options.associate = value != 0;
  if (silent) {
    ui.options.launch = has(L"--launch");
    const std::wstring error = install(ui.payload, ui.options, [](double, const std::wstring &) {});
    if (error.empty() && ui.options.launch)
      ShellExecuteW(nullptr, L"open", join(dir, L"Wavy-Flo.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return error.empty() ? 0 : 1;
  }
  ui.updateMode = has(L"--update");
  // Handed over from an older Setup: the choices made there, installed at once.
  if (has(L"--continue")) {
    ui.startAtOnce = true;
    if (has(L"--desktop")) ui.options.desktop = valueOf(L"--desktop") == L"1";
    if (has(L"--associate")) ui.options.associate = valueOf(L"--associate") == L"1";
  }
  // Wavy-Flo's own updater has already fetched the newest; so has a Setup that handed over.
  ui.askNewest = ui.online || (!ui.updateMode && !ui.startAtOnce && !has(L"--offline"));
  return runWindow(instance);
}
