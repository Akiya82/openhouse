#include "ui.hpp"

#include <imgui.h>

#include <map>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdlib>

#include "common/nordic_dfu.hpp"
#include "lh/version.hpp"

namespace openhouse::installer {
namespace {

ImFont* g_title = nullptr;
ImFont* g_huge = nullptr;
const ImVec4 kGood{0.36f, 0.80f, 0.45f, 1}, kWarn{1.00f, 0.78f, 0.30f, 1}, kBad{1.00f, 0.42f, 0.40f, 1},
    kMuted{0.62f, 0.64f, 0.68f, 1};

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool BigButton(const char* label, float width = 0, float height = 0) {
  return ImGui::Button(label, ImVec2(width, height > 0 ? height : ImGui::GetFrameHeight() * 1.6f));
}

void Title(const char* text) {
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::Spacing();
}

void Wrapped(const char* text) {
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
}

void Step(int n, const char* text) {
  ImGui::TextColored(kWarn, "%d.", n);
  ImGui::SameLine();
  Wrapped(text);
}

// Two to four big buttons side by side, centred; returns the one pressed (-1: none).
enum class Tint { None, Red, Green, Yellow };

int ChoiceRow(const std::vector<const char*>& labels, float height = 0, const std::vector<Tint>& tints = {}) {
  const float gap = ImGui::GetStyle().ItemSpacing.x * 2;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float w = std::min(360.f, (avail - gap * float(labels.size() - 1)) / float(labels.size()));
  const float total = w * float(labels.size()) + gap * float(labels.size() - 1);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail - total) / 2));
  int pressed = -1;
  ImGui::PushFont(g_title);
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i) ImGui::SameLine(0, gap);
    const Tint tint = i < tints.size() ? tints[i] : Tint::None;
    if (tint != Tint::None) {
      const ImVec4 c = tint == Tint::Red ? ImVec4(0.62f, 0.24f, 0.24f, 1)
                     : tint == Tint::Green ? ImVec4(0.22f, 0.50f, 0.28f, 1)
                                           : ImVec4(0.62f, 0.50f, 0.16f, 1);
      ImGui::PushStyleColor(ImGuiCol_Button, c);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 1.2f, c.y * 1.2f, c.z * 1.2f, 1));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.85f, c.y * 0.85f, c.z * 0.85f, 1));
    }
    if (BigButton(labels[i], w, height > 0 ? height : ImGui::GetFrameHeight() * 3)) pressed = int(i);
    if (tint != Tint::None) ImGui::PopStyleColor(3);
  }
  ImGui::PopFont();
  return pressed;
}

void Warning(const char* heading, const char* body) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.45f, 0.10f, 0.10f, 1));
  ImGui::BeginChild(heading, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted(heading);
  ImGui::PopFont();
  Wrapped(body);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void JobLog(const Job& job) {
  for (const auto& line : job.LogLines()) ImGui::BulletText("%s", line.c_str());
}

void ProgressBar(const Job& job) {
  const std::string text = job.StatusText();
  ImGui::TextUnformatted(text.empty() ? "Working..." : text.c_str());
  char pct[16];
  std::snprintf(pct, sizeof pct, "%.0f%%", job.Progress() * 100);
  ImGui::ProgressBar(job.Progress(), ImVec2(-1, 0), pct);
}

// A device openhouse doesn't know: its id, its likely radio, a link to report it, and the
// user's go-ahead to flash it anyway. True once accepted.
bool UnknownDevice(uint32_t hwid, RadioFamily family, const std::string& key, std::set<std::string>* accepted) {
  ImGui::Indent();
  ImGui::TextColored(kWarn, "Unknown device (ID %08X). Device type: %s", hwid,
                     family == RadioFamily::Tracker10 ? "older hardware (VIVE 1.0, neo)" : "newer hardware (wm3)");
  ImGui::TextColored(kWarn, "The firmware might not work on it. Please report the device ID on GitHub.");
  if (ImGui::SmallButton(("Report on GitHub##" + key).c_str())) {
    char url[160];
    std::snprintf(url, sizeof url, "https://github.com/YusufOzmen01/openhouse/issues/new?title=New%%20device%%20ID%%3A%%20%08X", hwid);
    OpenInFileManager(Path(url));
  }
  ImGui::SameLine();
  bool ok = accepted->count(key) > 0;
  if (ImGui::Checkbox(("I accept##" + key).c_str(), &ok)) { if (ok) accepted->insert(key); else accepted->erase(key); }
  ImGui::Unindent();
  return ok;
}

// "1.0.10" > "1.0.9"; an empty version (before the marker existed) is older than any.
int CompareVersions(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return a.empty() == b.empty() ? 0 : a.empty() ? -1 : 1;
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    const long x = i < a.size() ? std::strtol(a.c_str() + i, nullptr, 10) : 0;
    const long y = j < b.size() ? std::strtol(b.c_str() + j, nullptr, 10) : 0;
    if (x != y) return x < y ? -1 : 1;
    i = a.find('.', i); i = i == std::string::npos ? a.size() : i + 1;
    j = b.find('.', j); j = j == std::string::npos ? b.size() : j + 1;
  }
  return 0;
}

// SteamVR's own 1.0 base station firmware: the newest numbered lighthouse_tx_htc_2_0 image
// (436), never the rescue ones.
fs::path StockBaseStationImage(const fs::path& steamvr) {
  fs::path best;
  int bestVersion = -1;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(steamvr / "tools" / "lighthouse" / "firmware" / "lighthouse_tx" / "archive" / "htc_2.0", ec)) {
    const std::string name = Utf8(e.path().filename());
    const std::string prefix = "lighthouse_tx_htc_2_0-";
    if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".bin") continue;
    const int version = std::atoi(name.c_str() + prefix.size());
    if (version > bestVersion) { bestVersion = version; best = e.path(); }
  }
  return best;
}

// The lhbs image in the package's firmwares/basestation folder.
fs::path OurBaseStationImage() {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(FirmwareFolder() / "basestation", ec))
    if (e.path().extension() == ".bin") return e.path();
  return {};
}

