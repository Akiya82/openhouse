#include "ohdongle.hpp"

#include <hidapi/hidapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device_config.hpp"
#include "platform/platform.hpp"
#include "usb_transport.hpp"

namespace lh::usb {
namespace {

constexpr uint16_t kVid = 0x1209, kPid = 0x0001;
// Radio path latency on top of the wired 0.9 ms the IMU parser already subtracts
// (sample -> LPC -> SPI -> air, up to one TDMA cycle -> USB). ClockSync follows the minimum.
constexpr double kExtraLatency = 0.0015;
constexpr double kFeatureTimeout = 0.15;

constexpr uint8_t kCmdDown = 0x21, kCmdStats = 0x23, kCmdPair = 0x24, kCmdLive = 0x29, kCmdIdent = 0x2A;

uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; w && *w; ++w) s += char(*w < 128 ? *w : '?');
  return s;
}

// "device_serial_number" from a device config JSON, "" if absent.
std::string ConfigSerial(const std::string& json) {
  size_t k = json.find("\"device_serial_number\"");
  if (k == std::string::npos) return "";
  k = json.find(':', k);
  if (k == std::string::npos) return "";
  const size_t a = json.find('"', k), b = a == std::string::npos ? a : json.find('"', a + 1);
  if (b == std::string::npos || b - a > 40) return "";
  return json.substr(a + 1, b - a - 1);
}

void AddSensorType(std::string* json, int type) {
  const size_t brace = json->find('{');
  if (brace == std::string::npos || json->find("\"sensor_type\"") != std::string::npos) return;
  json->insert(brace + 1, "\"sensor_type\":" + std::to_string(type) + ",");
}

}  // namespace

OhDongle::OhDongle(IInputSink* sink, ReportDecoder* decoder, RawCaptureWriter* capture, std::atomic<uint16_t>* nextEp,
                   std::string steamConfigDir, LogFn log)
    : sink_(sink), decoder_(decoder), capture_(capture), nextEp_(nextEp), steamConfigDir_(std::move(steamConfigDir)),
      log_(std::move(log)) {
  if (steamConfigDir_.empty()) steamConfigDir_ = platform::SteamConfigDirectory();
}

OhDongle::~OhDongle() { Close(); }

bool OhDongle::Open() {
  hid_device_info* list = hid_enumerate(kVid, kPid);
  // interface 0: commands/replies + even slots' stream; interface 1 (newer dongle firmware): odd slots'
  // stream (a second 1 ms IN endpoint; one carries ~61 kB/s, six devices need more)
  std::string path, path1, serial;
  for (hid_device_info* d = list; d; d = d->next) {
    if (Narrow(d->product_string) != "openhouse dongle") continue;
    if (d->interface_number == 1) { path1 = d->path; continue; }
    if (!path.empty()) continue;
    path = d->path;
    serial = "OHDG-" + Narrow(d->serial_number);
  }
  hid_free_enumeration(list);
  if (path.empty()) return false;
  dev_ = hid_open_path(path.c_str());
  if (!dev_) return false;
  hid_set_nonblocking(dev_, 1);
  dev1_ = path1.empty() ? nullptr : hid_open_path(path1.c_str());
  if (dev1_) hid_set_nonblocking(dev1_, 1);
  {
    std::lock_guard lock(mutex_);
    serial_ = serial;
  }
  for (auto& s : slots_) { s.used = s.active = s.noIdent = false; s.identSeq = -1; s.identTries = 0; }
  nextLive_ = 0;
  log_(LogLevel::Info, "openhouse dongle " + serial + " opened");
  return true;
}

void OhDongle::Drop() {
  if (!dev_) return;
  for (int i = 0; i < 6; ++i) if (slots_[i].attached) Detach(i);
  hid_close(dev_);
  dev_ = nullptr;
  if (dev1_) hid_close(dev1_);
  dev1_ = nullptr;
  std::string serial;
  {
    std::lock_guard lock(mutex_);
    serial.swap(serial_);
  }
  log_(LogLevel::Info, "openhouse dongle " + serial + " closed");
}

