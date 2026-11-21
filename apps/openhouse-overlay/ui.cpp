#include <filesystem>
#include "ui.hpp"

#include "openvr_runtime.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "common/ui_theme.hpp"
#include "lh/bug_report.hpp"
#include "lh/version.hpp"

namespace openhouse::overlay {
namespace {
using json = nlohmann::json;
using viewer::Base;
using viewer::Device;
using viewer::State;

// The pixel font at 2x (default), 3x (titles) and 4x (big numbers): sharp only at whole multiples.
ImFont* g_title = nullptr;
ImFont* g_huge = nullptr;

const ImVec4 kGood{0.36f, 0.80f, 0.45f, 1}, kWarn{1.00f, 0.78f, 0.30f, 1}, kBad{1.00f, 0.42f, 0.40f, 1},
    kInfo{0.45f, 0.70f, 1.00f, 1}, kOff{0.55f, 0.58f, 0.62f, 1}, kAccent{1.00f, 0.86f, 0.55f, 1};

std::filesystem::path SettingsPath() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  return std::filesystem::path(base ? base : ".") / "lh" / "overlay.json";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "lh" / "overlay.json";
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".config" / "lh" / "overlay.json";
#endif
}

std::string Short(const std::string& serial) {
  const auto dash = serial.find('-');
  return dash == std::string::npos ? serial : serial.substr(dash + 1);
}

std::string DeviceName(const Device& d) {
  std::string kind = d.role == "hmd" ? "Headset" : d.role == "controller" ? "Controller" : "Tracker";
  if (d.hand == "left_hand") kind += " (left)";
  else if (d.hand == "right_hand") kind += " (right)";
  return kind;
}

std::string BaseName(const State& s, const std::string& serial) {
  for (const auto& b : s.bases)
    if (b.serial == serial && !b.channel.empty()) return "Channel " + b.channel + " (" + Short(serial) + ")";
  return Short(serial);
}

// 1.0 stations have letter channels (b, c, A) or none; 2.0 ones numbers.
bool IsLh2(const Base& b) { return b.lh2; }

ImVec4 DeviceColor(const Device& d) {
  if (!d.connected || !d.enabled) return kOff;
  if (d.state == "tracking") return kGood;
  if (d.state == "coasting" || d.state == "acquiring") return kWarn;
  return kBad;
}

const char* DeviceStateText(const Device& d) {
  if (!d.connected) return "Off";
  if (!d.enabled) return "Disabled";
  if (d.state == "tracking") return "Tracking";
  if (d.state == "coasting") return "Coasting";
  if (d.state == "lost") return "Lost";
  if (d.state == "acquiring") return "Acquiring";
  return "No light";
}

ImVec4 BaseColor(const Base& b) {
  if (!b.seen) return kOff;
  return b.origin || b.learned ? kGood : kWarn;
}

std::string BaseStatus(const Base& b, double settleSigma) {
  char text[96];
  if (!b.seen && !b.placed) return "Not seen";
  if (b.origin) return "Origin";
  if (b.locked) return "Locked";
  if (!b.placed) return "Not placed";
  if (b.learned) { std::snprintf(text, sizeof text, "Placed (%.1f mm)", b.sigma * 1000); return text; }
  std::snprintf(text, sizeof text, "Learning (%.0f mm, needs %.0f)", b.sigma * 1000, settleSigma * 1000);
  return text;
}

void Dot(const ImVec4& color, float radius = 5) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, p.y + h / 2), radius, ImGui::ColorConvertFloat4ToU32(color));
  ImGui::Dummy(ImVec2(radius * 2 + 4, h));
  ImGui::SameLine();
}

// A labelled value row in a two-column table.
void Row(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void Row(const char* label, const char* fmt, ...) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  va_list args;
  va_start(args, fmt);
  ImGui::TextV(fmt, args);
  va_end(args);
}

// A page's title: large, with its icon before it.
void Title(const char* text, lh::ui::Icon icon) {
  ImGui::PushFont(g_title);
  const float s = ImGui::GetTextLineHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  lh::ui::DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + s / 2, p.y + s / 2), s * 0.85f, lh::ui::kAccent);
  ImGui::Dummy(ImVec2(s, s));
  ImGui::SameLine(0, s * 0.45f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
}


// A short tooltip on the last item, also when it's disabled.
void Tip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

bool BeginFacts(const char* id) {
  return ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg);
}

// A summary tile on the overview: a big number and what it counts.