// A device row's radio: its colour (stock: none; Doorman: green when current, yellow when older)
// and what to show next to it.
struct RadioState {
  const ImVec4* color = nullptr;
  std::string text;
};
RadioState DescribeRadio(uint32_t version, RadioFamily family) {
  static std::map<RadioFamily, uint32_t> ours;
  if (!ours.count(family)) ours[family] = OurRadioVersion(family);
  char text[64];
  if (!version) return {nullptr, "Radio: unknown"};
  if (!IsDoormanRadio(version)) {
    std::snprintf(text, sizeof text, "Radio: stock %u", version);
    return {nullptr, text};
  }
  const bool current = ours[family] && version >= ours[family];
  std::snprintf(text, sizeof text, current ? "Radio: doorman %08X" : "Radio: doorman %08X (older)", version);
  return {current ? &kGood : &kWarn, text};
}

// Device lists fill the page down to what's under them: a warning line, Flash and Back.
float DeviceListReserve() {
  const ImGuiStyle& st = ImGui::GetStyle();
  return ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * 1.6f + st.ItemSpacing.y +
         ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y * 2;
}

fs::path HolyiotImage() { return FirmwareFolder() / "dongle" / "doorman-holyiot.hex"; }

fs::path DongleImage(bool softDevice) {
  return FirmwareFolder() / "dongle" / (softDevice ? "doorman-dongle-sd.uf2" : "doorman-dongle-nosd.uf2");
}

}  // namespace

void Job::Start(std::function<bool(Job&)> work) {
  Reset();
  running_ = true;
  thread_ = std::thread([this, work] {
    bool ok = false;
    std::lock_guard devices(g_deviceMutex);  // a stopped scan may still be finishing
    try {
      ok = work(*this);
    } catch (const std::exception& e) {
      Fail(e.what());
    }
    ok_ = ok;
    finished_ = true;
    running_ = false;
  });
}

void Ui::SetUpImGui() {
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.Fonts->Clear();
  // ImGui's own pixel font: sharp only at whole multiples of its 13 px.
  ImFontConfig font;
  font.OversampleH = font.OversampleV = 1;
  font.PixelSnapH = true;
  font.SizePixels = 13.f;
  io.Fonts->AddFontDefault(&font);
  font.SizePixels = 26.f;
  g_title = io.Fonts->AddFontDefault(&font);
  font.SizePixels = 39.f;
  g_huge = io.Fonts->AddFontDefault(&font);
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0;
  style.FrameRounding = 2;
  style.FramePadding = ImVec2(10, 7);
  style.ItemSpacing = ImVec2(10, 9);
  style.WindowPadding = ImVec2(18, 14);
}

Ui::Ui() {
  steamvr_ = FindSteamVR();
  askSteamVR_ = steamvr_.root.empty();
  RefreshStatus();
}

void Ui::RefreshStatus() { status_ = steamvr_.root.empty() ? DriverStatus{} : ReadStatus(steamvr_.root); }

void Ui::Go(Page page) {
  baseDrives_.Stop();
  dongleSeen_.Stop();
  wired_.Stop();
  wireless_.Stop();
  if (!job_.Running()) job_.Reset();
  page_ = page;
  checked_.clear();
  RefreshStatus();
  installAction_ = !status_.openhouse ? 0 : CompareVersions(status_.version, lh::kVersion) < 0 ? 1 : 2;
  switch (page) {
    case Page::BaseStation:
    case Page::RestoreBase:
      baseStep_ = BaseStep::Warning;
      confirmed10_ = false;
      break;
    case Page::Dongle: dongleStep_ = DongleStep::Board; break;
    case Page::RadioWired:
    case Page::RestoreRadio:
      radioStep_ = RadioStep::Choose;
      wired_.Start(FindWiredDevices, 2.0);
      break;
    case Page::RadioWireless:
    case Page::RestoreRadioWireless:
      radioStep_ = RadioStep::Choose;
      stockChoice_.clear();
      stockModels_ = steamvr_.root.empty() ? std::vector<StockModel>{} : StockModels(steamvr_.root);
      wireless_.Start(FindWirelessDevices, 3.0);
      break;
    default: break;
  }
}

void Ui::Back(Page to) {
  ImGui::Spacing();
  ImGui::BeginDisabled(job_.Running());
  if (ImGui::Button(to == Page::Home ? "< Home" : "< Back")) Go(to);
  ImGui::EndDisabled();
}

void Ui::Draw(float width, float height) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::Begin("openhouse installer", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  Header();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::BeginChild("page", ImVec2(0, -(ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2)));
  switch (page_) {
    case Page::Home: Home(); break;
    case Page::Install: InstallPage(true); break;
    case Page::Uninstall: InstallPage(false); break;
    case Page::FlashMenu: FlashMenu(); break;
    case Page::BaseStation: BaseStationPage(false); break;
    case Page::DoormanMenu: DoormanMenu(); break;
    case Page::Dongle: DonglePage(); break;
    case Page::RadioMenu: RadioMenu(); break;
    case Page::RadioWired: RadioWiredPage(false); break;
    case Page::RadioWireless: RadioWirelessPage(false); break;
    case Page::RestoreRadioWireless: RadioWirelessPage(true); break;
    case Page::RestoreRadioMenu: RestoreRadioMenu(); break;
    case Page::RestoreMenu: RestoreMenu(); break;
    case Page::RestoreBase: BaseStationPage(true); break;
    case Page::RestoreRadio: RadioWiredPage(true); break;
  }
  ImGui::EndChild();
  Footer();
  SteamVRPrompt();
  ImGui::End();
}