void OhDongle::Close() {
  Drop();
  {
    std::lock_guard lock(mutex_);
    for (auto& w : featureWaits_) w->done = true;
    featureWaits_.clear();
  }
  cv_.notify_all();
  for (auto& s : slots_) if (s.worker.joinable()) s.worker.join();
}

void OhDongle::Send(uint8_t cmd, const uint8_t* args, size_t n) {
  if (!dev_) return;
  uint8_t buf[65] = {0, cmd, ++seq_};
  std::memcpy(buf + 3, args, std::min<size_t>(n, 62));
  if (hid_write(dev_, buf, sizeof buf) < 0) Drop();
}

void OhDongle::Downlink(int slot, const uint8_t* frame, size_t n) {
  uint8_t a[62] = {uint8_t(slot), uint8_t(n)};
  std::memcpy(a + 2, frame, std::min<size_t>(n, 60));
  Send(kCmdDown, a, 2 + std::min<size_t>(n, 60));
}

void OhDongle::Pump(double now) {
  if (!dev_) {
    if (now < nextOpen_) return;
    nextOpen_ = now + 2.0;
    if (!Open()) return;
  }
  if (now >= nextLive_) {
    nextLive_ = now + 0.25;
    Send(kCmdLive, nullptr, 0);
  }
  if (now >= nextStats_) {             // one paired slot's counters every ~1/6 s: each slot once a second
    nextStats_ = now + 1.0 / 6;
    for (int k = 0; k < 6; ++k) {
      const int i = (statsSlot_ + k) % 6;
      if (!slots_[i].used) continue;
      statsSlot_ = (i + 1) % 6;
      const uint8_t a = uint8_t(i);
      Send(kCmdStats, &a, 1);
      statsSeq_ = seq_ | (i << 8);
      break;
    }
    RefreshStatsCache();
  }
  std::optional<int> pair;
  int pairSlot = -1;
  std::deque<std::pair<int, std::vector<uint8_t>>> downs;
  std::vector<std::pair<int, uint8_t>> requests;
  {
    std::lock_guard lock(mutex_);
    pair.swap(pairMs_);
    pairSlot = pairSlot_;
    downs.swap(downlinks_);
    for (auto& w : featureWaits_)
      if (!w->sent) { w->sent = true; requests.emplace_back(w->slot, w->id); }
  }
  if (pair) {
    // [secs][slot + 1] (0: the first free slot; older dongle firmware ignores it)
    const uint8_t args[2] = {uint8_t(std::clamp((*pair + 999) / 1000, 0, 255)), uint8_t(pairSlot + 1)};
    Send(kCmdPair, args, 2);
  }
  for (auto& [slot, frame] : downs) Downlink(slot, frame.data(), frame.size());
  for (auto& [slot, id] : requests) {
    const uint8_t f[2] = {'P', id};
    Downlink(slot, f, 2);
  }
  for (int i = 0; i < 6; ++i) {
    Slot& s = slots_[i];
    if (s.workerDone) {
      s.worker.join();
      s.workerDone = false;
      if (s.serial.empty() && !s.fetchedSerial.empty()) {
        s.serial = s.fetchedSerial;
        s.gyro = s.fetchedGyro;
        s.accel = s.fetchedAccel;
      }
      if (s.sensorType < 0) s.sensorType = s.fetchedSensorType;
      if (s.active && !s.serial.empty() && !s.attached) {
        std::string config = s.fetchedConfig;
        if (config.empty()) config = UsbTransport::LoadSteamConfig(steamConfigDir_, s.serial);
        Attach(i, config);
      }
    }
    if (s.active && s.serial.empty() && s.noIdent && !s.worker.joinable() && now - s.identAskT > 5.0) {
      s.identAskT = now;
      StartConfigFetch(i);   // retried every 5 s until the device answers
    } else if (s.active && s.serial.empty() && !s.noIdent && !s.worker.joinable() && now - s.identAskT > 0.5) {
      s.identAskT = now;
      if (++s.identTries > 4) { s.noIdent = true; continue; }   // no 'H': identify it from its config
      const uint8_t a = uint8_t(i);
      Send(kCmdIdent, &a, 1);
      s.identSeq = seq_;
    }
  }
  if (!dev_) return;
  auto drain = [&](hid_device_* dev) {
    uint8_t buf[2][64];
    int n[2] = {0, 0}, cur = 0;
    n[cur] = hid_read(dev, buf[cur], 64);
    for (int count = 0; n[cur] > 0 && count < 64; ++count) {
      const int next = cur ^ 1;
      n[next] = hid_read(dev, buf[next], 64);
      OnReport(now, buf[cur], n[cur], n[next] <= 0);
      cur = next;
    }
    return n[cur] >= 0;
  };
  if (dev1_ && !drain(dev1_)) { Drop(); return; }
  if (!drain(dev_)) Drop();
}

