#include "ui.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "lh/telemetry.hpp"
#include "lh/version.hpp"
#include "update_check.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace openhouse::dongle {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

ImFont* g_title = nullptr;
ImFont* g_huge = nullptr;

// openhouse-overlay's colours
const ImVec4 kGood{0.36f, 0.80f, 0.45f, 1}, kWarn{1.00f, 0.78f, 0.30f, 1}, kBad{1.00f, 0.42f, 0.40f, 1},
    kInfo{0.45f, 0.70f, 1.00f, 1}, kOff{0.55f, 0.58f, 0.62f, 1}, kAccent{1.00f, 0.86f, 0.55f, 1};

fs::path SettingsPath() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  return fs::path(base ? base : ".") / "lh" / "dongle.json";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "lh" / "dongle.json";
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : ".") / ".config" / "lh" / "dongle.json";
#endif
}

ImU32 U32(const ImVec4& c, float alpha = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

// A status dot; `frame`: centred on a frame-height row (after AlignTextToFramePadding).
void Dot(const ImVec4& color, float radius = 5, bool frame = false) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = frame ? ImGui::GetFrameHeight() : ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, p.y + h / 2), radius, U32(color));
  ImGui::Dummy(ImVec2(radius * 2 + 4, frame ? ImGui::GetTextLineHeight() : h));
  ImGui::SameLine();
}

void Title(const char* text) {
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
}

void Tip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

bool BigButton(const char* label, float width = 0) { return ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.25f)); }

// A button tinted with a colour: the main action, or a dangerous one.
bool TintButton(const char* label, const ImVec4& c, float width = 0) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 0.6f, c.y * 0.6f, c.z * 0.6f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.75f, c.y * 0.75f, c.z * 0.75f, 1));
  const bool r = BigButton(label, width);
  ImGui::PopStyleColor(3);
  return r;
}