void Ui::Header() {
  const float side = ImGui::GetFontSize() * 4;
  if (logo_) {
    ImGui::Image(static_cast<ImTextureID>(logo_), ImVec2(side, side));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::PushFont(g_huge);
  ImGui::TextUnformatted("openhouse");
  ImGui::PopFont();
  ImGui::TextColored(kMuted, "Installer v%s", lh::kVersion);
  ImGui::EndGroup();
}

// Bottom: what's installed now, and where SteamVR is.
void Ui::Footer() {
  ImGui::Separator();
  if (steamvr_.root.empty()) {
    ImGui::TextColored(kBad, "SteamVR not found");
  } else if (!status_.driverFound) {
    ImGui::TextColored(kBad, "SteamVR's lighthouse driver not found");
  } else if (status_.openhouse) {
    if (status_.version.empty()) ImGui::TextColored(kGood, "Installed: openhouse (1.0.3 or older)");
    else ImGui::TextColored(kGood, "Installed: openhouse v%s", status_.version.c_str());
  } else {
    ImGui::TextColored(kMuted, "Installed: SteamVR's own driver");
  }
  const char* label = steamvr_.root.empty() ? "Choose SteamVR folder" : "Change SteamVR folder";
  if (!steamvr_.root.empty()) {
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    // Long paths lose their start, so the button always fits.
    const float room = ImGui::GetWindowWidth() - ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x -
                       ImGui::CalcTextSize(label).x - ImGui::GetFontSize() * 3;
    std::string where = "SteamVR: " + Utf8(steamvr_.root);
    while (where.size() > 12 && ImGui::CalcTextSize(where.c_str()).x > room) where = "SteamVR: ..." + where.substr(13);
    ImGui::TextColored(kMuted, "%s", where.c_str());
  }
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                                                         ImGui::CalcTextSize(label).x - ImGui::GetStyle().FramePadding.x * 2));
  ImGui::BeginDisabled(job_.Running());
  if (ImGui::SmallButton(label)) askSteamVR_ = true;
  ImGui::EndDisabled();
}

void Ui::SteamVRPrompt() {
  if (askSteamVR_) {
    ImGui::OpenPopup("SteamVR folder");
    askSteamVR_ = false;
    std::snprintf(steamvrInput_, sizeof steamvrInput_, "%s", Utf8(steamvr_.root).c_str());
    steamvrError_.clear();
  }
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, 0));
  if (!ImGui::BeginPopupModal("SteamVR folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  if (steamvr_.root.empty()) {
    ImGui::TextColored(kWarn, "SteamVR couldn't be found. Please pick the folder SteamVR is installed in.");
  }
  ImGui::TextColored(kMuted, "Usually Steam/steamapps/common/SteamVR");
  ImGui::Spacing();
  ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7);
  ImGui::InputText("##steamvr", steamvrInput_, sizeof steamvrInput_);
  ImGui::SameLine();
  if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
    const std::string picked = PickFolder("Choose the SteamVR folder");
    if (!picked.empty()) std::snprintf(steamvrInput_, sizeof steamvrInput_, "%s", picked.c_str());
  }
  if (!steamvrError_.empty()) ImGui::TextColored(kBad, "%s", steamvrError_.c_str());
  ImGui::Spacing();
  if (BigButton("Use this folder", ImGui::GetFontSize() * 14)) {
    fs::path chosen = Path(steamvrInput_);
    // A Steam library or Steam itself picked by mistake: the SteamVR folder in it.
    for (const fs::path& c : {chosen, chosen / "SteamVR", chosen / "common" / "SteamVR", chosen / "steamapps" / "common" / "SteamVR"})
      if (IsSteamVRFolder(c)) { chosen = c; break; }
    if (IsSteamVRFolder(chosen)) {
      steamvr_.root = chosen;
      steamvr_.saved = true;
      RememberSteamVR(chosen);
      RefreshStatus();
      ImGui::CloseCurrentPopup();
    } else {
      steamvrError_ = "That isn't a SteamVR folder";
    }
  }
  ImGui::SameLine();
  if (BigButton(steamvr_.root.empty() ? "Not now" : "Cancel", ImGui::GetFontSize() * 10)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// Linux, before talking to devices: the udev rule that lets this user open them.
bool Ui::DeviceAccess() {
  if (Now() - accessChecked_ > 2.0) {
    haveAccess_ = DeviceAccessInstalled();
    accessChecked_ = Now();
  }
  if (haveAccess_) return true;
  ImGui::TextColored(kWarn, "Device access isn't set up.");
  ImGui::BeginDisabled(closeJob_.Running());
  if (BigButton(closeJob_.Running() ? "Setting up..." : "Set up device access")) {
    closeJob_.Start([](Job& j) {
      std::string error;
      if (!InstallDeviceAccess(&error)) { j.Fail(error); return false; }
      return true;
    });
    accessChecked_ = -1;
  }
  ImGui::EndDisabled();
  if (closeJob_.Finished()) {
    if (!closeJob_.Ok()) ImGui::TextColored(kBad, "Failed: %s", closeJob_.Error().c_str());
    accessChecked_ = -1;
  }
  return false;
}

bool Ui::RuntimesClosed() {
  if (Now() - runningChecked_ > 1.0) {
    running_ = RunningRuntimes();
    runningChecked_ = Now();
  }
  if (running_.empty()) return true;
  std::string list;
  for (const auto& n : running_) list += (list.empty() ? "" : ", ") + n;
  ImGui::TextColored(kWarn, "Close these first: %s", list.c_str());
  ImGui::BeginDisabled(closeJob_.Running());
  if (BigButton(closeJob_.Running() ? "Closing..." : "Close them")) {
    closeJob_.Start([](Job& j) {
      if (!CloseRuntimes()) { j.Fail("Couldn't close them, close them manually"); return false; }
      return true;
    });
  }
  ImGui::EndDisabled();
  if (closeJob_.Finished() && !closeJob_.Ok()) ImGui::TextColored(kBad, "%s", closeJob_.Error().c_str());
  return false;
}

// ---- home ----

void Ui::Home() {
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 2));
  const float avail = ImGui::GetContentRegionAvail().x;
  const char* lines[] = {"What would you like to do?"};
  ImGui::PushFont(g_title);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - ImGui::CalcTextSize(lines[0]).x) / 2);
  ImGui::TextUnformatted(lines[0]);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
  const float h = ImGui::GetFontSize() * 5;
  int top;
  if (status_.openhouse) {
    const bool older = CompareVersions(status_.version, lh::kVersion) < 0;
    top = ChoiceRow({older ? "Update Driver" : "Reinstall Driver", "Uninstall Driver"}, h,
                    {older ? Tint::Green : Tint::Yellow, Tint::Red});
  } else {
    top = ChoiceRow({"Install Driver"}, h);
  }
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
  const int bottom = ChoiceRow({"Flash Firmware", "Restore Firmware"}, h);
  if (top == 0) Go(Page::Install);
  if (top == 1) Go(Page::Uninstall);
  if (bottom == 0) Go(Page::FlashMenu);
  if (bottom == 1) Go(Page::RestoreMenu);
}