void OhDongle::OnReport(double t, const uint8_t* r, int n, bool last) {
  if (n < 3) return;
  if (r[0] == 0x80 || r[0] == 0x81) {
    const int slot = r[1], len = std::min<int>(r[2], n - 3);
    if (slot >= 6) return;
    static thread_local std::vector<uint8_t> frag[6];
    const uint8_t* body = r + 3;
    if (r[0] == 0x81) {
      frag[slot].insert(frag[slot].end(), body, body + len);
      if (frag[slot].size() >= size_t(1 + frag[slot][0])) {
        OnFrame(slot, t, frag[slot].data() + 1, frag[slot][0], last);
        frag[slot].clear();
      }
      return;
    }
    frag[slot].clear();
    for (int i = 0; i < len && i + 1 + body[i] <= len;) {
      const int fl = body[i];
      if (!fl) break;
      OnFrame(slot, t, body + i + 1, fl, last && i + 1 + fl >= len);
      i += 1 + fl;
    }
    return;
  }
  const uint8_t* d = r + 3;
  if (r[0] == kCmdLive && r[2] == 0 && n >= 63) {
    for (int i = 0; i < 6; ++i) {
      const uint8_t* e = d + i * 10;
      Slot& s = slots_[i];
      s.used = e[0] & 1;
      const bool active = (e[0] & 2) != 0;
      if (s.active && !active) {
        if (s.attached) Detach(i);
        s.serial.clear();
        s.noIdent = false;
        s.identTries = 0;
      }
      s.active = active;
    }
  } else if (r[0] == kCmdStats && r[2] == 0 && n >= 63 && statsSeq_ >= 0 && r[1] == (statsSeq_ & 0xff)) {
    Slot& s = slots_[statsSeq_ >> 8];
    statsSeq_ = -1;
    uint32_t v[15];
    std::memcpy(v, d, sizeof v);
    if (s.haveStats && t > s.statsT) {
      const double dt = t - s.statsT;
      const uint32_t dp = v[0] - s.polls, dok = v[1] - s.respOk;
      s.fps = (v[5] - s.framesUp) / dt;
      s.kBps = (v[6] - s.bytesUp) / dt / 1000;
      s.dps = (v[7] - s.drops) / dt;
      s.loss = dp ? 100.0 * (1.0 - double(std::min(dok, dp)) / dp) : -1;
    }
    s.haveStats = true; s.statsT = t;
    s.polls = v[0]; s.respOk = v[1]; s.framesUp = v[5]; s.bytesUp = v[6]; s.drops = v[7];
    s.rssiD = -std::abs(int32_t(v[10])); s.rssiT = -std::abs(int32_t(v[11]));   // dongle keeps magnitudes
  } else if (r[0] == kCmdIdent && n >= 19) {
    for (int i = 0; i < 6; ++i) {
      Slot& s = slots_[i];
      if (s.identSeq != r[1]) continue;
      s.identSeq = -1;
      if (r[2] == 0 && d[0] == 'H') { if (s.active) OnIdent(i, d, 16); }
      else if (r[2] == 0 && d[0] == 0) s.noIdent = true;   // the device sent no 'H': not our firmware
      break;
    }
  }
}