bool Confirm(const char* id, const char* text) {
  bool yes = false;
  if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const float w = ImGui::GetFontSize() * 7, h = ImGui::GetFrameHeight() * 1.5f;
    if (ImGui::Button("Yes", ImVec2(w, h))) { yes = true; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return yes;
}

// A summary tile: a big value and what it is.
void Tile(const char* id, const char* value, const char* label, const ImVec4& color, float width) {
  const ImGuiStyle& st = ImGui::GetStyle();
  ImGui::PushFont(g_huge);
  const float big = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const float h = big + ImGui::GetTextLineHeight() + st.ItemSpacing.y + st.WindowPadding.y * 2 + 2;
  ImGui::BeginChild(id, ImVec2(width, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  ImGui::PushFont(g_huge);
  ImGui::TextColored(color, "%s", value);
  ImGui::PopFont();
  ImGui::TextDisabled("%s", label);
  ImGui::EndChild();
}

ImVec4 LossColor(double loss) {
  if (loss < 0) return kOff;
  return loss < 3 ? kGood : loss < 10 ? kWarn : kBad;
}

ImVec4 RssiColor(int rssi) {
  if (!rssi) return kOff;
  return rssi > -65 ? kGood : rssi > -78 ? kWarn : kBad;
}

const char* SignalWord(int rssi) {
  if (!rssi) return "-";
  return rssi > -55 ? "excellent" : rssi > -65 ? "good" : rssi > -78 ? "fair" : "weak";
}

// Signal bars (4), coloured by strength.
void SignalBars(int rssi) {
  const int bars = !rssi ? 0 : rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -78 ? 2 : 1;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight(), w = h * 0.22f, gap = h * 0.12f;
  for (int i = 0; i < 4; ++i) {
    const float bh = h * (0.35f + 0.65f * (i + 1) / 4);
    const ImVec2 a(p.x + i * (w + gap), p.y + h - bh), b(a.x + w, p.y + h);
    dl->AddRectFilled(a, b, i < bars ? U32(RssiColor(rssi)) : U32(kOff, 0.3f), 1);
  }
  ImGui::Dummy(ImVec2(4 * (w + gap), h));
}

// A small graph with its current value written on it.
void Graph(const char* id, const std::deque<float>& h, float lo, float hi, const ImVec4& color, const char* overlay,
           float width, float height) {
  std::vector<float> v(h.begin(), h.end());
  if (v.empty()) v.push_back(lo);
  ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.07f, 0.09f, 1));
  ImGui::PlotLines(id, v.data(), int(v.size()), 0, overlay, lo, hi, ImVec2(width, height));
  ImGui::PopStyleColor(2);
}

std::string Uptime(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char t[48];
  if (s >= 3600) std::snprintf(t, sizeof t, "%uh %02um", s / 3600, s / 60 % 60);
  else std::snprintf(t, sizeof t, "%um %02us", s / 60, s % 60);
  return t;
}

// Folders of mounted UF2 drives (the dongle's update mode shows up as one).
std::vector<fs::path> Uf2Drives() {
  std::vector<fs::path> out;
  std::error_code ec;
#ifdef _WIN32
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i)
    if (mask & (1u << i)) {
      const fs::path root = std::string(1, char('A' + i)) + ":\\";
      if (fs::exists(root / "INFO_UF2.TXT", ec)) out.push_back(root);
    }
#else
  const char* user = std::getenv("USER");
  std::vector<fs::path> bases = {"/media", "/mnt"};
  if (user) { bases.push_back(fs::path("/run/media") / user); bases.push_back(fs::path("/media") / user); }
  for (const auto& base : bases) {
    if (!fs::is_directory(base, ec)) continue;
    for (const auto& e : fs::directory_iterator(base, ec))
      if (fs::exists(e.path() / "INFO_UF2.TXT", ec)) out.push_back(e.path());
  }
#endif
  return out;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
Ui::Ui() : driver_(lh::telemetry::kDefaultPort) {
  std::ifstream in(SettingsPath());
  const json j = json::parse(in, nullptr, false);
  // Kept in a variable: items() refers to the object it came from.
  const json devices = j.is_object() ? j.value("devices", json::object()) : json::object();
  if (devices.is_object())
    for (const auto& [id, v] : devices.items())
      known_[id] = {v.value("serial", ""), v.value("model", ""), v.value("nickname", "")};
  if (const char* page = std::getenv("OPENHOUSE_DONGLE_PAGE")) {   // testing, with the snapshot
    const std::string p = page;
    page_ = p == "slots" ? Page::Slots : p == "radio" ? Page::Radio : p == "channels" ? Page::Channels
          : p == "firmware" ? Page::Firmware : p == "about" ? Page::About : Page::Overview;
  }
}

Ui::~Ui() {
  if (updater_.joinable()) updater_.join();
}

void Ui::SaveSettings() {
  json devices = json::object();
  for (const auto& [id, k] : known_) devices[id] = {{"serial", k.serial}, {"model", k.model}, {"nickname", k.nickname}};
  std::error_code ec;
  fs::create_directories(SettingsPath().parent_path(), ec);
  std::ofstream(SettingsPath()) << json{{"devices", devices}}.dump(2) << "\n";
  knownDirty_ = false;
}

void Ui::SetUpImGui(int k) {
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.Fonts->Clear();
  ImFontConfig font;
  font.OversampleH = font.OversampleV = 1;
  font.PixelSnapH = true;
  font.SizePixels = 13.f * k;
  io.Fonts->AddFontDefault(&font);
  font.SizePixels = 13.f * (k + 1);
  g_title = io.Fonts->AddFontDefault(&font);
  font.SizePixels = 13.f * (k + 2);
  g_huge = io.Fonts->AddFontDefault(&font);
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0;
  style.ChildRounding = 4;
  style.FrameRounding = 3;
  style.FramePadding = ImVec2(8, 6);
  style.ItemSpacing = ImVec2(10, 8);
  style.WindowPadding = ImVec2(10, 10);
  style.ScaleAllSizes(float(k));
}

std::shared_ptr<Dongle> Ui::Current() {
  const auto& all = manager_.Dongles();
  for (const auto& d : all)
    if (d->Serial() == dongle_) return d;
  if (all.empty()) return nullptr;
  dongle_ = all.front()->Serial();
  return all.front();
}

void Ui::Toast(const std::string& text, bool error) {
  toast_ = text;
  toastError_ = error;
  toastUntil_ = now_ + 6;
}

Dongle::Done Ui::Report(const std::string& what) {
  return [this, what](bool ok, const std::string& message) {
    std::lock_guard lock(resultsMutex_);
    results_.emplace_back(ok ? message : what + " failed: " + message, ok);
  };
}

std::string Ui::DeviceLabel(const SlotState& s) const {
  if (!s.nickname.empty()) return s.nickname;
  if (!s.id.model.empty() && !s.id.serial.empty()) return s.id.model + "  " + s.id.serial;
  if (!s.id.serial.empty()) return s.id.serial;
  return s.used ? "Unknown device" : "Empty";
}

// What we learn about a slot's device is remembered by its pairing id, and given back to
// every dongle (a device keeps its id when paired elsewhere).
void Ui::Remember(const DongleState& d) {
  for (const auto& s : d.slots) {
    if (s.pairId.empty() || s.id.serial.empty()) continue;
    auto& k = known_[s.pairId];
    if (k.serial != s.id.serial || (!s.id.model.empty() && k.model != s.id.model)) {
      k.serial = s.id.serial;
      if (!s.id.model.empty()) k.model = s.id.model;
      knownDirty_ = true;
    }
  }
}

void Ui::Poll(double now) {
  now_ = now;
  manager_.Poll(Now());
  driver_.Poll(now);
  {
    std::lock_guard lock(resultsMutex_);
    for (auto& [text, ok] : results_) Toast(text, !ok);
    results_.clear();
  }
  // Names: the driver's (it knows which device is on which slot), then what we remember.
  std::map<std::string, std::string> driverModel;
  if (driver_.Connected())
    for (const auto& dev : driver_.Current().devices) driverModel[dev.serial] = dev.model;
  for (const auto& d : manager_.Dongles()) {
    const DongleState st = d->Snapshot();
    Remember(st);
    for (int i = 0; i < kSlots; ++i) {
      const auto& s = st.slots[i];
      if (s.pairId.empty()) continue;
      Identity id;
      if (driver_.Connected())
        for (const auto& [serial, connected] : driver_.Current().receivers)
          if (serial == st.serial + "/" + std::to_string(i) && !connected.empty()) {
            id = {connected, driverModel.count(connected) ? driverModel[connected] : "", "driver"};
          }
      const auto k = known_.find(s.pairId);
      if (id.serial.empty() && k != known_.end() && !k->second.serial.empty())
        id = {k->second.serial, k->second.model, "remembered"};
      if (!id.serial.empty() && id.serial != s.id.serial) d->SetKnown(s.pairId, id);
      if (k != known_.end() && k->second.nickname != s.nickname) d->SetNickname(i, k->second.nickname);
    }
  }
  if (knownDirty_) SaveSettings();
}

// ---- layout ----

void Ui::Draw(float width, float height) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::Begin("openhouse dongle", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
  const float statusHeight = ImGui::GetFrameHeightWithSpacing() + 4;
  const float body = ImGui::GetContentRegionAvail().y - statusHeight;
  Sidebar(body);
  ImGui::SameLine();
  ImGui::BeginChild("page", ImVec2(0, body), ImGuiChildFlags_None);
  const auto dongle = Current();
  if (page_ == Page::About) About();
  else if (!dongle) NoDongle();
  else {
    const DongleState d = dongle->Snapshot();
    if (!d.open && !d.error.empty()) {
      Title(d.serial.c_str());
      ImGui::TextColored(kBad, "Can't use this dongle: %s", d.error.c_str());
    } else {
      switch (page_) {
        case Page::Overview: Overview(d); break;
        case Page::Slots: Slots(d); break;
        case Page::Radio: Radio(d); break;
        case Page::Channels: Channels(d); break;
        case Page::Firmware: Firmware(d); break;
        case Page::About: break;
      }
    }
  }
  ImGui::EndChild();
  StatusBar();
  ImGui::End();
}

void Ui::Sidebar(float height) {
  ImGui::PushFont(g_title);
  const float name = ImGui::CalcTextSize("openhouse").x;
  ImGui::PopFont();
  const float logoSide = ImGui::GetFontSize() * 2.6f;
  const float w = std::max(ImGui::GetFontSize() * 11, logoSide + name + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().WindowPadding.x * 2 + 4);
  ImGui::BeginChild("sidebar", ImVec2(w, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  if (logo_) {
    ImGui::Image(static_cast<ImTextureID>(logo_), ImVec2(logoSide, logoSide));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted("openhouse");
  ImGui::PopFont();
  ImGui::TextDisabled("dongle manager");
  ImGui::EndGroup();
  ImGui::Spacing();
  // which dongle (more than one can be plugged in)
  const auto& all = manager_.Dongles();
  if (all.size() > 1) {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##dongle", dongle_.c_str())) {
      for (const auto& d : all)
        if (ImGui::Selectable(d->Serial().c_str(), d->Serial() == dongle_)) dongle_ = d->Serial();
      ImGui::EndCombo();
    }
  } else if (all.size() == 1) {
    ImGui::TextDisabled("%s", all.front()->Serial().c_str());
  }
  ImGui::Separator();
  ImGui::Spacing();
  struct Item { Page page; const char* label; };
  static const Item items[] = {{Page::Overview, "Overview"}, {Page::Slots, "Slots"},     {Page::Radio, "Radio"},
                               {Page::Channels, "Channels"}, {Page::Firmware, "Firmware"}, {Page::About, "About"}};
  const float rowHeight = ImGui::GetFrameHeight() * 1.15f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 6));
  for (const auto& item : items) {
    const bool selected = page_ == item.page;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(selected ? ImGuiCol_ButtonActive : ImGuiCol_Button));
    if (ImGui::Button(item.label, ImVec2(-1, rowHeight))) page_ = item.page;
    ImGui::PopStyleColor();
  }
  ImGui::PopStyleVar();
  // the dongle and the driver, at the bottom
  const float line = ImGui::GetTextLineHeightWithSpacing();
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - line * 3.4f));
  ImGui::Separator();
  const auto d = Current();
  const bool open = d && d->Snapshot().open;
  Dot(open ? kGood : kBad);
  ImGui::TextUnformatted(open ? "Dongle connected" : "No dongle");
  Dot(driver_.Connected() ? kGood : kOff);
  ImGui::TextUnformatted(driver_.Connected() ? "Driver running" : "Driver not running");
  Tip(driver_.Connected() ? "Device names come from the running openhouse driver"
                          : "The openhouse driver isn't running: names come from what this app remembers, "
                            "or from reading the device (Identify)");
  ImGui::EndChild();
}