// ---- install / uninstall ----

void Ui::InstallPage(bool install) {
  // What Install does here, decided when the page opened (the status changes once it's done).
  static const char* const kActions[] = {"Install", "Update", "Reinstall"};
  static const char* const kDoing[] = {"Installing", "Updating", "Reinstalling"};
  static const char* const kDone[] = {"installed", "updated", "reinstalled"};
  const int a = install ? installAction_ : 0;
  Title(install ? (std::string(kActions[a]) + " Driver").c_str() : "Uninstall Driver");
  if (job_.Finished() && !statusFresh_) {
    RefreshStatus();
    statusFresh_ = true;
  }
  if (job_.Finished()) {
    if (job_.Ok()) {
      ImGui::PushFont(g_title);
      ImGui::TextColored(kGood, "%s", install ? (std::string("openhouse has been ") + kDone[a] + ".").c_str() : "openhouse has been uninstalled.");
      ImGui::PopFont();
      ImGui::TextUnformatted(install ? "You can start SteamVR now." : "SteamVR's own driver has been restored.");
    } else {
      ImGui::PushFont(g_title);
      ImGui::TextColored(kBad, "%s", install ? (std::string(kActions[a]) + " failed.").c_str() : "Uninstall failed.");
      ImGui::PopFont();
      ImGui::TextUnformatted(job_.Error().c_str());
    }
    Back(Page::Home);
    return;
  }
  if (job_.Running()) {
    ImGui::Text("%s openhouse...", install ? kDoing[a] : "Uninstalling");
    return;
  }
  if (steamvr_.root.empty()) {
    ImGui::TextColored(kBad, "SteamVR not found");
    if (BigButton("Choose SteamVR folder")) askSteamVR_ = true;
    Back(Page::Home);
    return;
  }
  if (install) {
    if (!PackageHasDriver()) {
      ImGui::TextColored(kBad, "Driver not found next to the installer. Extract the whole package first.");
      Back(Page::Home);
      return;
    }
    const std::string installed = status_.version.empty() ? "1.0.3 or older" : "v" + status_.version;
    if (a == 1) ImGui::Text("Updates openhouse %s to v%s.", installed.c_str(), lh::kVersion);
    else if (a == 2) ImGui::Text("openhouse %s is installed. Reinstalls v%s.", installed.c_str(), lh::kVersion);
    else ImGui::Text("Installs openhouse v%s.", lh::kVersion);
#ifndef _WIN32
    ImGui::Spacing();
    ImGui::Checkbox("Update check for Monado / WiVRn", &monadoCheck_);
#endif
  } else {
    if (!status_.openhouse) {
      ImGui::TextColored(kMuted, "openhouse isn't installed.");
    }
    ImGui::TextUnformatted("Restores SteamVR's own driver. Your room and settings are kept.");
  }
  ImGui::Spacing();
  if (!RuntimesClosed()) { Back(Page::Home); return; }
  if (!install) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.24f, 0.24f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.30f, 0.30f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.18f, 0.18f, 1));
  }
  if (install && a == 1) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.50f, 0.28f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.60f, 0.34f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.19f, 0.43f, 0.24f, 1));
  } else if (install && a == 2) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.50f, 0.16f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.60f, 0.20f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.53f, 0.43f, 0.14f, 1));
  }
  const bool start = BigButton(install ? kActions[a] : "Uninstall", ImGui::GetFontSize() * 14);
  if (install && a != 0) ImGui::PopStyleColor(3);
  if (!install) ImGui::PopStyleColor(3);
  if (start) {
    statusFresh_ = false;
    const fs::path root = steamvr_.root;
    InstallOptions options;
    options.monadoCheck = monadoCheck_;
    job_.Start([this, root, options, install](Job& j) {
      auto log = [&j](const std::string& line) { j.Log(line); };
      if (install) InstallDriver(root, options, log);
      else UninstallDriver(root, log);
      return true;
    });
  }
  Back(Page::Home);
}

// ---- menus ----

void Ui::FlashMenu() {
  Title("Flash Firmware");
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
  switch (ChoiceRow({"Flash 1.0 Lighthouse\n(lhbs) Firmware", "Flash Doorman\nFirmware"}, ImGui::GetFontSize() * 7)) {
    case 0: Go(Page::BaseStation); break;
    case 1: Go(Page::DoormanMenu); break;
  }
  Back(Page::Home);
}

void Ui::DoormanMenu() {
  Title("Flash Doorman Firmware");
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
  switch (ChoiceRow({"Flash Dongle\nFirmware", "Flash Tracker/Controller\nRadio Firmware"}, ImGui::GetFontSize() * 7)) {
    case 0: Go(Page::Dongle); break;
    case 1: Go(Page::RadioMenu); break;
  }
  Back(Page::FlashMenu);
}

void Ui::RadioMenu() {
  Title("Flash Tracker/Controller Radio Firmware");
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
  switch (ChoiceRow({"Wired (recommended)", "Wireless"}, ImGui::GetFontSize() * 7)) {
    case 0: Go(Page::RadioWired); break;
    case 1: Go(Page::RadioWireless); break;
  }
  Back(Page::DoormanMenu);
}

void Ui::RestoreRadioMenu() {
  Title("Restore Tracker/Controller Radio Firmware");
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
  switch (ChoiceRow({"Wired (recommended)", "Wireless"}, ImGui::GetFontSize() * 7)) {
    case 0: Go(Page::RestoreRadio); break;
    case 1: Go(Page::RestoreRadioWireless); break;
  }
  Back(Page::RestoreMenu);
}

void Ui::RestoreMenu() {
  Title("Restore Firmware");
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
  switch (ChoiceRow({"1.0 Base Station\nFirmware", "Tracker/Controller\nRadio Firmware"}, ImGui::GetFontSize() * 7)) {
    case 0: Go(Page::RestoreBase); break;
    case 1: Go(Page::RestoreRadioMenu); break;
  }
  Back(Page::Home);
}

// ---- 1.0 base station ----