bool Confirm(const char* id, const char* text) {
  bool yes = false;
  if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("%s", text);
    ImGui::Spacing();
    const float w = ImGui::GetFontSize() * 7, h = ImGui::GetFrameHeight() * 1.5f;
    if (ImGui::Button("Yes", ImVec2(w, h))) { yes = true; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return yes;
}

json Settings(const State& s) {
  json j = json::parse(s.settingsJson, nullptr, false);
  return j.is_object() ? j : json::object();
}
}  // namespace

Ui::Ui(int port) : client_(port) {
  std::ifstream in(SettingsPath());
  const json j = json::parse(in, nullptr, false);
  if (j.is_object()) {
    detail_ = j.value("advanced", false) ? viewer::Detail::Advanced : viewer::Detail::Basic;
    logs_.Filter().debug = j.value("debugLog", false);
  }
  update_.Start();
  // OPENHOUSE_OVERLAY_PAGE (testing, with OPENHOUSE_OVERLAY_SNAPSHOT): the page shown first.
  if (const char* page = std::getenv("OPENHOUSE_OVERLAY_PAGE")) {
    const std::string p = page;
    page_ = p == "devices" ? Page::Devices : p == "bases" ? Page::Bases : p == "dongles" ? Page::Dongles : p == "map" ? Page::Map
          : p == "settings" ? Page::Settings : p == "debug" ? Page::Debug : p == "about" ? Page::About : Page::Overview;
  }
}

void Ui::SaveSettings() const {
  const json j = {{"advanced", detail_ == viewer::Detail::Advanced}, {"debugLog", const_cast<Ui*>(this)->logs_.Filter().debug}};
  std::error_code ec;
  std::filesystem::create_directories(SettingsPath().parent_path(), ec);
  std::ofstream(SettingsPath()) << j.dump(2) << "\n";
}

void Ui::SetUpImGui(int k) {
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.Fonts->Clear();
  // ImGui's own pixel font: sharp only at whole multiples of its 13 px.
  ImFontConfig font;
  font.OversampleH = font.OversampleV = 1;
  font.PixelSnapH = true;
  font.SizePixels = 13.f * k;
  io.Fonts->AddFontDefault(&font);
  font.SizePixels = 13.f * (k + 1);
  g_title = io.Fonts->AddFontDefault(&font);
  font.SizePixels = 13.f * (k + 2);
  g_huge = io.Fonts->AddFontDefault(&font);
  // Plain ImGui, with roomy frames: easy to hit with a laser pointer.
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0;
  style.FramePadding = ImVec2(8, 6);
  style.ItemSpacing = ImVec2(10, 8);
  style.WindowPadding = ImVec2(12, 12);
  style.ScaleAllSizes(float(k));
  lh::ui::ApplyTheme(float(k));
  style.Colors[ImGuiCol_ChildBg] = lh::ui::CardColor();   // bordered panels are cards over the snow
}

// A wide, tall button: the main actions, easy to hit with a laser pointer.
static bool BigButton(const char* label, float width = 0) {
  return ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.25f));
}

// The same, for what can't be undone (they ask first).
static bool DangerButton(const char* label, float width = 0) { return BigButton(label, width); }

void Ui::Toast(const std::string& text, bool error) {
  toast_ = text;
  toastError_ = error;
  toastUntil_ = now_ + 6;
}

void Ui::Send(const std::string& text, const std::string& what) {
  const uint64_t id = client_.SendCommand(text);
  if (!id) { Toast("Not connected", true); return; }
  pending_[id] = what;
}

void Ui::Poll(double now) {
  now_ = now;
  client_.Poll(now);
  for (const auto& r : client_.TakeReplies()) {
    const auto it = pending_.find(r.id);
    const std::string what = it == pending_.end() ? "command" : it->second;
    if (it != pending_.end()) pending_.erase(it);
    if (r.ok) Toast(what + ": done");
    else Toast(what + " failed" + (r.error.empty() ? "" : ": " + r.error), true);
  }
}

void Ui::Draw(float width, float height) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::Begin("openhouse", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoScrollbar);
  lh::ui::Snow(ImGui::GetWindowDrawList(), ImVec2(0, 0), ImVec2(width, height));
  const float statusHeight = ImGui::GetFrameHeightWithSpacing() + 4;
  const float body = ImGui::GetContentRegionAvail().y - statusHeight;
  Sidebar(body);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("page", ImVec2(0, body), ImGuiChildFlags_None);
  ImGui::PopStyleColor();
  switch (page_) {
    case Page::Overview: Overview(); break;
    case Page::Devices: Devices(); break;
    case Page::Bases: Bases(); break;
    case Page::Dongles: Dongles(); break;
    case Page::Map: Map(); break;
    case Page::Settings: SettingsPage(); break;
    case Page::Debug: Debug(); break;
    case Page::About: About(); break;
  }
  ImGui::EndChild();
  StatusBar();
  ReportPopup();
  ImGui::End();
}