void Ui::StatusBar() {
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  if (!toast_.empty() && now_ < toastUntil_) ImGui::TextColored(toastError_ ? kBad : kGood, "%s", toast_.c_str());
  else ImGui::TextDisabled("openhouse %s", lh::kVersion);
}

void Ui::NoDongle() {
  Title("No openhouse dongle");
  ImGui::TextWrapped("Plug in an openhouse dongle. It shows up here within a couple of seconds.");
#ifndef _WIN32
  ImGui::Spacing();
  ImGui::TextDisabled("On Linux it needs access to /dev/hidraw*: the openhouse udev rule gives it (see the README).");
#endif
}

// The pairing banner: a countdown while the dongle is pairing, else the button.
void Ui::PairBar(const DongleState& d) {
  const auto dongle = Current();
  if (d.pairing) {
    const double left = d.pairUntil > 0 ? std::max(0.0, d.pairUntil - Now()) : -1;
    char text[128];
    std::snprintf(text, sizeof text, "Pairing into %s%s",
                  d.pairTarget >= 0 ? ("slot " + std::to_string(d.pairTarget)).c_str() : "the first free slot",
                  left >= 0 ? ("  -  " + std::to_string(int(left)) + " s left").c_str() : "");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.18f, 0.28f, 1));
    ImGui::BeginChild("pairing", ImVec2(0, ImGui::GetFrameHeight() * 2.6f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    Dot(kInfo, 6);
    ImGui::PushFont(g_title);
    ImGui::TextColored(kInfo, "%s", text);
    ImGui::PopFont();
    ImGui::TextDisabled("Hold the device's power button until its light blinks: it joins by itself.");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 9);
    if (ImGui::Button("Stop pairing", ImVec2(ImGui::GetFontSize() * 9, 0))) dongle->StopPairing(Report("Stop pairing"));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return;
  }
  int firstFree = -1;
  for (int i = 0; i < kSlots && firstFree < 0; ++i)
    if (!d.slots[i].used) firstFree = i;
  if (firstFree >= 0) {
    if (TintButton("Pair a device", kInfo, ImGui::GetFontSize() * 12)) dongle->Pair(60, -1, Report("Pairing"));
    Tip("60 s of pairing into the first free slot");
  } else {
    ImGui::BeginDisabled();
    BigButton("All slots paired", ImGui::GetFontSize() * 12);
    ImGui::EndDisabled();
    Tip("Pair into a slot from its card: that replaces the device on it");
  }
}