void Ui::BaseStationPage(bool restore) {
  Title(restore ? "Restore 1.0 Base Station Firmware" : "Flash 1.0 Lighthouse (lhbs) Firmware");
  const Page back = restore ? Page::RestoreMenu : Page::FlashMenu;
  auto flashAgain = [&] {
    if (BigButton("Flash Again", ImGui::GetFontSize() * 12)) Go(restore ? Page::RestoreBase : Page::BaseStation);
    ImGui::SameLine();
  };
  switch (baseStep_) {
    case BaseStep::Warning: {
      Warning("ONLY for Lighthouse 1.0 base stations!",
              "This firmware is only for 1.0 base stations (the ones that came with the original HTC Vive).\n"
              "It WILL BRICK 2.0 base stations!");
      ImGui::Spacing();
      if (restore && (steamvr_.root.empty() || StockBaseStationImage(steamvr_.root).empty())) {
        ImGui::TextColored(kBad, steamvr_.root.empty() ? "SteamVR not found" : "SteamVR's base station firmware not found");
        if (steamvr_.root.empty() && BigButton("Choose SteamVR folder")) askSteamVR_ = true;
        Back(back);
        return;
      } else if (!restore && OurBaseStationImage().empty()) {
        ImGui::TextColored(kBad, "lhbs firmware not found in the firmwares folder");
        Back(back);
        return;
      }
      ImGui::Checkbox("This is a Lighthouse 1.0 base station", &confirmed10_);
      ImGui::BeginDisabled(!confirmed10_);
      if (BigButton("Continue", ImGui::GetFontSize() * 12)) {
        const fs::path image = restore ? StockBaseStationImage(steamvr_.root) : OurBaseStationImage();
        std::string error;
        if (!ReadFile(image, &baseImage_)) error = "couldn't read " + Utf8(image);
        else error = CheckBaseStationImage(baseImage_);
        baseImageName_ = Utf8(image.filename());
        if (error.empty()) {
          baseStep_ = BaseStep::Waiting;
          baseDrives_.Start(FindBaseStations, 1.0);
        } else {
          job_.Reset();
          job_.Fail(baseImageName_ + ": " + error);
          baseStep_ = BaseStep::Done;
        }
      }
      ImGui::EndDisabled();
      Back(back);
      return;
    }
    case BaseStep::Waiting:
    case BaseStep::Ready: {
      const auto drives = baseDrives_.Value();
      if (drives.empty()) {
        baseStep_ = BaseStep::Waiting;
        Step(1, "Unplug the base station's power cable");
        Step(2, "Hold the button on the back");
        Step(3, "Plug the micro-USB cable to your PC, then plug the power cable and let go of the button");
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "Waiting for the base station...");
      } else {
        baseStep_ = BaseStep::Ready;
        baseDrive_ = drives.front();
        ImGui::TextColored(kGood, "Base station found (%s)", baseDrive_.name.c_str());
        ImGui::Text("Firmware: %s", baseImageName_.c_str());
        ImGui::Spacing();
        if (BigButton("Flash", ImGui::GetFontSize() * 12)) {
          baseDrives_.Stop();
          baseStep_ = BaseStep::Writing;
          const Drive drive = baseDrive_;
          const std::string image = baseImage_;
          job_.Start([this, drive, image](Job& j) {
            alreadyInstalled_ = false;
            std::string current;
            if (ReadBaseStation(drive, &current) && current.compare(0, image.size(), image) == 0) {
              alreadyInstalled_ = true;
              Eject(drive);
              return true;
            }
            std::string error;
            if (!WriteBaseStation(drive, image, &error, [&](float f) { j.Status(f, "Writing"); })) { j.Fail(error); return false; }
            Eject(drive);
            return true;
          });
        }
      }
      Back(back);
      return;
    }
    case BaseStep::Writing: {
      JobLog(job_);
      if (job_.Running()) { ProgressBar(job_); return; }
      if (!job_.Ok() || alreadyInstalled_) { baseStep_ = BaseStep::Done; return; }
      baseStep_ = BaseStep::Replug;
      sawUnplug_ = false;
      baseDrives_.Start(FindBaseStations, 1.0);
      return;
    }
    case BaseStep::Replug: {
      JobLog(job_);
      ImGui::Spacing();
      ImGui::TextColored(kWarn, "Verifying:");
      Step(1, "Unplug the base station's power cable");
      Step(2, "Hold the button again and plug the power cable back in");
      const auto drives = baseDrives_.Value();
      if (drives.empty()) sawUnplug_ = true;
      if (sawUnplug_ && !drives.empty() && baseDrives_.Have()) {
        baseDrives_.Stop();
        baseStep_ = BaseStep::Verifying;
        const Drive drive = drives.front();
        const std::string image = baseImage_;
        job_.Start([drive, image](Job& j) {
          j.Status(0.5f, "Verifying");
          std::string check;
          const bool read = ReadBaseStation(drive, &check);
          Eject(drive);
          if (!read || check.compare(0, image.size(), image) != 0) {
            j.Fail("verification failed, try again");
            return false;
          }
          return true;
        });
      } else {
        ImGui::TextColored(kMuted, sawUnplug_ ? "Waiting for the base station..." : "Waiting for it to be unplugged...");
      }
      return;
    }
    case BaseStep::Verifying:
      if (job_.Running()) { ProgressBar(job_); return; }
      baseStep_ = BaseStep::Done;
      return;
    case BaseStep::Done: {
      JobLog(job_);
      ImGui::Spacing();
      if (job_.Ok()) {
        ImGui::PushFont(g_title);
        ImGui::TextColored(kGood, alreadyInstalled_ ? "This firmware is already installed." : restore ? "Stock firmware has been restored." : "lhbs has been installed.");
        ImGui::PopFont();
        ImGui::TextUnformatted("Unplug the USB cable and power cycle the base station.");
      } else {
        ImGui::TextColored(kBad, "Failed: %s", job_.Error().c_str());
      }
      ImGui::Spacing();
      flashAgain();
      if (ImGui::Button("< Home")) Go(Page::Home);
      return;
    }
  }
}

// ---- dongle ----