void OhDongle::OnFrame(int slot, double t, const uint8_t* f, int n, bool last) {
  Slot& s = slots_[slot];
  switch (f[0]) {
    case 'H':
      OnIdent(slot, f, n);
      return;
    case 'p': {
      if (n != 18) return;
      std::memcpy(s.ring[s.ringPos].data(), f + 1, 17);
      if (s.ringN == 0) s.ring[1] = s.ring[2] = s.ring[0];
      s.ringPos = (s.ringPos + 1) % 3;
      s.ringN = std::min(s.ringN + 1, 3);
      if (!s.attached) return;
      uint8_t rep[1 + 3 * 17] = {0x20};
      for (int k = 0; k < 3; ++k) std::memcpy(rep + 1 + 17 * k, s.ring[k].data(), 17);
      capture_->WriteReport(t - kExtraLatency, s.epImu, last, rep, sizeof rep);
      decoder_->OnReport(t - kExtraLatency, s.epImu, last, rep, sizeof rep);
      return;
    }
    case 'q': {
      if (!s.attached || n < 9 || (n - 1) % 8 || n > 57) return;
      uint8_t rep[0x39];
      std::memset(rep, 0xff, sizeof rep);
      rep[0] = 0x21;
      std::memcpy(rep + 1, f + 1, size_t(n - 1));
      capture_->WriteReport(t, s.epOpt, last, rep, sizeof rep);
      decoder_->OnReport(t, s.epOpt, last, rep, sizeof rep);
      return;
    }
    case 's': {   // LH2 stream: wired 0x28 is [0x28][len][stream]
      if (!s.attached || n < 2) return;
      uint8_t rep[64] = {0x28, uint8_t(std::min(n - 1, 62))};
      std::memcpy(rep + 2, f + 1, rep[1]);
      capture_->WriteReport(t, s.epOpt, last, rep, sizeof rep);
      decoder_->OnReport(t, s.epOpt, last, rep, sizeof rep);
      return;
    }
    case 'Q':     // input: the wired buttons report's bytes 8..23 (u32 buttons at 8, trackpad at 0x14)
    case 'n':     // Index input: wired type 0x0700 bytes 8.. (buttons, trigger, axes, 0xa1 record at 20)
    case 'V': {   // other controller reports: the wired report from byte 2
      if (!s.attached) return;
      uint8_t rep[64] = {0x01};
      if (f[0] == 'Q') {
        if (n < 17) return;
        rep[2] = 1; rep[3] = 0x3C;
        std::memcpy(rep + 8, f + 1, 16);
      } else if (f[0] == 'n') {
        if (n < 2) return;
        rep[1] = 0x00; rep[2] = 0x07;
        std::memcpy(rep + 8, f + 1, size_t(std::min(n - 1, 56)));
      } else {
        std::memcpy(rep + 2, f + 1, size_t(std::min(n - 1, 62)));
      }
      capture_->WriteReport(t, s.epBtn, last, rep, sizeof rep);
      decoder_->OnReport(t, s.epBtn, last, rep, sizeof rep);
      return;
    }
    case 'B':
      if (s.attached && n >= 2) sink_->OnBattery(s.serial, {0.0, f[1]});
      return;
    case 'P': {
      std::lock_guard lock(mutex_);
      for (auto& w : featureWaits_) {
        if (w->slot != slot || !w->sent || w->done) continue;
        w->reply.assign(1, w->id);
        w->reply.insert(w->reply.end(), f + 1, f + n);
        w->done = true;
        cv_.notify_all();
        break;
      }
      return;
    }
    default:
      return;
  }
}

void OhDongle::OnIdent(int slot, const uint8_t* f, int n) {
  if (n < 16 || f[1] != 1) return;
  Slot& s = slots_[slot];
  char serial[16];
  std::snprintf(serial, sizeof serial, "LHR-%08X", unsigned(Rd32(f + 2)));
  if (s.serial == serial) return;
  if (s.attached) Detach(slot);
  s.serial = serial;
  s.gyro = int8_t(f[6]);
  s.accel = int8_t(f[7]);
  const uint32_t hw = Rd32(f + 8);
  s.sensorType = (hw & 0xff) <= 11 && hw ? int(hw & 0xff) : -1;
  std::string config = UsbTransport::LoadSteamConfig(steamConfigDir_, s.serial);
  if (!config.empty()) Attach(slot, config);
  else if (!s.worker.joinable()) StartConfigFetch(slot);
}

