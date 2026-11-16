#ifndef _WIN32
#include <thread>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif
#include "update_check.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "lh/version.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

namespace openhouse::overlay {
namespace {
// "v1.0.1", "1.0" -> {1, 0, 1}, {1, 0, 0}; missing parts are 0.
std::array<int, 3> ParseVersion(std::string v) {
  std::array<int, 3> out{0, 0, 0};
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  int part = 0;
  for (size_t i = 0; i < v.size() && part < 3;) {
    if (!std::isdigit(static_cast<unsigned char>(v[i]))) break;
    size_t end = i;
    while (end < v.size() && std::isdigit(static_cast<unsigned char>(v[end]))) ++end;
    out[part++] = std::atoi(v.substr(i, end - i).c_str());
    if (end >= v.size() || v[end] != '.') break;
    i = end + 1;
  }
  return out;
}

std::string TagName(const std::string& json) {
  const auto key = json.find("\"tag_name\"");
  if (key == std::string::npos) return {};
  const auto open = json.find('"', json.find(':', key) + 1);
  if (open == std::string::npos) return {};
  const auto close = json.find('"', open + 1);
  return close == std::string::npos ? std::string() : json.substr(open + 1, close - open - 1);
}

std::string FetchLatest() {
  std::string body;
#ifdef _WIN32
  HINTERNET session = WinHttpOpen(L"openhouse-overlay", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return body;
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
  HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/repos/YusufOzmen01/openhouse/releases/latest",
                                                   nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE)
                              : nullptr;
  if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA,
                                    0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    for (DWORD available = 0; status == 200 && WinHttpQueryDataAvailable(request, &available) && available;) {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
      body.append(chunk.data(), read);
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
#else
  if (std::system("command -v curl >/dev/null 2>&1") != 0) return body;
  FILE* pipe = popen("curl -fsSL --max-time 5 -H 'Accept: application/vnd.github+json' "
                     "https://api.github.com/repos/YusufOzmen01/openhouse/releases/latest 2>/dev/null", "r");
  if (!pipe) return body;
  char buffer[4096];
  for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) body.append(buffer, n);
  pclose(pipe);
#endif
  return body;
}
}  // namespace

UpdateCheck::~UpdateCheck() {
  if (thread_.joinable()) thread_.join();
}

bool UpdateCheck::Disabled() const { return std::getenv("OPENHOUSE_NO_UPDATE_CHECK") != nullptr; }

void UpdateCheck::Start() {
  if (busy_ || Disabled()) return;
  if (thread_.joinable()) thread_.join();
  busy_ = true;
  done_ = false;
  thread_ = std::thread([this] {
    const std::string tag = TagName(FetchLatest());
    {
      std::lock_guard lock(mutex_);
      latest_ = tag;
    }
    newer_ = !tag.empty() && ParseVersion(tag) > ParseVersion(lh::kVersion);
    done_ = true;
    busy_ = false;
  });
}

std::string UpdateCheck::Latest() const {
  std::lock_guard lock(mutex_);
  return latest_;
}

void UpdateCheck::OpenReleasesPage() {
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL);
#else
  std::string command = std::string("xdg-open ") + kReleasesPage + " >/dev/null 2>&1 &";
  if (std::system(command.c_str()) != 0) std::fprintf(stderr, "openhouse-overlay: %s\n", kReleasesPage);
#endif
}

void UpdateCheck::OpenLatestRelease() const {
  const std::string tag = Latest();
  if (tag.empty()) OpenReleasesPage();
  else Open("https://github.com/YusufOzmen01/openhouse/releases/tag/" + tag);
}

void UpdateCheck::Open(const std::string& target) {
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
  std::wstring wide(n > 0 ? n : 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wide.data(), n);
  ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  // No shell: the path goes to xdg-open as is, whatever is in it.
  char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(target.c_str()), nullptr};
  pid_t pid = 0;
  if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0) {
    std::thread([pid] { int status = 0; waitpid(pid, &status, 0); }).detach();
  }
#endif
}

}  // namespace openhouse::overlay