void Ui::DonglePage() {
  Title("Flash Dongle Firmware");
  switch (dongleStep_) {
    case DongleStep::Board: {
      ImGui::TextUnformatted("Board:");
      const auto watch = [this](bool holyiot) {
        dongleHolyiot_ = holyiot;
        dongleStep_ = DongleStep::Detect;
        dongleSeen_.Start([holyiot] {
          DongleSeen s;
          if (holyiot) s.ports = lh::dfu::FindBootloaderPorts();
          else s.drives = FindUf2Drives();
          for (auto& d : FindDoormanDongles())
            if (d.holyiot == holyiot) s.dongles.push_back(d);
          return s;
        }, 1.0);
      };
      if (BigButton("ProMicro nRF52840 (Adafruit Bootloader)", ImGui::GetFontSize() * 34)) watch(false);
      if (BigButton("Holyiot 21017 (Nordic Bootloader)", ImGui::GetFontSize() * 34)) watch(true);
      Back(Page::DoormanMenu);
      return;
    }
    case DongleStep::Detect: {
      const DongleSeen seen = dongleSeen_.Value();
      if (!DeviceAccess()) { Back(Page::DoormanMenu); return; }
      Step(1, "Plug the dongle in");
      Step(2, dongleHolyiot_ ? "If it doesn't show up, press its RESET button to put it in DFU mode"
                             : "If it doesn't show up, short the RST and GND pins twice to put it in DFU mode");
      ImGui::Spacing();
      if (seen.drives.empty() && seen.ports.empty() && seen.dongles.empty()) ImGui::TextColored(kMuted, "Waiting for the dongle...");
      // It restarts as a Doorman Dongle: check it answers.
      static const auto verify = [](Job& j) {
        for (int i = 0; i < 200; ++i) {
          j.Status(0.8f + 0.2f * float(i) / 200.f, "Verifying");
          for (const auto& d : FindDoormanDongles())
            if (d.firmware) {
              char text[96];
              std::snprintf(text, sizeof text, "Dongle %s is working (firmware %04X)", d.serial.c_str(), d.firmware);
              j.Log(text);
              j.Status(1, "Done");
              return true;
            }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        j.Fail("the dongle didn't show up after flashing, replug it and try again");
        return false;
      };
      static const auto flashUf2 = [](Job& j, const Uf2Drive& drive) {
        const fs::path image = DongleImage(drive.softDevice);
        j.Log("Firmware: " + Utf8(image.filename()));
        std::string error;
        if (!CopyUf2(drive, image, &error, [&](float f) { j.Status(f * 0.8f, "Flashing"); })) { j.Fail(error); return false; }
        return verify(j);
      };
      static const auto flashDfu = [](Job& j, const std::string& port) {
        const fs::path file = HolyiotImage();
        j.Log("Firmware: " + Utf8(file.filename()));
        lh::dfu::Image image;
        std::string error;
        // App space of Nordic's open bootloader: after the MBR, below the pairing page and the bootloader.
        if (!lh::dfu::LoadHex(Utf8(file), 0x1000, 0xDA000, &image, &error)) { j.Fail(error); return false; }
        if (image.base != 0x1000) { j.Fail("this firmware isn't built for this bootloader"); return false; }
        j.Log("Bootloader on " + port);
        if (!lh::dfu::Flash(port, image, [&](float f) { j.Status(f * 0.8f, "Flashing"); }, &error)) { j.Fail(error); return false; }
        return verify(j);
      };
      const auto start = [this](std::function<bool(Job&)> work) {
        dongleSeen_.Stop();
        dongleStep_ = DongleStep::Flashing;
        job_.Start(std::move(work));
      };
      for (const auto& d : seen.drives) {
        const std::string label = d.name + (d.board.empty() ? "" : "  (" + d.board + ")");
        if (BigButton(label.c_str(), ImGui::GetFontSize() * 34)) start([d](Job& j) { return flashUf2(j, d); });
      }
      for (const auto& port : seen.ports) {
        const std::string label = "Nordic DFU bootloader (" + port + ")";
        if (BigButton(label.c_str(), ImGui::GetFontSize() * 34)) start([port](Job& j) { return flashDfu(j, port); });
      }
      for (const auto& d : seen.dongles) {
        char label[128];
        std::snprintf(label, sizeof label, "Doorman Dongle %s", d.serial.c_str());
        if (!BigButton(label, ImGui::GetFontSize() * 34)) continue;
        // Restarts it in its update mode, then flashes what appears.
        const DoormanDongleInfo dongle = d;
        start([dongle](Job& j) {
          const auto drivesBefore = FindUf2Drives();
          const auto portsBefore = lh::dfu::FindBootloaderPorts();
          j.Status(0, "Rebooting into DFU mode");
          if (!RebootToBootloader(dongle)) { j.Fail("couldn't open the dongle, close SteamVR and doorman-dongle-manager"); return false; }
          for (int i = 0; i < 150; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (dongle.holyiot) {
              for (const auto& port : lh::dfu::FindBootloaderPorts())
                if (std::find(portsBefore.begin(), portsBefore.end(), port) == portsBefore.end()) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the port settle
                  return flashDfu(j, port);
                }
            } else {
              for (const auto& drive : FindUf2Drives())
                if (std::none_of(drivesBefore.begin(), drivesBefore.end(), [&](const Uf2Drive& b) { return b.mount == drive.mount; }))
                  return flashUf2(j, drive);
            }
          }
          j.Fail(dongle.holyiot ? "the dongle didn't enter DFU mode, press its RESET button and try again"
                                : "the dongle didn't enter DFU mode, short RST and GND twice and try again");
          return false;
        });
      }
      Back(Page::DoormanMenu);
      return;
    }
    case DongleStep::Flashing:
      JobLog(job_);
      if (job_.Running()) { ProgressBar(job_); return; }
      dongleStep_ = DongleStep::Done;
      return;
    case DongleStep::Done:
      JobLog(job_);
      ImGui::Spacing();
      if (job_.Ok()) {
        ImGui::PushFont(g_title);
        ImGui::TextColored(kGood, "Flashed!");
        ImGui::PopFont();
      } else {
        ImGui::TextColored(kBad, "Failed: %s", job_.Error().c_str());
      }
      ImGui::Spacing();
      if (BigButton("Flash Again", ImGui::GetFontSize() * 12)) Go(Page::Dongle);
      ImGui::SameLine();
      if (ImGui::Button("< Home")) Go(Page::Home);
      return;
  }
}