// ---- pages ----

void Ui::Overview(const DongleState& d) {
  Title("Overview");
  PairBar(d);
  ImGui::Spacing();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float tw = (ImGui::GetContentRegionAvail().x - gap * 3) / 4;
  double loss = 0;
  int n = 0, worstRssi = 0;
  for (const auto& s : d.slots)
    if (s.active) {
      if (s.lossPercent >= 0) { loss += s.lossPercent; n++; }
      if (s.rssiDongle && (!worstRssi || s.rssiDongle < worstRssi)) worstRssi = s.rssiDongle;
    }
  char v[32];
  std::snprintf(v, sizeof v, "%d / %d", d.joined, kSlots);
  Tile("t1", v, "devices connected", d.joined ? kGood : kOff, tw);
  ImGui::SameLine();
  std::snprintf(v, sizeof v, "%d", d.paired);
  Tile("t2", v, "slots paired", kInfo, tw);
  ImGui::SameLine();
  std::snprintf(v, sizeof v, "%.1f kB/s", d.kBytesPerSec);
  Tile("t3", v, "data from devices", kAccent, tw);
  ImGui::SameLine();
  if (n) std::snprintf(v, sizeof v, "%.1f %%", loss / n); else std::snprintf(v, sizeof v, "-");
  Tile("t4", v, "radio loss (resent)", n ? LossColor(loss / n) : kOff, tw);

  ImGui::Spacing();
  ImGui::SeparatorText("Slots");
  if (ImGui::BeginTable("slots", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
    ImGui::TableSetupColumn("Device");
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
    ImGui::TableSetupColumn("Loss", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
    ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
    ImGui::TableHeadersRow();
    for (int i = 0; i < kSlots; ++i) {
      const auto& s = d.slots[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%d", i);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.used) ImGui::TextUnformatted(DeviceLabel(s).c_str()); else ImGui::TextDisabled("Empty");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (!s.used) ImGui::TextDisabled("-");
      else { Dot(s.active ? kGood : kOff, 5, true); ImGui::TextColored(s.active ? kGood : kOff, s.active ? "Connected" : "Off"); }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.active) { SignalBars(s.rssiDongle); ImGui::SameLine(); ImGui::TextColored(RssiColor(s.rssiDongle), "%d dBm", s.rssiDongle); }
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.active && s.lossPercent >= 0) ImGui::TextColored(LossColor(s.lossPercent), "%.1f %%", s.lossPercent); else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (s.active) ImGui::Text("%.1f kB/s", s.kBytesPerSec); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  int on = 0;
  for (int k = 0; k < kHops; ++k) on += (d.channelMask >> k) & 1;
  ImGui::TextDisabled("Radio on %d of %d channels, now at %d MHz  |  firmware %d  |  up %s", on, kHops, 2400 + d.currentChannel,
                      d.firmware, Uptime(d.uptimeMs).c_str());
}

void Ui::SlotCard(const DongleState& d, int i, float width) {
  const auto dongle = Current();
  const auto& s = d.slots[i];
  const ImVec4 edge = !s.used ? kOff : s.active ? kGood : kWarn;
  ImGui::PushID(i);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(edge.x, edge.y, edge.z, s.used ? 0.7f : 0.25f));
  // header (title font) + separator + four lines + the buttons
  ImGui::PushFont(g_title);
  const float titleH = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const ImGuiStyle& st = ImGui::GetStyle();
  const float cardH = st.WindowPadding.y * 2 + titleH + st.ItemSpacing.y * 7 + ImGui::GetTextLineHeight() * 4 +
                      ImGui::GetFrameHeight() * 1.25f + 2;
  ImGui::BeginChild("card", ImVec2(width, cardH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  // header: slot number and state
  ImGui::PushFont(g_title);
  ImGui::Text("Slot %d", i);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 7);
  Dot(edge);
  ImGui::TextColored(edge, "%s", !s.used ? "Empty" : s.active ? "Connected" : "Off");
  ImGui::Separator();
  if (!s.used) {
    ImGui::TextDisabled("Nothing paired");
    ImGui::Spacing();
    ImGui::BeginDisabled(d.pairing);
    if (TintButton("Pair to this slot", kInfo, -1)) dongle->Pair(60, i, Report("Pairing"));
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopID();
    return;
  }
  ImGui::TextUnformatted(DeviceLabel(s).c_str());
  if (!s.nickname.empty() && !s.id.serial.empty()) ImGui::TextDisabled("%s %s", s.id.model.c_str(), s.id.serial.c_str());
  else ImGui::TextDisabled("radio id %s%s", s.pairId.c_str(), s.id.source.empty() ? "" : ("  (" + s.id.source + ")").c_str());
  if (s.active) {
    SignalBars(s.rssiDongle);
    ImGui::SameLine();
    ImGui::TextColored(RssiColor(s.rssiDongle), "%d dBm %s", s.rssiDongle, SignalWord(s.rssiDongle));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextColored(LossColor(s.lossPercent), "%.1f %% loss", std::max(0.0, s.lossPercent));
    ImGui::Text("%.1f kB/s   IMU %.0f Hz   light %.0f/s", s.kBytesPerSec, s.imuHz, s.lightPerSec);
  } else {
    ImGui::TextDisabled("Turned off or out of range");
    ImGui::TextDisabled(" ");
  }
  // actions
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() * 1.25f - ImGui::GetStyle().WindowPadding.y);
  const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
  ImGui::BeginDisabled(d.pairing);
  if (BigButton("Replace", bw)) { pendingSlot_ = i; ImGui::OpenPopup("Replace?"); }
  ImGui::EndDisabled();
  Tip("Pair another device into this slot: the one on it now is forgotten");
  ImGui::SameLine();
  if (TintButton("Unpair", kBad, bw)) { pendingSlot_ = i; ImGui::OpenPopup("Unpair?"); }
  Tip("Forget this device: it can't connect until it's paired again");
  ImGui::SameLine();
  if (BigButton("More...", bw)) {
    pendingSlot_ = i;
    std::snprintf(rename_, sizeof rename_, "%s", s.nickname.c_str());
    ImGui::OpenPopup("slot menu");
  }
  if (ImGui::BeginPopup("slot menu")) {
    ImGui::BeginDisabled(!s.active);
    if (ImGui::MenuItem("Turn off")) dongle->PowerOff(i, Report("Turn off"));
    if (ImGui::MenuItem("Identify (read its name)")) dongle->Identify(i, Report("Identify"));
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("Name");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::InputText("##name", rename_, sizeof rename_, ImGuiInputTextFlags_EnterReturnsTrue)) {
      known_[s.pairId].nickname = rename_;
      knownDirty_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("Enter saves it (empty: no name)");
    ImGui::EndPopup();
  }
  char text[160];
  std::snprintf(text, sizeof text, "Pair a new device into slot %d? %s will be forgotten.", i, DeviceLabel(s).c_str());
  if (Confirm("Replace?", text) && pendingSlot_ == i) dongle->Pair(60, i, Report("Pairing"));
  std::snprintf(text, sizeof text, "Unpair %s from slot %d? It can't connect until it's paired again.", DeviceLabel(s).c_str(), i);
  if (Confirm("Unpair?", text) && pendingSlot_ == i) dongle->Forget(i, Report("Unpair"));
  ImGui::EndChild();
  ImGui::PopID();
}

void Ui::Slots(const DongleState& d) {
  Title("Slots");
  PairBar(d);
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 10);
  if (!d.pairing) {
    ImGui::BeginDisabled(d.paired == 0);
    if (TintButton("Unpair all", kBad, ImGui::GetFontSize() * 10)) ImGui::OpenPopup("Unpair all?");
    ImGui::EndDisabled();
  }
  if (Confirm("Unpair all?", "Forget every device paired to this dongle? They can't connect until they're paired again."))
    Current()->Forget(-1, Report("Unpair all"));
  ImGui::Spacing();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const int columns = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 60 ? 3 : 2;
  const float w = (ImGui::GetContentRegionAvail().x - gap * (columns - 1)) / columns;
  for (int i = 0; i < kSlots; ++i) {
    if (i % columns) ImGui::SameLine();
    SlotCard(d, i, w);
  }
}

void Ui::Radio(const DongleState& d) {
  Title("Radio");
  // every slot, one row each
  if (ImGui::BeginTable("radio", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
    ImGui::TableSetupColumn("Device");
    for (const char* c : {"Polls/s", "Loss", "Resent/s", "Frames/s", "Data", "On air", "Signal d / t"})
      ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * (c[0] == 'S' ? 7.5f : 5.2f));
    ImGui::TableHeadersRow();
    for (int i = 0; i < kSlots; ++i) {
      const auto& s = d.slots[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char label[8];
      std::snprintf(label, sizeof label, "%d", i);
      if (ImGui::Selectable(label, selectedSlot_ == i, ImGuiSelectableFlags_SpanAllColumns)) selectedSlot_ = i;
      ImGui::TableNextColumn();
      if (!s.used) { ImGui::TextDisabled("Empty"); continue; }
      ImGui::TextColored(s.active ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kOff, "%s", DeviceLabel(s).c_str());
      if (!s.active) continue;
      ImGui::TableNextColumn(); ImGui::Text("%.0f", s.pollsPerSec);
      ImGui::TableNextColumn(); ImGui::TextColored(LossColor(s.lossPercent), "%.1f %%", std::max(0.0, s.lossPercent));
      ImGui::TableNextColumn(); ImGui::Text("%.0f", s.resendsPerSec + s.dupPerSec);
      ImGui::TableNextColumn(); ImGui::Text("%.0f", s.framesPerSec);
      ImGui::TableNextColumn(); ImGui::Text("%.1f kB/s", s.kBytesPerSec);
      ImGui::TableNextColumn();
      if (s.airKBytesPerSec > 0) ImGui::Text("%.1f kB/s", s.airKBytesPerSec); else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::TextColored(RssiColor(s.rssiDongle), "%d", s.rssiDongle);
      ImGui::SameLine(0, 4); ImGui::TextDisabled("/"); ImGui::SameLine(0, 4);
      ImGui::TextColored(RssiColor(s.rssiDevice), "%d dBm", s.rssiDevice);
    }
    ImGui::EndTable();
  }
  // the selected slot in detail: graphs over the last two minutes
  const auto& s = d.slots[selectedSlot_];
  ImGui::Spacing();
  char head[160];
  std::snprintf(head, sizeof head, "Slot %d: %s", selectedSlot_, s.used ? DeviceLabel(s).c_str() : "empty");
  ImGui::SeparatorText(head);
  if (!s.active) { ImGui::TextDisabled(s.used ? "Not connected" : "Nothing paired"); return; }
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float gw = (ImGui::GetContentRegionAvail().x - gap * 2) / 3, gh = ImGui::GetFontSize() * 6;
  char o[64];
  std::snprintf(o, sizeof o, "loss %.1f %%", std::max(0.0, s.lossPercent));
  Graph("##loss", s.lossHistory, 0, 25, LossColor(s.lossPercent), o, gw, gh);
  ImGui::SameLine();
  std::snprintf(o, sizeof o, "signal %d dBm", s.rssiDongle);
  Graph("##rssi", s.rssiHistory, -95, -35, RssiColor(s.rssiDongle), o, gw, gh);
  ImGui::SameLine();
  float peak = 4;
  for (float x : s.kbpsHistory) peak = std::max(peak, x * 1.2f);
  std::snprintf(o, sizeof o, "%.1f kB/s", s.kBytesPerSec);
  Graph("##kbps", s.kbpsHistory, 0, peak, kInfo, o, gw, gh);
  ImGui::TextDisabled("last two minutes");
  ImGui::Spacing();
  if (ImGui::BeginTable("detail", 4, ImGuiTableFlags_SizingStretchProp)) {
    const auto row = [](const char* a, const std::string& b, const char* c, const std::string& e) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::TextDisabled("%s", a);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
      ImGui::TableNextColumn(); ImGui::TextDisabled("%s", c);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(e.c_str());
    };
    char a[64], b[64];
    std::snprintf(a, sizeof a, "%.0f Hz", s.imuHz); std::snprintf(b, sizeof b, "%.0f / s", s.lightPerSec);
    row("IMU", a, "Light frames", b);
    std::snprintf(a, sizeof a, "%.1f / s", s.crcPerSec); std::snprintf(b, sizeof b, "%.1f / s", s.timeoutsPerSec);
    row("Corrupted answers", a, "No answer", b);
    std::snprintf(a, sizeof a, "%d", s.deviceMissed); std::snprintf(b, sizeof b, "%d", s.deviceCrc);
    row("Polls the device missed", a, "Corrupted polls (device)", b);
    std::snprintf(a, sizeof a, "%.0f / s", s.dropsPerSec);
    std::snprintf(b, sizeof b, "%d .. %d dBm", s.rssiMin, s.rssiMax);
    row("Dropped on the dongle", a, "Signal range", b);
    if (s.airKBytesPerSec > 0 && s.kBytesPerSec > 0) {
      std::snprintf(a, sizeof a, "%.0f %% smaller", 100.0 * (1 - s.airKBytesPerSec / s.kBytesPerSec));
      std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(s.frames));
      row("Compression", a, "Frames since start", b);
    }
    ImGui::EndTable();
  }
}