void Ui::Sidebar(float height) {
  // Wide enough for the logo and the name beside it.
  ImGui::PushFont(g_title);
  const float name = ImGui::CalcTextSize("openhouse").x;
  ImGui::PopFont();
  const float logoSide = ImGui::GetFontSize() * 2.6f;
  const float w = std::max(ImGui::GetFontSize() * 10, logoSide + name + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().WindowPadding.x * 2 + 4);
  ImGui::BeginChild("sidebar", ImVec2(w, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  if (logo_) {
    ImGui::Image(static_cast<ImTextureID>(logo_), ImVec2(logoSide, logoSide));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted("openhouse");
  ImGui::PopFont();
  ImGui::TextDisabled("v%s", lh::kVersion);
  ImGui::EndGroup();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  using lh::ui::Icon;
  struct Item { Page page; const char* label; Icon icon; };
  static const Item items[] = {{Page::Overview, "Overview", Icon::Overview}, {Page::Devices, "Devices", Icon::Tracker},
                               {Page::Bases, "Base stations", Icon::BaseStation}, {Page::Dongles, "Dongles", Icon::Dongle},
                               {Page::Map, "Room map", Icon::Map},          {Page::Settings, "Settings", Icon::Settings},
                               {Page::Debug, "Debug", Icon::Debug},         {Page::About, "About", Icon::About}};
  // Tall rows (easy to hit with a laser pointer), as tall as fits above the room actions.
  const float gap = 4 * lh::ui::Scale();
  const float bottom = ImGui::GetTextLineHeightWithSpacing() * 2.2f + ImGui::GetFrameHeight() * 1.25f * 2 +
                       ImGui::GetStyle().ItemSpacing.y * 4;
  const float room = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - bottom;
  const float rowHeight = std::clamp(room / float(std::size(items)) - gap, ImGui::GetFrameHeight(), ImGui::GetFrameHeight() * 1.3f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, gap));
  for (const auto& item : items)
    if (lh::ui::NavItem(item.label, item.icon, page_ == item.page, rowHeight)) page_ = item.page;
  ImGui::PopStyleVar();
  // Recording and the room reset, then the connection, at the bottom.
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const float actions = ImGui::GetFrameHeight() * 1.25f * 2 + ImGui::GetStyle().ItemSpacing.y * 3;
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - line * 2.2f - actions));
  RoomActions();
  ImGui::Separator();
  Dot(client_.Connected() ? kGood : kBad);
  ImGui::TextUnformatted(client_.Connected() ? "Connected" : "Not connected");
  ImGui::EndChild();
}

void Ui::RoomActions() {
  const bool connected = client_.Connected();
  const bool recording = connected && Settings(client_.Current()).value("recording", false);
  ImGui::BeginDisabled(!connected);
  if (recording) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.16f, 0.14f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.24f, 0.20f, 1));
  }
  if (BigButton(recording ? "Stop recording" : "Record session", -1))
    Send(json{{"cmd", "record"}, {"on", !recording}}.dump(), recording ? "Stop recording" : "Record session");
  if (recording) ImGui::PopStyleColor(2);
  Tip(recording ? "Stops and saves a bug report with the recording" : "Records the raw tracking data for a bug report");
  if (BigButton("Reset room", -1)) ImGui::OpenPopup("Reset the room?");
  ImGui::EndDisabled();
  if (Confirm("Reset the room?", "All base stations will be learned again.")) Send(R"({"cmd":"resetRoom"})", "Reset room");
}