void OhDongle::StartConfigFetch(int slot) {
  Slot& s = slots_[slot];
  s.fetchedConfig.clear();
  s.workerDone = false;
  s.fetchedSerial.clear();
  const std::string serial = s.serial;
  const std::string who = serial.empty() ? "slot " + std::to_string(slot) : serial;
  log_(LogLevel::Info, who + ": reading device config over the radio");
  s.worker = std::thread([this, slot, serial, who] {
    Slot& w = slots_[slot];
    std::string json, error;
    const auto t0 = std::chrono::steady_clock::now();
    auto read = [&](uint8_t* data, size_t size) { return FeatureRead(slot, data, size); };
    if (serial.empty()) {          // unidentified device: IMU ranges from feature 0x01 (as wired)
      uint8_t r[64] = {0x01};
      if (read(r, sizeof r) >= 3 && r[0] == 0x01) { w.fetchedGyro = int8_t(r[1]); w.fetchedAccel = int8_t(r[2]); }
    }
    w.fetchedSensorType = -1;
    if (w.sensorType < 0) {        // sensor type: hardware id low byte in feature 0x05 at [41] (as wired:
      for (int a = 0; a < 3 && w.fetchedSensorType < 0; ++a) {   // without it, OOTX decoding guesses)
        uint8_t r[64] = {0x05};
        if (read(r, sizeof r) >= 45 && r[0] == 0x05) {
          const uint32_t build = Rd32(r + 1), hw = Rd32(r + 41);
          if (build && hw && (hw & 0xff) <= 11) w.fetchedSensorType = int(hw & 0xff);
          break;
        }
      }
      log_(LogLevel::Info, who + ": sensor type " + (w.fetchedSensorType < 0 ? std::string("unavailable") :
                               std::to_string(w.fetchedSensorType)) + " (feature 0x05)");
    }
    const bool ok = ReadDeviceConfig(read, &json, &error);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ok) {
      w.fetchedConfig = json;
      if (serial.empty()) w.fetchedSerial = ConfigSerial(json);
      log_(LogLevel::Info, who + ": read device config (" + std::to_string(json.size()) + " bytes) in " +
                               std::to_string(int(ms)) + " ms" +
                               (serial.empty() ? ", serial " + (w.fetchedSerial.empty() ? "missing" : w.fetchedSerial) : ""));
    } else {
      log_(LogLevel::Warn, who + ": device config unavailable (" + error + ")");
    }
    w.workerDone = true;
  });
}

int OhDongle::FeatureRead(int slot, uint8_t* data, size_t size) {
  auto w = std::make_shared<FeatureWait>();
  w->slot = slot;
  w->id = data[0];
  std::unique_lock lock(mutex_);
  if (serial_.empty()) return -1;
  featureWaits_.push_back(w);
  cv_.wait_for(lock, std::chrono::duration<double>(kFeatureTimeout), [&] { return w->done; });
  std::erase(featureWaits_, w);
  if (w->reply.empty()) return -1;
  const size_t n = std::min(size, w->reply.size());
  std::memcpy(data, w->reply.data(), n);
  return int(n);
}

void OhDongle::Attach(int slot, const std::string& configIn) {
  Slot& s = slots_[slot];
  std::string config = configIn;
  if (s.sensorType >= 0 && !config.empty()) AddSensorType(&config, s.sensorType);
  if (config.empty()) log_(LogLevel::Warn, s.serial + ": no config (plug it in by USB once, or check the radio link)");
  EndpointInfo imu;
  imu.ep = (*nextEp_)++;
  imu.vid = kVid;
  imu.pid = kPid;
  imu.iface = uint8_t(slot);
  imu.kind = EndpointKind::Imu;
  imu.serial = s.serial;
  imu.gyroRange = s.gyro;
  imu.accelRange = s.accel;
  imu.configJson = config;
  EndpointInfo opt = imu;
  opt.ep = (*nextEp_)++;
  opt.kind = EndpointKind::Optical;
  opt.configJson.clear();
  EndpointInfo btn = opt;
  btn.ep = (*nextEp_)++;
  btn.kind = EndpointKind::Buttons;
  s.epImu = imu.ep;
  s.epOpt = opt.ep;
  s.epBtn = btn.ep;
  capture_->WriteEndpoint(imu);
  capture_->WriteEndpoint(opt);
  capture_->WriteEndpoint(btn);
  decoder_->AddEndpoint(imu);
  decoder_->AddEndpoint(opt);
  decoder_->AddEndpoint(btn);
  s.attached = true;
  {
    std::lock_guard lock(mutex_);
    eps_.push_back(imu);
    eps_.push_back(opt);
    eps_.push_back(btn);
  }
  log_(LogLevel::Info, s.serial + ": connected on openhouse dongle slot " + std::to_string(slot));
}