void Ui::Channels(const DongleState& d) {
  Title("Channels");
  ImGui::TextWrapped("The radio hops over 16 channels and leaves out the ones losing too much (Wi-Fi, other "
                     "2.4 GHz gear). The bars are each channel's loss; grey ones are off for now.");
  ImGui::Spacing();
  // hop channels: loss bars
  {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 9;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(ImVec4(0.06f, 0.07f, 0.09f, 1)), 4);
    const float bw = w / kHops, label = ImGui::GetTextLineHeight() * 2.2f, top = ImGui::GetTextLineHeight();
    for (int k = 0; k < kHops; ++k) {
      const bool on = (d.channelMask >> k) & 1;
      const bool idle = d.joined == 0;   // nothing connected: no traffic, so no loss to measure
      const float loss = idle ? 0.f : d.hopLoss[k];
      const float frac = std::min(1.f, loss / 30.f);
      const float x0 = p.x + k * bw + bw * 0.18f, x1 = p.x + (k + 1) * bw - bw * 0.18f;
      const float base = p.y + h - label;
      const ImVec4 c = !on || idle ? kOff : LossColor(loss);
      dl->AddRectFilled(ImVec2(x0, base - std::max(2.f, frac * (h - label - top))), ImVec2(x1, base), U32(c, on ? 0.9f : 0.35f), 2);
      char t[16];
      std::snprintf(t, sizeof t, "%d", 2400 + d.hopChannel[k]);
      const ImVec2 ts = ImGui::CalcTextSize(t);
      dl->AddText(ImVec2((x0 + x1 - ts.x) / 2, base + 2), U32(on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kOff), t);
      if (idle) std::snprintf(t, sizeof t, on ? "-" : "off");
      else std::snprintf(t, sizeof t, on ? "%d%%" : "off", int(loss));
      const ImVec2 ls = ImGui::CalcTextSize(t);
      dl->AddText(ImVec2((x0 + x1 - ls.x) / 2, base + 2 + ts.y), U32(kOff), t);
      if (d.hopChannel[k] == d.currentChannel && on) dl->AddRect(ImVec2(x0 - 2, p.y + 2), ImVec2(x1 + 2, base), U32(kInfo, 0.6f), 2);
    }
    ImGui::Dummy(ImVec2(w, h));
  }
  int on = 0;
  for (int k = 0; k < kHops; ++k) on += (d.channelMask >> k) & 1;
  if (d.joined == 0) ImGui::TextDisabled("%d of %d channels in use  |  no device connected: loss shows once one is", on, kHops);
  else ImGui::TextDisabled("%d of %d channels in use  |  MHz, loss over the last couple of seconds", on, kHops);
  ImGui::Spacing();
  ImGui::SeparatorText("2.4 GHz band");
  if (BigButton("Scan the band", ImGui::GetFontSize() * 12)) Current()->Scan(Report("Scan"));
  Tip("Listens on every 1 MHz channel from 2400 to 2480: the link pauses for a moment");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (d.scan.empty()) ImGui::TextDisabled("Not scanned yet");
  else ImGui::TextDisabled("Scanned %.0f s ago. Taller is busier; Wi-Fi channels 1, 6 and 11 are marked", Now() - d.scanTime);
  if (!d.scan.empty()) {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 9;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(ImVec4(0.06f, 0.07f, 0.09f, 1)), 4);
    const int n = int(d.scan.size());
    const float bw = w / n, label = ImGui::GetTextLineHeight() * 1.3f;
    const float base = p.y + h - label;
    // Wi-Fi channels 1/6/11 (22 MHz wide around 2412/2437/2462)
    for (int wifi : {12, 37, 62}) {
      const float x0 = p.x + (wifi - 11) * bw, x1 = p.x + (wifi + 11) * bw;
      dl->AddRectFilled(ImVec2(x0, p.y + 2), ImVec2(x1, base), U32(kAccent, 0.06f));
      char t[16];
      std::snprintf(t, sizeof t, "Wi-Fi %d", wifi == 12 ? 1 : wifi == 37 ? 6 : 11);
      dl->AddText(ImVec2(x0 + 4, p.y + 4), U32(kAccent, 0.6f), t);
    }
    for (int i = 0; i < n; ++i) {
      if (d.scan[i] == 0xFF) continue;
      const float dbm = -float(d.scan[i]);  // -100 (quiet) .. -40 (busy)
      const float frac = std::clamp((dbm + 100.f) / 60.f, 0.f, 1.f);
      const ImVec4 c = dbm > -60 ? kBad : dbm > -75 ? kWarn : kGood;
      dl->AddRectFilled(ImVec2(p.x + i * bw + 1, base - std::max(2.f, frac * (h - label - 4))), ImVec2(p.x + (i + 1) * bw - 1, base), U32(c, 0.85f));
    }
    for (int k = 0; k < kHops; ++k) {   // our hop channels
      const int ch = d.hopChannel[k];
      if (ch >= n) continue;
      const float x = p.x + (ch + 0.5f) * bw;
      dl->AddTriangleFilled(ImVec2(x, base + 2), ImVec2(x - 4, base + 9), ImVec2(x + 4, base + 9),
                            U32((d.channelMask >> k) & 1 ? kInfo : kOff));
    }
    for (int mhz = 0; mhz <= 80; mhz += 10) {
      char t[8];
      std::snprintf(t, sizeof t, "%d", 2400 + mhz);
      dl->AddText(ImVec2(p.x + mhz * bw, base + 10), U32(kOff), t);
    }
    ImGui::Dummy(ImVec2(w, h + ImGui::GetTextLineHeight()));
    ImGui::TextDisabled("Blue marks: the channels the radio hops over");
  }
}