void Ui::CopyServerLog() {
  const std::string dir = SteamVRLogDirectory();
  std::string text;
  if (!dir.empty()) {
    std::ifstream in(std::filesystem::u8path(dir + "/vrserver.txt"), std::ios::binary);
    if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (text.empty()) {
    logCopyResult_ = "vrserver.txt not found";
  } else {
    ImGui::SetClipboardText(text.c_str());
    logCopyResult_ = "Copied vrserver.txt";
  }
  showLogCopy_ = true;
}

void Ui::ReportPopup() {
  if (showLogCopy_) { ImGui::OpenPopup("vrserver logs"); showLogCopy_ = false; }
  if (ImGui::BeginPopupModal("vrserver logs", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(logCopyResult_.c_str());
    ImGui::Spacing();
    if (BigButton("Close", ImGui::GetFontSize() * 10)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  // A new report from the driver (Stop recording, Save bug report): say where it is.
  if (client_.Connected() && client_.HaveState()) {
    const json r = Settings(client_.Current()).value("report", json::object());
    const uint64_t count = r.value("count", uint64_t(0));
    if (reportCount_ != UINT64_MAX && count > reportCount_) {
      reportPath_ = r.value("path", std::string());
      reportError_ = r.value("error", std::string());
      showReport_ = true;
    }
    reportCount_ = count;
  }
  if (showReport_) { ImGui::OpenPopup("Bug report"); showReport_ = false; }
  if (ImGui::BeginPopupModal("Bug report", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (reportPath_.empty()) {
      ImGui::TextColored(kBad, "Couldn't save the bug report");
      if (!reportError_.empty()) ImGui::TextUnformatted(reportError_.c_str());
    } else {
      ImGui::TextUnformatted("Saved to:");
      ImGui::TextColored(kInfo, "%s", reportPath_.c_str());
    }
    ImGui::Spacing();
    const float w = ImGui::GetFontSize() * 10;
    if (!reportPath_.empty()) {
      if (BigButton("Open folder", w)) UpdateCheck::Open(reportPath_.substr(0, reportPath_.find_last_of("/\\")));
      ImGui::SameLine();
    }
    if (BigButton("Close", w)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void Ui::StatusBar() {
  ImGui::Separator();
  bool any = false;
  if (update_.Done() && update_.Newer()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAccent, "Update available: %s", update_.Latest().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Update")) update_.OpenLatestRelease();
    Tip("Close SteamVR before installing");
    any = true;
  }
  if (!toast_.empty() && now_ < toastUntil_) {
    if (any) ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(toastError_ ? kBad : kGood, "%s", toast_.c_str());
    any = true;
  } else if (!pending_.empty()) {
    if (any) ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("...");
    any = true;
  }
  if (!any) ImGui::TextUnformatted("");
}

// ---- pages ----

void Ui::Overview() {
  Title("Overview", lh::ui::Icon::Overview);
  if (!client_.Connected()) {
    ImGui::TextColored(kBad, "Not connected to the driver");
    return;
  }
  const State& s = client_.Current();
  const json st = Settings(s);
  int seen = 0, placed = 0;
  for (const auto& b : s.bases) { seen += b.seen; placed += b.seen && (b.origin || b.learned); }

  // One line: room, runtime, driver, recording.
  if (seen == 0) ImGui::TextColored(kWarn, "Room: no base stations");
  else ImGui::TextColored(s.room.settled ? kGood : kWarn, "Room: %s (%d / %d placed)", s.room.settled ? "Ready" : "Learning", placed, seen);
  ImGui::SameLine(0, ImGui::GetFontSize() * 2);
  ImGui::Text("Runtime: %s", st.value("steamvr", false) ? "SteamVR" : st.value("host", std::string("?")).c_str());
  ImGui::SameLine(0, ImGui::GetFontSize() * 2);
  ImGui::Text("Driver: v%s", client_.Version().empty() ? "?" : client_.Version().c_str());
  if (st.value("recording", false)) {
    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::TextColored(kBad, "Recording");
  }

  // Devices: what each one is doing.
  ImGui::SeparatorText("Devices");
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
  if (s.devices.empty()) ImGui::TextDisabled("No devices");
  else if (ImGui::BeginTable("devices", 6, flags)) {
    std::vector<const Device*> ordered;
    for (const auto& d : s.devices) ordered.push_back(&d);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Device* a, const Device* b) { return (a->role == "hmd") > (b->role == "hmd"); });
    ImGui::TableSetupColumn("Device", 0, 2.2f);
    ImGui::TableSetupColumn("Serial", 0, 1.6f);
    ImGui::TableSetupColumn("State", 0, 1.2f);
    ImGui::TableSetupColumn("Battery", 0, 1.6f);
    ImGui::TableSetupColumn("Bases", 0, 0.8f);
    ImGui::TableSetupColumn("Primary base", 0, 1.8f);
    ImGui::TableHeadersRow();
    for (const Device* dp : ordered) {
      const Device& d = *dp;
      ImGui::PushID(d.serial.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable(DeviceName(d).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
        selectedDevice_ = d.serial;
        page_ = Page::Devices;
      }
      ImGui::TableNextColumn(); ImGui::TextUnformatted(d.serial.c_str());
      ImGui::TableNextColumn(); ImGui::TextColored(DeviceColor(d), "%s", DeviceStateText(d));
      ImGui::TableNextColumn();
      if (d.battery) {
        char text[24];
        std::snprintf(text, sizeof text, "%d%%%s", *d.battery, d.charging ? " +" : "");
        const ImVec4 c = *d.battery <= 15 ? kBad : *d.battery <= 35 ? kWarn : kGood;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
        ImGui::ProgressBar(*d.battery / 100.f, ImVec2(-1, 0), text);
        ImGui::PopStyleColor();
      } else {
        ImGui::TextDisabled("-");
      }
      ImGui::TableNextColumn();
      int lit = 0;
      for (const auto& c : d.coverage) lit += c.frames > 0;
      if (d.connected) ImGui::Text("%d", lit); else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(d.primary.empty() ? "-" : BaseName(s, d.primary).c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Base stations: placement and who sees them.
  ImGui::SeparatorText("Base stations");
  if (s.bases.empty()) ImGui::TextDisabled("No base stations");
  else if (ImGui::BeginTable("bases", 5, flags)) {
    ImGui::TableSetupColumn("Base station", 0, 1.6f);
    ImGui::TableSetupColumn("Serial", 0, 1.6f);
    ImGui::TableSetupColumn("Status", 0, 2.6f);
    ImGui::TableSetupColumn("Seen by", 0, 0.9f);
    ImGui::TableSetupColumn("Faults", 0, 0.8f);
    ImGui::TableHeadersRow();
    for (const auto& b : s.bases) {
      ImGui::PushID(b.serial.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const std::string name = b.channel.empty() ? "Channel ?" : "Channel " + b.channel;
      if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
        selectedBase_ = b.serial;
        page_ = Page::Bases;
      }
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b.serial.c_str());
      ImGui::TableNextColumn(); ImGui::TextColored(BaseColor(b), "%s", BaseStatus(b, s.room.settleSigma).c_str());
      ImGui::TableNextColumn();
      int by = 0;
      for (const auto& d : s.devices)
        for (const auto& c : d.coverage) by += c.base == b.serial && c.frames > 0;
      ImGui::Text("%d", by);
      ImGui::TableNextColumn();
      if (b.faults) ImGui::TextColored(kBad, "0x%02x", b.faults); else ImGui::TextDisabled("-");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  if (BigButton("Copy vrserver logs")) CopyServerLog();
}

void Ui::Devices() {
  Title("Devices", lh::ui::Icon::Tracker);
  const State& s = client_.Current();
  if (s.devices.empty()) { ImGui::TextDisabled("No devices"); return; }
  const float listWidth = ImGui::GetFontSize() * 20;
  ImGui::BeginChild("device-list", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
  for (const auto& d : s.devices) {
    if (selectedDevice_.empty()) selectedDevice_ = d.serial;
    ImGui::PushID(d.serial.c_str());
    Dot(DeviceColor(d));
    char label[96];
    std::snprintf(label, sizeof label, "%s\n%s", DeviceName(d).c_str(), d.serial.c_str());
    if (ImGui::Selectable(label, selectedDevice_ == d.serial, ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetTextLineHeight() * 2.2f)))
      selectedDevice_ = d.serial;
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("device", ImVec2(0, 0), ImGuiChildFlags_Borders);
  const Device* d = nullptr;
  for (const auto& x : s.devices) if (x.serial == selectedDevice_) d = &x;
  if (!d) { ImGui::TextDisabled("No device selected"); ImGui::EndChild(); return; }
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted(DeviceName(*d).c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s  %s", d->model.c_str(), d->serial.c_str());
  ImGui::Spacing();
  if (BeginFacts("facts")) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("State");
    ImGui::TableNextColumn();
    ImGui::TextColored(DeviceColor(*d), "%s", DeviceStateText(*d));
    if (d->battery) Row("Battery", "%d%%%s", *d->battery, d->charging ? " (charging)" : "");
    else Row("Battery", "-");
    Row("Primary base", "%s", d->primary.empty() ? "-" : BaseName(s, d->primary).c_str());
    if (d->valid) {
      Row("Position", "%.3f  %.3f  %.3f m", d->p[0], d->p[1], d->p[2]);
      Row("Speed", "%.2f m/s", std::sqrt(d->v[0] * d->v[0] + d->v[1] * d->v[1] + d->v[2] * d->v[2]));
    }
    ImGui::EndTable();
  }
  ImGui::SeparatorText("Light");
  if (d->coverage.empty()) ImGui::TextDisabled("None");
  for (const auto& c : d->coverage) {
    const bool used = std::find(d->bases.begin(), d->bases.end(), c.base) != d->bases.end();
    char text[64];
    std::snprintf(text, sizeof text, "%.1f sensors, %d sweeps", c.sensors, c.frames);
    ImGui::ProgressBar(float(std::min(c.sensors / 12.0, 1.0)), ImVec2(ImGui::GetFontSize() * 18, 0), text);
    ImGui::SameLine();
    ImGui::Text("%s%s", BaseName(s, c.base).c_str(), used ? "" : " (unused)");
  }
  ImGui::SeparatorText("Actions");
  const bool canBuzz = d->role != "hmd" && d->connected;
  ImGui::BeginDisabled(!canBuzz);
  if (BigButton("Vibrate")) Send(json{{"cmd", "identify"}, {"serial", d->serial}}.dump(), "Identify " + d->serial);
  ImGui::SameLine();
  if (DangerButton("Power off")) ImGui::OpenPopup("Power off?");
  ImGui::EndDisabled();
  if (Confirm("Power off?", ("Turn " + d->serial + " off?").c_str()))
    Send(json{{"cmd", "powerOff"}, {"serial", d->serial}}.dump(), "Power off " + d->serial);
  ImGui::SameLine();
  bool enabled = d->enabled;
  const bool changed = ImGui::Checkbox("Enabled", &enabled);
  Tip("Not tracked while off");
  if (changed)
    Send(json{{"cmd", "enable"}, {"serial", d->serial}, {"enabled", enabled}}.dump(), (enabled ? "Enable " : "Disable ") + d->serial);
  ImGui::EndChild();
}

void Ui::Bases() {
  Title("Base stations", lh::ui::Icon::BaseStation);
  const State& s = client_.Current();
  if (s.bases.empty()) { ImGui::TextDisabled("No base stations"); return; }
  const float listWidth = ImGui::GetFontSize() * 20;
  ImGui::BeginChild("base-list", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
  for (const auto& b : s.bases) {
    if (selectedBase_.empty()) selectedBase_ = b.serial;
    ImGui::PushID(b.serial.c_str());
    Dot(BaseColor(b));
    char label[96];
    std::snprintf(label, sizeof label, "%s\n%s  %s", b.channel.empty() ? "Channel ?" : ("Channel " + b.channel).c_str(),
                  b.serial.c_str(), IsLh2(b) ? "2.0" : "1.0");
    if (ImGui::Selectable(label, selectedBase_ == b.serial, ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetTextLineHeight() * 2.2f)))
      selectedBase_ = b.serial;
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("base", ImVec2(0, 0), ImGuiChildFlags_Borders);
  const Base* b = nullptr;
  for (const auto& x : s.bases) if (x.serial == selectedBase_) b = &x;
  if (!b) { ImGui::TextDisabled("No base station selected"); ImGui::EndChild(); return; }
  ImGui::PushFont(g_title);
  ImGui::TextUnformatted(BaseName(s, b->serial).c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s, Lighthouse %s", b->serial.c_str(), IsLh2(*b) ? "2.0" : "1.0");
  ImGui::Spacing();
  if (BeginFacts("facts")) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("Placement");
    ImGui::TableNextColumn();
    ImGui::TextColored(BaseColor(*b), "%s", BaseStatus(*b, s.room.settleSigma).c_str());
    Row("Seen", "%s", b->seen ? "Yes" : "No");
    Row("Uncertainty", "%.1f mm", b->sigma * 1000);
    Row("Keyframes", "%d", b->observations);
    if (b->placed) Row("Position", "%.3f  %.3f  %.3f m", b->p[0], b->p[1], b->p[2]);
    Row("Mode", "%d", b->mode);
    if (b->faults) Row("Faults", "0x%02x", b->faults);
    else Row("Faults", "None");
    ImGui::EndTable();
  }
  ImGui::SeparatorText("Seen by");
  bool any = false;
  for (const auto& d : s.devices)
    for (const auto& c : d.coverage)
      if (c.base == b->serial && c.frames > 0) {
        any = true;
        Dot(DeviceColor(d), 4);
        ImGui::Text("%s %s: %.1f sensors, %d sweeps", DeviceName(d).c_str(), Short(d.serial).c_str(), c.sensors, c.frames);
      }
  if (!any) ImGui::TextDisabled("None");
  ImGui::EndChild();
}

void Ui::Dongles() {
  Title("Dongles", lh::ui::Icon::Dongle);
  const State& s = client_.Current();
  if (s.receivers.empty()) { ImGui::TextDisabled("No dongles"); return; }
  if (!ImGui::BeginTable("dongles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn("Dongle");
  ImGui::TableSetupColumn("Device");
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
  ImGui::TableHeadersRow();
  for (const auto& [serial, connected] : s.receivers) {
    ImGui::PushID(serial.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(serial.c_str());
    ImGui::TableNextColumn();
    if (connected.empty()) ImGui::TextDisabled("-");
    else {
      std::string name = connected;
      for (const auto& d : s.devices) if (d.serial == connected) name = DeviceName(d) + "  " + connected;
      ImGui::TextUnformatted(name.c_str());
    }
    ImGui::TableNextColumn();
    if (ImGui::Button("Pair")) Send(json{{"cmd", "pair"}, {"receiver", serial}, {"seconds", 30}}.dump(), "Pairing on " + serial);
    Tip("Hold the device's power button until it blinks");
    ImGui::PopID();
  }
  ImGui::EndTable();
}

void Ui::Map() {
  Title("Room map", lh::ui::Icon::Map);
  const State& s = client_.Current();
  const json st = Settings(s);
  const float bar = ImGui::GetFrameHeight() * 1.25f + ImGui::GetStyle().ItemSpacing.y;
  ImGui::BeginChild("map", ImVec2(0, -bar), ImGuiChildFlags_Borders);
  viewer::DrawMap(s);
  ImGui::EndChild();

  // Room learning: how many of the base stations seen are placed.
  int seen = 0, placed = 0;
  for (const auto& b : s.bases) { seen += b.seen; placed += b.seen && (b.origin || b.learned); }
  const float buttonWidth = ImGui::GetFontSize() * 12;
  const float progressWidth = ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x;
  char text[96];
  if (!client_.Connected()) std::snprintf(text, sizeof text, "Not connected");
  else if (seen == 0) std::snprintf(text, sizeof text, "No base stations seen");
  else std::snprintf(text, sizeof text, "%s: %d / %d base stations placed", s.room.settled ? "Ready" : "Learning", placed, seen);
  const ImVec4 color = client_.Connected() && seen && s.room.settled ? kGood : kWarn;
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
  ImGui::ProgressBar(seen ? float(placed) / float(seen) : 0.f, ImVec2(progressWidth, ImGui::GetFrameHeight() * 1.25f), text);
  ImGui::PopStyleColor();
  ImGui::SameLine();

  const bool steamvr = st.value("steamvr", false);
  const bool available = client_.Connected() && st.value("calibration", json::object()).value("available", false);
  ImGui::BeginDisabled(!available || floorAt_ > 0);
  if (BigButton("Calibrate floor", buttonWidth)) {
    floorAt_ = now_ + 7;
    ImGui::OpenPopup("Calibrate floor");
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !available) {
    if (steamvr) ImGui::SetTooltip("Use SteamVR's Room Setup instead");
    else if (client_.Connected()) ImGui::SetTooltip("The room isn't ready yet");
  }
  FloorCalibration();
}

void Ui::FloorCalibration() {
  if (floorAt_ <= 0) return;
  if (!ImGui::IsPopupOpen("Calibrate floor")) ImGui::OpenPopup("Calibrate floor");
  if (ImGui::BeginPopupModal("Calibrate floor", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    const int left = std::max(0, int(std::ceil(floorAt_ - now_)));
    ImGui::TextUnformatted("Put your controllers on the floor");
    ImGui::PushFont(g_huge);
    ImGui::Text("%d", left);
    ImGui::PopFont();
    if (BigButton("Cancel", ImGui::GetFontSize() * 10)) {
      floorAt_ = 0;
      ImGui::CloseCurrentPopup();
    } else if (now_ >= floorAt_) {
      Send(R"({"cmd":"calibrate","action":"floorControllers"})", "Calibrate floor");
      floorAt_ = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void Ui::SettingsPage() {
  Title("Settings", lh::ui::Icon::Settings);
  if (!client_.Connected() || !client_.HaveState()) { ImGui::TextDisabled("Not connected"); return; }
  const json st = Settings(client_.Current());
  const bool steamvr = st.value("steamvr", false);
  auto set = [&](const char* key, const json& value, const std::string& what) {
    Send(json{{"cmd", "set"}, {"key", key}, {"value", value}}.dump(), what);
  };
  auto toggle = [&](const char* key, const char* label, const char* help = nullptr) {
    bool v = st.value(key, false);
    if (ImGui::Checkbox(label, &v)) set(key, v, label);
    if (help) Tip(help);
  };
  auto slider = [&](const char* key, const char* label, const char* help) {
    static std::map<std::string, float> editing;
    float v = editing.count(key) ? editing[key] : st.value(key, 60.f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
    if (ImGui::SliderFloat(label, &v, 0, 150, "%.0f ms")) editing[key] = std::round(v / 5) * 5;
    Tip(help);
    if (ImGui::IsItemDeactivatedAfterEdit()) { set(key, editing[key], label); editing.erase(key); }
  };
  ImGui::SeparatorText("Output");
  slider("positionSmoothingMs", "Position smoothing", "Higher is steadier, lower reacts faster");
  slider("rotationSmoothingMs", "Rotation smoothing", "Higher is steadier, lower reacts faster");
  toggle("predictWithAcceleration", "Acceleration prediction", "Less lag on fast moves");
  ImGui::SeparatorText("Mixed basestations (experimental)");
  toggle("mixedLight", "Enabled");
  ImGui::BeginDisabled(!st.value("mixedLight", false));
  ImGui::Indent();
  toggle("mixedLightControllers", "Controllers use both kinds");
  toggle("mixedLightTrackers", "Trackers use both kinds");
  toggle("mixedLightScout", "Headset times the 2.0 stations", "Fewer blips");
  toggle("mixedLightRotorReference", "Rotor reference", "The headset measures the 2.0 rotor wobble for the other devices");
  toggle("mixedLightPreferLh2", "1.0 as fallback only", "Use 1.0 light only when 2.0 doesn't see a device well");
  ImGui::Unindent();
  ImGui::EndDisabled();
  ImGui::SeparatorText("Room");
  ImGui::BeginDisabled(!steamvr);
  toggle("useSteamVRRooms", "Use SteamVR's rooms", "Restart SteamVR after changing it");
  ImGui::EndDisabled();
}

void Ui::Debug() {
  Title("Debug", lh::ui::Icon::Debug);
  bool advanced = detail_ == viewer::Detail::Advanced;
  if (ImGui::Checkbox("Advanced", &advanced)) detail_ = advanced ? viewer::Detail::Advanced : viewer::Detail::Basic;
  ImGui::SameLine();
  ImGui::Checkbox("Debug logs", &logs_.Filter().debug);
  ImGui::SameLine();
  ImGui::Checkbox("Hide multi", &logs_.Filter().hideMulti);
  ImGui::SameLine();
  ImGui::Checkbox("Hide 2.0", &logs_.Filter().hideLh2);
  const State& s = client_.Current();
  const float top = ImGui::GetContentRegionAvail().y * 0.45f;
  ImGui::BeginChild("tables", ImVec2(0, top), ImGuiChildFlags_Borders);
  if (ImGui::CollapsingHeader("Devices", ImGuiTreeNodeFlags_DefaultOpen)) viewer::DrawDevices(s, detail_);
  if (ImGui::CollapsingHeader("Base stations", ImGuiTreeNodeFlags_DefaultOpen)) viewer::DrawBases(s, detail_);
  if (ImGui::CollapsingHeader("Room")) {
    ImGui::Text("%s, epoch %llu, %s, %llu resets this session", s.room.id.c_str(), s.room.epoch, s.room.settled ? "settled" : "settling",
                s.room.resets);
  }
  if (ImGui::CollapsingHeader("Settings (raw)")) ImGui::TextWrapped("%s", Settings(s).dump(2).c_str());
  ImGui::EndChild();
  ImGui::SeparatorText("Log");
  logs_.Draw(client_);
}

void Ui::About() {
  Title("About", lh::ui::Icon::About);
  ImGui::SeparatorText("openhouse");
  ImGui::Text("Version: v%s", lh::kVersion);
  ImGui::Text("Driver: %s", client_.Connected() ? (client_.Version().empty() ? "?" : ("v" + client_.Version()).c_str()) : "Not connected");
  ImGui::SeparatorText("Updates");
  if (update_.Disabled()) ImGui::TextDisabled("Disabled (OPENHOUSE_NO_UPDATE_CHECK)");
  else if (update_.Busy()) ImGui::TextDisabled("Checking...");
  else if (update_.Done() && update_.Latest().empty()) ImGui::TextColored(kWarn, "Couldn't check");
  else if (update_.Done() && update_.Newer()) ImGui::TextColored(kAccent, "Update available: %s", update_.Latest().c_str());
  else if (update_.Done() && update_.Unknown()) ImGui::TextColored(kWarn, "Can't compare this build with %s", update_.Latest().c_str());
  else if (update_.Done()) ImGui::TextColored(kGood, "Up to date");
  if (!update_.Disabled()) {
    ImGui::BeginDisabled(update_.Busy());
    if (BigButton("Check")) update_.Start();
    ImGui::EndDisabled();
    ImGui::SameLine();
  }
  if (update_.Done() && update_.Unknown()) {
    if (BigButton("Reinstall")) update_.OpenLatestRelease();
    ImGui::SameLine();
  }
  if (BigButton("Open GitHub Releases")) UpdateCheck::OpenReleasesPage();
  if (!inVR_) {
    ImGui::Spacing();
    if (BigButton("Quit")) quit_ = true;
  }
}

}  // namespace openhouse::overlay