// ---- radios, wired ----

void Ui::RadioWiredPage(bool restore) {
  Title(restore ? "Restore Tracker/Controller Radio Firmware" : "Flash Radio Firmware: Wired");
  const Page back = restore ? Page::RestoreRadioMenu : Page::RadioMenu;
  if (steamvr_.root.empty()) {
    ImGui::TextColored(kBad, "SteamVR not found");
    if (BigButton("Choose SteamVR folder")) askSteamVR_ = true;
    Back(back);
    return;
  }
  if (radioStep_ != RadioStep::Choose) {
    JobLog(job_);
    if (job_.Running()) {
      char text[64];
      std::snprintf(text, sizeof text, "Device %d of %d", current_.load(), total_);
      ImGui::TextColored(kWarn, "%s", text);
      ProgressBar(job_);
      return;
    }
    ImGui::Spacing();
    if (job_.Ok()) {
      ImGui::PushFont(g_title);
      ImGui::TextColored(kGood, "Flashed!");
      ImGui::PopFont();
      ImGui::TextUnformatted("Unplug the devices and turn them on.");
    } else {
      ImGui::TextColored(kBad, "Failed: %s", job_.Error().c_str());
    }
    ImGui::Spacing();
    if (BigButton("Flash Again", ImGui::GetFontSize() * 12)) Go(restore ? Page::RestoreRadio : Page::RadioWired);
    ImGui::SameLine();
    if (ImGui::Button("< Home")) Go(Page::Home);
    return;
  }
  ImGui::TextUnformatted("Plug your devices in with a USB cable and select the ones to flash.");
  if (!RuntimesClosed() || !DeviceAccess()) { Back(back); return; }
  const auto devices = wired_.Value();
  ImGui::Spacing();
  ImGui::BeginChild("wired", ImVec2(0, -DeviceListReserve()), ImGuiChildFlags_Borders);
  if (devices.empty()) ImGui::TextColored(kMuted, "Waiting for devices...");
  int trackers10 = 0;
  for (const auto& d : devices) {
    trackers10 += d.model.family == RadioFamily::Tracker10;
    const bool supported = d.model.family != RadioFamily::Unsupported;
    const fs::path image = restore ? StockRadioImage(steamvr_.root, d.model.hwid) : OurRadioImage(d.model.family);
    const bool haveImage = supported && !image.empty();
    const bool allowed = d.model.known || accepted_.count(d.serial);
    if (!allowed) checked_.erase(d.serial);
    bool on = checked_.count(d.serial) > 0;
    ImGui::BeginDisabled(!haveImage || !allowed);
    const std::string label = d.model.name + "  " + d.serial;
    const RadioState radio = DescribeRadio(d.radioVersion, d.model.family);
    if (radio.color) ImGui::PushStyleColor(ImGuiCol_Text, *radio.color);
    if (ImGui::Checkbox(label.c_str(), &on)) { if (on) checked_.insert(d.serial); else checked_.erase(d.serial); }
    if (radio.color) ImGui::PopStyleColor();
    if (d.model.known && supported) { ImGui::SameLine(); ImGui::TextColored(radio.color ? *radio.color : kMuted, "%s", radio.text.c_str()); }
    ImGui::EndDisabled();
    if (!d.model.known && haveImage) UnknownDevice(d.model.hwid, d.model.family, d.serial, &accepted_);
    else if (!d.model.known) { ImGui::SameLine(); ImGui::TextColored(kMuted, "(unknown device %08X, no firmware to restore)", d.model.hwid); }
    else if (!supported) { ImGui::SameLine(); ImGui::TextColored(kMuted, "(not supported)"); }
    else if (!haveImage) { ImGui::SameLine(); ImGui::TextColored(kMuted, "(firmware not found)"); }
  }
  ImGui::EndChild();
  std::vector<WiredDevice> chosen;
  for (const auto& d : devices) if (checked_.count(d.serial)) chosen.push_back(d);
  const int chosen10 = int(std::count_if(chosen.begin(), chosen.end(), [](const WiredDevice& d) { return d.model.family == RadioFamily::Tracker10; }));
  if (trackers10 > 1 && chosen10 > 0)
    ImGui::TextColored(kWarn, "Only one VIVE Tracker 1.0 or VIVE Controller can be plugged in while flashing");
  ImGui::BeginDisabled(chosen.empty() || (trackers10 > 1 && chosen10 > 0));
  if (BigButton("Flash", ImGui::GetFontSize() * 12)) {
    wired_.Stop();
    radioStep_ = RadioStep::Flashing;
    total_ = int(chosen.size());
    current_ = 0;
    const fs::path root = steamvr_.root;
    job_.Start([this, chosen, root, restore](Job& j) {
      bool all = true;
      for (const auto& d : chosen) {
        ++current_;
        const fs::path image = restore ? StockRadioImage(root, d.model.hwid) : OurRadioImage(d.model.family);
        std::string error;
        const bool ok = FlashWired(root, d, image, [&](float f, const std::string& s) { j.Status(f, d.serial + ": " + s); }, &error);
        j.Log(d.model.name + " " + d.serial + (ok ? ": done" : ": failed (" + error + ")"));
        all = all && ok;
        if (!ok) j.Fail(d.serial + ": " + error);
      }
      return all;
    });
  }
  ImGui::EndDisabled();
  Back(back);
}

// ---- radios, wireless ----