void Ui::Firmware(const DongleState& d) {
  Title("Firmware");
  const auto dongle = Current();
  if (ImGui::BeginTable("fw", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    const auto row = [](const char* a, const std::string& b) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::TextDisabled("%s", a);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
    };
    char t[96];
    row("Dongle", d.serial);
    std::snprintf(t, sizeof t, "%d (%s build%s)", d.firmware, d.softdeviceVariant ? "Nordic bootloader" : "bare",
                  d.watchdog ? ", watchdog" : "");
    row("Firmware", t);
    row("Up for", Uptime(d.uptimeMs));
    std::snprintf(t, sizeof t, "0x%X", d.resetReason);
    row("Last reset", d.resetReason == 0 ? "power on" : d.resetReason & 1 ? "reset pin" : d.resetReason & 4 ? "software" : d.resetReason & 2 ? "watchdog" : t);
    std::snprintf(t, sizeof t, "%u%s", d.joinsHeard, "");
    row("Join requests heard", t);
    if (d.crashCount) {
      std::snprintf(t, sizeof t, "%u (last: type %u at 0x%08X)", d.crashCount, d.crashType, d.crashPc);
      row("Crashes", t);
    } else {
      row("Crashes", "none");
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (BigButton("Restart dongle", ImGui::GetFontSize() * 12)) ImGui::OpenPopup("Restart?");
  Tip("The devices reconnect by themselves in a second or two");
  if (d.crashCount) {
    ImGui::SameLine();
    if (BigButton("Clear crash record", ImGui::GetFontSize() * 12)) dongle->ClearCrash(Report("Clear crash record"));
  }
  if (Confirm("Restart?", "Restart the dongle? Every device drops for a moment and reconnects.")) dongle->Reset(Report("Restart"));

  ImGui::Spacing();
  ImGui::SeparatorText("Update");
  ImGui::TextWrapped("Pick the .uf2 file for this dongle (%s). The dongle restarts into update mode and shows up "
                     "as a USB drive; the file is copied onto it and the dongle starts the new firmware by itself.",
                     d.softdeviceVariant ? "ohdongle_sd.uf2" : "ohdongle_nosd.uf2");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13);
  ImGui::InputTextWithHint("##uf2", "path to the .uf2 file", firmwarePath_, sizeof firmwarePath_);
  ImGui::SameLine();
  const bool exists = firmwarePath_[0] && fs::exists(fs::u8path(firmwarePath_));
  ImGui::BeginDisabled(!exists || updating_);
  if (TintButton(updating_ ? "Updating..." : "Update", kAccent, -1)) ImGui::OpenPopup("Update?");
  ImGui::EndDisabled();
  if (Confirm("Update?", "Update the dongle's firmware? Every device drops until it's done (about 10 s)."))
    StartUpdate(dongle, firmwarePath_);
  {
    std::lock_guard lock(updateMutex_);
    if (!updateStatus_.empty()) ImGui::TextColored(updateError_ ? kBad : updating_ ? kInfo : kGood, "%s", updateStatus_.c_str());
  }
}

void Ui::StartUpdate(std::shared_ptr<Dongle> dongle, const std::string& file) {
  if (updating_) return;
  if (updater_.joinable()) updater_.join();
  updating_ = true;
  const auto set = [this](const std::string& s, bool error = false) {
    std::lock_guard lock(updateMutex_);
    updateStatus_ = s;
    updateError_ = error;
  };
  set("Putting the dongle in update mode...");
  updater_ = std::thread([this, dongle, file, set] {
    const auto before = Uf2Drives();
    dongle->EnterBootloader(nullptr);
    const double until = Now() + 20;
    fs::path drive;
    while (Now() < until && drive.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      for (const auto& d : Uf2Drives())
        if (std::find(before.begin(), before.end(), d) == before.end() || before.size() == 0) drive = d;
    }
    if (drive.empty()) {
      set("The update drive didn't show up. If it appeared but isn't mounted, mount it and copy the file onto it yourself.", true);
      updating_ = false;
      return;
    }
    set("Copying onto " + drive.string() + "...");
    std::error_code ec;
    fs::copy_file(fs::u8path(file), drive / fs::u8path(file).filename(), fs::copy_options::overwrite_existing, ec);
    std::error_code gone;
    if (ec && fs::exists(drive, gone)) {   // the drive leaves as the dongle restarts: that's success
      set("Copy failed: " + ec.message(), true);
    } else {
      set("Done: the dongle restarts with the new firmware.");
    }
    updating_ = false;
  });
}

void Ui::About() {
  Title("About");
  ImGui::TextWrapped("openhouse dongle manager: the openhouse wireless dongle's slots, pairing and radio.");
  ImGui::Spacing();
  ImGui::TextWrapped("A dongle has six slots, one device each. Pairing puts a device into a slot; it then "
                     "reconnects to that slot by itself whenever it's on. SteamVR's own pairing page also pairs "
                     "into openhouse dongles while the openhouse driver runs: it picks a free slot, or one whose "
                     "device is off.");
  ImGui::Spacing();
  ImGui::TextWrapped("The driver doesn't need to be stopped to use this: both can talk to the dongle.");
  ImGui::Spacing();
  ImGui::TextDisabled("openhouse %s", lh::kVersion);
  if (ImGui::Button("Release page")) overlay::UpdateCheck::OpenReleasesPage();
}

}  // namespace openhouse::dongle