void OhDongle::Detach(int slot) {
  Slot& s = slots_[slot];
  if (!s.attached) return;
  decoder_->RemoveEndpoint(s.epImu);
  decoder_->RemoveEndpoint(s.epOpt);
  decoder_->RemoveEndpoint(s.epBtn);
  sink_->OnDeviceDetached(s.serial);
  s.attached = false;
  s.ringN = 0;
  {
    std::lock_guard lock(mutex_);
    std::erase_if(eps_, [&](const EndpointInfo& e) { return e.serial == s.serial; });
  }
  log_(LogLevel::Info, s.serial + ": disconnected from openhouse dongle slot " + std::to_string(slot));
}

std::vector<EndpointInfo> OhDongle::Endpoints() const {
  std::lock_guard lock(mutex_);
  return eps_;
}

std::vector<DeviceId> OhDongle::Connected() const {
  std::lock_guard lock(mutex_);
  std::vector<DeviceId> v;
  for (auto& e : eps_) if (e.kind == EndpointKind::Imu) v.push_back(e.serial);
  return v;
}

std::string OhDongle::Serial() const {
  std::lock_guard lock(mutex_);
  return serial_;
}

bool OhDongle::Command(const DeviceId& serial, const std::array<uint8_t, 64>& report, const char** status, size_t size) {
  int slot = -1;
  {
    std::lock_guard lock(mutex_);
    for (auto& e : eps_) if (e.serial == serial && e.kind == EndpointKind::Imu) slot = e.iface;
    if (slot < 0) return false;
    // Radio 'P' frame, as Valve's dongles tunnel host feature reports to a device's main MCU
    // (HTC's and Valve's main firmwares, and ours, all read it this way):
    //   [0xff][cmd][len][data] -> [P][cmd][len][data]   (command; cmd >= 0x80)
    //   [id][data] (id < 0x80) -> [P][id][data]         (SET_FEATURE; [P][id] alone is a GET)
    std::vector<uint8_t> frame(1, 'P');
    if (report[0] == 0xff) {
      const size_t n = std::min<size_t>(2 + report[2], 59);
      frame.insert(frame.end(), report.begin() + 1, report.begin() + 1 + long(n));
    } else {
      const size_t n = std::min<size_t>(std::max<size_t>(size, 2), 60);
      frame.insert(frame.end(), report.begin(), report.begin() + long(n));
    }
    downlinks_.emplace_back(slot, std::move(frame));
  }
  *status = "written";
  return true;
}

bool OhDongle::Pair(int milliseconds, int slot) {
  std::lock_guard lock(mutex_);
  if (serial_.empty()) return false;
  pairMs_ = milliseconds;
  pairSlot_ = slot >= 0 && slot < 6 ? slot : -1;
  return true;
}

std::vector<OhDongle::SlotStat> OhDongle::SlotStats() const {
  std::lock_guard lock(mutex_);
  return serial_.empty() ? std::vector<SlotStat>() : statsCache_;
}

void OhDongle::RefreshStatsCache() {     // reader thread: slots_ is its own
  std::vector<SlotStat> out;
  int joined = 0;
  for (const auto& s : slots_) joined += s.active;
  for (int i = 0; i < 6; ++i) {
    const Slot& s = slots_[i];
    SlotStat st;
    st.slot = i; st.used = s.used; st.active = s.active; st.serial = s.attached ? s.serial : DeviceId();
    if (s.active) {
      st.kBytesPerSec = s.kBps; st.framesPerSec = s.fps; st.lossPercent = s.loss; st.dropsPerSec = s.dps;
      st.rssiDongle = s.rssiD; st.rssiDevice = s.rssiT;
    }
    st.devicesOnDongle = joined;
    out.push_back(st);
  }
  std::lock_guard lock(mutex_);
  statsCache_.swap(out);
}

}  // namespace lh::usb