void Ui::RadioWirelessPage(bool restore) {
  Title(restore ? "Restore Radio Firmware: Wireless" : "Flash Radio Firmware: Wireless");
  const Page back = restore ? Page::RestoreRadioMenu : Page::RadioMenu;
  if (restore && steamvr_.root.empty()) {
    ImGui::TextColored(kBad, "SteamVR not found");
    if (BigButton("Choose SteamVR folder")) askSteamVR_ = true;
    Back(back);
    return;
  }
  // The stock image for a device: by its hardware id. Behind a Doorman Dongle the user picks it
  // only when the device didn't say which it is.
  auto stockFor = [this](const WirelessDevice& d) -> fs::path {
    if (d.route == Route::Watchman || !IsDoormanRadio(d.model.hwid)) return StockRadioImage(steamvr_.root, d.model.hwid);
    const auto c = stockChoice_.find(d.Key());
    if (c != stockChoice_.end()) return stockModels_[size_t(c->second)].image;
    std::vector<int> fits;
    for (size_t i = 0; i < stockModels_.size(); ++i) if (stockModels_[i].family == d.model.family) fits.push_back(int(i));
    return fits.size() == 1 ? stockModels_[size_t(fits[0])].image : fs::path();
  };
  if (radioStep_ != RadioStep::Choose) {
    JobLog(job_);
    if (job_.Running()) {
      char text[64];
      std::snprintf(text, sizeof text, "Device %d of %d", current_.load(), total_);
      ImGui::TextColored(kWarn, "%s", text);
      ProgressBar(job_);
      return;
    }
    ImGui::Spacing();
    if (job_.Ok()) {
      ImGui::PushFont(g_title);
      ImGui::TextColored(kGood, "Flashed!");
      ImGui::PopFont();
      ImGui::TextUnformatted(restore ? "Turn the devices back on. Pair them to their original dongle again." : "Turn the devices back on. You can pair them to a Doorman Dongle now.");
    } else {
      ImGui::TextColored(kBad, "Failed: %s", job_.Error().c_str());
    }
    ImGui::Spacing();
    if (BigButton("Flash Again", ImGui::GetFontSize() * 12)) Go(restore ? Page::RestoreRadioWireless : Page::RadioWireless);
    ImGui::SameLine();
    if (ImGui::Button("< Home")) Go(Page::Home);
    return;
  }
  ImGui::TextUnformatted("Turn on your devices. If they aren't paired, pair them to one of your dongles first.");
  if (!RuntimesClosed() || !DeviceAccess()) { Back(back); return; }
  const auto devices = wireless_.Value();
  ImGui::Spacing();
  ImGui::BeginChild("wireless", ImVec2(0, -DeviceListReserve()), ImGuiChildFlags_Borders);
  if (devices.empty()) ImGui::TextColored(kMuted, "Waiting for devices...");
  for (const auto& d : devices) {
    const bool supported = d.model.family != RadioFamily::Unsupported && !OurRadioImage(d.model.family).empty();
    const bool needsPick = restore && d.route == Route::Doorman && stockFor(d).empty();
    const bool unknownOk = !d.model.known && !restore && supported;
    const bool allowed = d.model.known || (unknownOk && accepted_.count(d.Key()));
    if (!allowed) checked_.erase(d.Key());
    bool on = checked_.count(d.Key()) > 0;
    ImGui::BeginDisabled(!supported || !allowed);
    const std::string label = d.model.name + "  " + d.serial + "  (" + (d.route == Route::Doorman ? "Doorman Dongle " : "dongle ") + d.dongle + ")";
    const RadioState radio = DescribeRadio(d.radioVersion, d.model.family);
    if (radio.color) ImGui::PushStyleColor(ImGuiCol_Text, *radio.color);
    if (ImGui::Checkbox(label.c_str(), &on)) { if (on) checked_.insert(d.Key()); else checked_.erase(d.Key()); }
    if (radio.color) ImGui::PopStyleColor();
    if (d.model.known && supported) { ImGui::SameLine(); ImGui::TextColored(radio.color ? *radio.color : kMuted, "%s", radio.text.c_str()); }
    ImGui::EndDisabled();
    if (unknownOk) UnknownDevice(d.model.hwid, d.model.family, d.Key(), &accepted_);
    else if (!d.model.known) { ImGui::SameLine(); ImGui::TextColored(kMuted, "(unknown device %08X)", d.model.hwid); }
    else if (!supported) { ImGui::SameLine(); ImGui::TextColored(kMuted, "(not supported)"); }
    if (restore && supported && d.model.known && d.route == Route::Doorman && IsDoormanRadio(d.model.hwid)) {
      ImGui::SameLine();
      const auto c = stockChoice_.find(d.Key());
      const std::string current = c != stockChoice_.end() ? stockModels_[size_t(c->second)].name : needsPick ? "Device type..." : "";
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
      if (ImGui::BeginCombo(("##type" + d.Key()).c_str(), current.c_str())) {
        for (size_t i = 0; i < stockModels_.size(); ++i)
          if (stockModels_[i].family == d.model.family && ImGui::Selectable(stockModels_[i].name.c_str())) stockChoice_[d.Key()] = int(i);
        ImGui::EndCombo();
      }
    } else if (restore && supported && stockFor(d).empty()) {
      ImGui::SameLine();
      ImGui::TextColored(kMuted, "(firmware not found)");
    }
  }
  ImGui::EndChild();
  std::vector<std::pair<WirelessDevice, fs::path>> chosen;
  for (const auto& d : devices)
    if (checked_.count(d.Key())) chosen.emplace_back(d, restore ? stockFor(d) : OurRadioImage(d.model.family));
  const bool missing = std::any_of(chosen.begin(), chosen.end(), [](const auto& c) { return c.second.empty(); });
  if (missing) ImGui::TextColored(kWarn, "Pick the device type for each selected device");
  ImGui::BeginDisabled(chosen.empty() || missing);
  if (BigButton("Flash", ImGui::GetFontSize() * 12)) {
    wireless_.Stop();
    radioStep_ = RadioStep::Flashing;
    total_ = int(chosen.size());
    current_ = 0;
    job_.Start([this, chosen](Job& j) {
      bool all = true;
      for (const auto& [d, file] : chosen) {
        ++current_;
        RadioImage image;
        std::string error;
        bool ok = LoadRadioImage(file, &image, &error) &&
                  FlashWireless(d, image, [&](float f, const std::string& s) { j.Status(f, d.serial + ": " + s); }, &error);
        j.Log(d.model.name + " " + d.serial + (ok ? ": done" : ": failed (" + error + ")"));
        all = all && ok;
        if (!ok) j.Fail(d.serial + ": " + error);
      }
      return all;
    });
  }
  ImGui::EndDisabled();
  Back(back);
}

}  // namespace openhouse::installer
