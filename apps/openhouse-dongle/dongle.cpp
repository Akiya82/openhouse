#include "dongle.hpp"

#include <hidapi/hidapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "device_config.hpp"

namespace openhouse::dongle {
namespace {
constexpr uint16_t kVid = 0x1209, kPid = 0x0001;
// Commands (doorman/dongle-fw/src/dongle.h, link.c)
constexpr uint8_t kInfo = 0x01, kBootloader = 0x10, kReset = 0x12, kClearCrash = 0x14, kLink = 0x20, kDown = 0x21,
                  kScan = 0x22, kStats = 0x23, kPair = 0x24, kSlotsCmd = 0x25, kForget = 0x26, kChannels = 0x27,
                  kCodec = 0x28, kLive = 0x29, kIdent = 0x2A;

hid_device* H(void* p) { return static_cast<hid_device*>(p); }
uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; w && *w; ++w) s += *w < 128 ? char(*w) : '?';
  return s;
}

std::string Hex(const uint8_t* p, int n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (int i = n - 1; i >= 0; --i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }   // as the dongle prints it
  return s;
}

void Push(std::deque<float>& h, float v) {
  h.push_back(v);
  while (h.size() > size_t(kHistory)) h.pop_front();
}

// A model name for the UI, from the device's config.
std::string ModelName(const nlohmann::json& j) {
  const std::string model = j.value("model_number", std::string());
  if (!model.empty()) return model;
  const std::string render = j.value("render_model", std::string());
  if (render.find("tracker_vive_1_0") != std::string::npos) return "Vive Tracker 1.0";
  if (render.find("tracker") != std::string::npos) return "Tracker";
  const std::string cls = j.value("device_class", std::string());
  return cls.empty() ? "Lighthouse device" : cls;
}
}  // namespace

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------------------------------------
Dongle::Dongle(std::string path, std::string serial) : path_(std::move(path)), serial_(std::move(serial)) {
  state_.serial = serial_;
  state_.path = path_;
  thread_ = std::thread([this] { Run(); });
}

Dongle::~Dongle() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

DongleState Dongle::Snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void Dongle::Queue(std::function<void()> action) {
  std::lock_guard lock(mutex_);
  actions_.push_back(std::move(action));
}

void Dongle::SetNickname(int slot, const std::string& name) {
  std::lock_guard lock(mutex_);
  if (slot >= 0 && slot < kSlots) state_.slots[slot].nickname = name;
}

void Dongle::SetKnown(const std::string& pairId, const Identity& id) {
  std::lock_guard lock(mutex_);
  for (auto& [k, v] : known_)
    if (k == pairId) { v = id; return; }
  known_.emplace_back(pairId, id);
}

bool Dongle::Open() {
  dev0_ = hid_open_path(path_.c_str());
  if (!dev0_) return false;
  hid_set_nonblocking(H(dev0_), 1);
  // interface 1 (odd slots' stream), the sibling with the same serial
  if (hid_device_info* list = hid_enumerate(kVid, kPid)) {
    for (hid_device_info* d = list; d; d = d->next)
      if (d->interface_number == 1 && Narrow(d->serial_number) == serial_.substr(5)) {
        dev1_ = hid_open_path(d->path);
        if (dev1_) hid_set_nonblocking(H(dev1_), 1);
        break;
      }
    hid_free_enumeration(list);
  }
  return true;
}

void Dongle::Close() {
  if (dev0_) hid_close(H(dev0_));
  if (dev1_) hid_close(H(dev1_));
  dev0_ = dev1_ = nullptr;
}

// One command and its reply; stream reports read meanwhile are handled as they come.
bool Dongle::Command(uint8_t cmd, const uint8_t* args, size_t n, Reply* reply, int timeoutMs) {
  if (!dev0_) return false;
  const uint8_t seq = ++seq_;
  uint8_t out[65] = {0, cmd, seq};
  std::memcpy(out + 3, args, std::min<size_t>(n, 62));
  if (hid_write(H(dev0_), out, sizeof out) < 0) { alive_ = false; return false; }
  const double until = Now() + timeoutMs / 1000.0;
  uint8_t r[64];
  while (Now() < until) {
    const int got = hid_read_timeout(H(dev0_), r, sizeof r, 5);
    if (got < 0) { alive_ = false; return false; }
    if (dev1_) for (int k = 0; k < 16; ++k) {
      uint8_t q[64];
      const int m = hid_read(H(dev1_), q, sizeof q);
      if (m <= 0) break;
      OnReport(q, m);
    }
    if (got == 0) continue;
    if (r[0] == cmd && r[1] == seq) {
      if (reply) { reply->status = r[2]; std::memcpy(reply->data.data(), r + 3, 61); }
      return r[2] == 0;
    }
    OnReport(r, got);
  }
  return false;
}

void Dongle::ReadPending(int timeoutMs) {
  uint8_t r[64];
  for (int pass = 0; pass < 200; ++pass) {
    bool any = false;
    for (void* d : {dev0_, dev1_}) {
      if (!d) continue;
      const int n = pass == 0 && d == dev0_ ? hid_read_timeout(H(d), r, sizeof r, timeoutMs) : hid_read(H(d), r, sizeof r);
      if (n < 0) { alive_ = false; return; }
      if (n > 0) { OnReport(r, n); any = true; }
    }
    if (!any) return;
  }
}

void Dongle::OnReport(const uint8_t* r, int n) {
  if (n < 3 || (r[0] != 0x80 && r[0] != 0x81) || r[1] >= kSlots) return;
  const int slot = r[1], len = std::min<int>(r[2], n - 3);
  const uint8_t* body = r + 3;
  auto& frag = frag_[slot];
  if (r[0] == 0x81) {
    frag.insert(frag.end(), body, body + len);
    if (!frag.empty() && frag.size() >= size_t(frag[0]) + 1) {
      if (frag[0]) OnFrame(slot, frag.data() + 1, frag[0]);
      frag.clear();
    }
    return;
  }
  frag.clear();
  for (int i = 0; i < len && i + 1 + body[i] <= len; i += 1 + body[i])
    if (body[i]) OnFrame(slot, body + i + 1, body[i]);
}

void Dongle::OnFrame(int slot, const uint8_t* f, int n) {
  auto& c = counters_[slot];
  switch (f[0]) {
    case 'p': c.imu++; break;
    case 'q': case 's': c.light++; break;
    case 'Q': case 'n': case 'V': c.input++; break;
    case 'P':
      if (slot == waitSlot_ && !waitDone_) { waitReply_.assign(f + 1, f + n); waitDone_ = true; }
      break;
    default: break;
  }
}

// A feature report read through the radio: [P][id] down, the device answers [P][data].
bool Dongle::FeatureRead(int slot, uint8_t* data, size_t size) {
  const uint8_t args[4] = {uint8_t(slot), 2, 'P', data[0]};
  waitSlot_ = slot;
  waitDone_ = false;
  if (!Command(kDown, args, sizeof args, nullptr)) { waitSlot_ = -1; return false; }
  const double until = Now() + 0.5;
  while (!waitDone_ && Now() < until && alive_) ReadPending(5);
  waitSlot_ = -1;
  if (!waitDone_) return false;
  const size_t n = std::min(size - 1, waitReply_.size());
  std::memcpy(data + 1, waitReply_.data(), n);
  return true;
}

void Dongle::Ident(int slot) {
  // our firmware: its 'H' identity, kept by the dongle
  Reply r;
  const uint8_t a = uint8_t(slot);
  if (Command(kIdent, &a, 1, &r) && r.data[0] == 'H' && r.data[1] == 1) {
    char serial[16];
    std::snprintf(serial, sizeof serial, "LHR-%08X", unsigned(Rd32(r.data.data() + 2)));
    std::lock_guard lock(mutex_);
    auto& s = state_.slots[slot];
    if (s.id.serial != serial) { s.id.serial = serial; s.id.source = "firmware"; }
  }
}

void Dongle::Poll(double now) {
  Reply r;
  if (now >= nextLive_) {
    nextLive_ = now + 0.25;
    if (Command(kLive, nullptr, 0, &r)) {
      std::lock_guard lock(mutex_);
      for (int i = 0; i < kSlots; ++i) {
        const uint8_t* e = r.data.data() + i * 10;
        auto& s = state_.slots[i];
        s.used = e[0] & 1;
        s.active = (e[0] & 2) != 0;
        s.rssiDongle = s.active ? -int(e[1]) : 0;
        s.rssiDevice = s.active ? -int(e[2]) : 0;
        s.rssiMin = -int(e[4]);  // min/max are magnitudes: the strongest is the smallest
        s.rssiMax = -int(e[3]);
        s.msSinceResponse = Rd32(e + 6);
        state_.currentChannel = e[5];
      }
    }
  }
  if (now >= nextSlow_) {
    nextSlow_ = now + 1.0;
    if (Command(kInfo, nullptr, 0, &r) && std::memcmp(r.data.data(), "OHDG", 4) == 0) {
      const uint8_t* d = r.data.data();
      std::lock_guard lock(mutex_);
      state_.firmware = d[4] | d[5] << 8;
      state_.softdeviceVariant = d[6];
      state_.watchdog = d[7];
      state_.resetReason = Rd32(d + 12);
      state_.uptimeMs = Rd32(d + 16);
      state_.crashCount = Rd32(d + 28);
      state_.crashType = Rd32(d + 32);
      state_.crashPc = Rd32(d + 36);
    }
    if (Command(kSlotsCmd, nullptr, 0, &r)) {
      std::lock_guard lock(mutex_);
      for (int i = 0; i < kSlots; ++i) {
        auto& s = state_.slots[i];
        const std::string id = r.data[i * 10] ? Hex(r.data.data() + i * 10 + 2, 8) : std::string();
        if (id != s.pairId) { s.pairId = id; s.id = {}; s.nickname.clear(); identTried_[i] = 0; }
        if (!id.empty() && s.id.serial.empty())
          for (const auto& [k, v] : known_)
            if (k == id) s.id = v;
      }
      const uint8_t p = r.data[60];
      state_.pairing = p & 1;
      state_.pairTarget = state_.pairing && (p >> 1) ? (p >> 1) - 1 : -1;
      if (!state_.pairing) state_.pairUntil = 0;
    }
    if (Command(kChannels, nullptr, 0, &r)) {
      std::lock_guard lock(mutex_);
      state_.channelMask = uint16_t(r.data[0] | r.data[1] << 8);
      for (int k = 0; k < kHops; ++k) { state_.hopChannel[k] = r.data[2 + k]; state_.hopLoss[k] = r.data[18 + k]; }
      state_.joinsHeard = Rd32(r.data.data() + 34);
    }
    const uint8_t keep = 0xFF;
    if (Command(kLink, &keep, 1, &r)) { std::lock_guard lock(mutex_); state_.linkOn = r.data[0]; }
    // identities: 'H' from our firmware, tried a few times per slot as it joins
    for (int i = 0; i < kSlots; ++i) {
      bool want;
      {
        std::lock_guard lock(mutex_);
        const auto& s = state_.slots[i];
        want = s.active && s.id.serial.empty() && identTried_[i] < 3;
      }
      if (want) { identTried_[i]++; Ident(i); }
    }
    // totals for the header
    std::lock_guard lock(mutex_);
    state_.joined = state_.paired = 0;
    state_.kBytesPerSec = 0;
    for (const auto& s : state_.slots) {
      state_.paired += s.used;
      state_.joined += s.active;
      if (s.active) state_.kBytesPerSec += s.kBytesPerSec;
    }
  }
  if (now >= nextStats_) {  // each paired slot's counters once a second, spread out
    nextStats_ = now + 1.0 / kSlots;
    const int slot = statsSlot_;
    statsSlot_ = (statsSlot_ + 1) % kSlots;
    bool used;
    { std::lock_guard lock(mutex_); used = state_.slots[slot].used; }
    if (used) PollStats(slot, now);
  }
}

void Dongle::PollStats(int slot, double now) {
  Reply r, c;
  const uint8_t a = uint8_t(slot);
  if (!Command(kStats, &a, 1, &r)) return;
  const bool codec = Command(kCodec, &a, 1, &c);
  uint32_t v[15];
  std::memcpy(v, r.data.data(), sizeof v);
  auto& k = counters_[slot];
  std::lock_guard lock(mutex_);
  auto& s = state_.slots[slot];
  if (k.have && now > k.t) {
    const double dt = now - k.t;
    const auto rate = [&](int i) { return double(v[i] - k.v[i]) / dt; };
    s.pollsPerSec = rate(0);
    const uint32_t polls = v[0] - k.v[0], ok = v[1] - k.v[1];
    s.lossPercent = polls ? 100.0 * (1.0 - double(std::min(ok, polls)) / polls) : -1;
    s.crcPerSec = rate(2);
    s.timeoutsPerSec = rate(3);
    s.dupPerSec = rate(4);
    s.framesPerSec = rate(5);
    s.kBytesPerSec = rate(6) / 1000;
    s.dropsPerSec = rate(7);
    s.resendsPerSec = rate(9);
    if (codec) s.airKBytesPerSec = double(Rd32(c.data.data()) - k.air) / dt / 1000;
    s.imuHz = double(k.imu - k.imu0) / dt;
    s.lightPerSec = double(k.light - k.light0) / dt;
    s.inputPerSec = double(k.input - k.input0) / dt;
    if (!s.active) s.pollsPerSec = s.framesPerSec = s.kBytesPerSec = s.airKBytesPerSec = s.imuHz = s.lightPerSec = 0;
    Push(s.lossHistory, s.active ? float(std::max(0.0, s.lossPercent)) : 0.f);
    Push(s.rssiHistory, s.active ? float(s.rssiDongle) : -100.f);
    Push(s.kbpsHistory, float(s.kBytesPerSec));
  }
  s.deviceMissed = int(v[12]);
  s.deviceCrc = int(v[13]);
  s.frames = v[5];
  s.drops = v[7];
  k.have = true;
  k.t = now;
  std::memcpy(k.v, v, sizeof v);
  if (codec) k.air = Rd32(c.data.data());
  k.imu0 = k.imu; k.light0 = k.light; k.input0 = k.input;
}

void Dongle::Run() {
  if (!Open()) {
    std::lock_guard lock(mutex_);
    state_.error = "can't open it (in use, or no permission: see the udev rule in the README)";
    alive_ = false;
    return;
  }
  { std::lock_guard lock(mutex_); state_.open = true; }
  while (!stop_ && alive_) {
    std::function<void()> action;
    {
      std::lock_guard lock(mutex_);
      if (!actions_.empty()) { action = std::move(actions_.front()); actions_.pop_front(); }
    }
    if (action) action();
    Poll(Now());
    ReadPending(5);
  }
  Close();
  std::lock_guard lock(mutex_);
  state_.open = false;
}

// ---- actions ----

void Dongle::Pair(int seconds, int slot, Done done) {
  Queue([=, this] {
    const uint8_t a[2] = {uint8_t(std::clamp(seconds, 0, 255)), uint8_t(slot >= 0 && slot < kSlots ? slot + 1 : 0)};
    Reply r;
    const bool ok = Command(kPair, a, 2, &r);
    if (ok) {
      std::lock_guard lock(mutex_);
      state_.pairing = seconds > 0;
      state_.pairTarget = seconds > 0 && slot >= 0 && state_.firmware >= 4 ? slot : -1;
      state_.pairUntil = seconds > 0 ? Now() + seconds : 0;
    }
    if (done) {
      if (!ok) done(false, "the dongle didn't answer");
      else if (slot >= 0 && state_.firmware < 4) done(true, "Pairing (this dongle firmware pairs into the first free slot: update it to choose)");
      else done(true, slot >= 0 ? "Pairing into slot " + std::to_string(slot) : "Pairing into the first free slot");
    }
  });
}

void Dongle::StopPairing(Done done) {
  Queue([=, this] {
    const uint8_t a[2] = {0, 0};
    const bool ok = Command(kPair, a, 2, nullptr);
    if (ok) { std::lock_guard lock(mutex_); state_.pairing = false; state_.pairUntil = 0; }
    if (done) done(ok, ok ? "Pairing stopped" : "the dongle didn't answer");
  });
}

void Dongle::Forget(int slot, Done done) {
  Queue([=, this] {
    const uint8_t a = slot < 0 ? 0xFF : uint8_t(slot);
    const bool ok = Command(kForget, &a, 1, nullptr);
    if (done) done(ok, ok ? (slot < 0 ? "Every slot unpaired" : "Slot " + std::to_string(slot) + " unpaired") : "the dongle didn't answer");
  });
}

void Dongle::PowerOff(int slot, Done done) {
  Queue([=, this] {
    const uint8_t a[2 + 7] = {uint8_t(slot), 7, 'P', 0x9f, 4, 'o', 'f', 'f', '!'};
    const bool ok = Command(kDown, a, sizeof a, nullptr);
    if (done) done(ok, ok ? "Power off sent to slot " + std::to_string(slot) : "the dongle didn't answer");
  });
}

void Dongle::Identify(int slot, Done done) {
  Queue([=, this] {
    std::string json, error;
    const auto read = [&](uint8_t* data, size_t size) -> int {
      return FeatureRead(slot, data, size) ? int(size) : -1;
    };
    const bool ok = lh::usb::ReadDeviceConfig(read, &json, &error);
    if (!ok) { if (done) done(false, "no config from the device (" + error + ")"); return; }
    const auto j = nlohmann::json::parse(json, nullptr, false);
    Identity id;
    id.serial = j.is_object() ? j.value("device_serial_number", std::string()) : std::string();
    id.model = j.is_object() ? ModelName(j) : std::string();
    id.source = "config";
    {
      std::lock_guard lock(mutex_);
      auto& s = state_.slots[slot];
      if (!id.serial.empty()) s.id = id;
    }
    if (done) done(!id.serial.empty(), id.serial.empty() ? "the config has no serial" : "Slot " + std::to_string(slot) + ": " + id.serial + " (" + id.model + ")");
  });
}

void Dongle::Scan(Done done) {
  Queue([=, this] {
    std::vector<uint8_t> all;
    bool ok = true;
    for (uint8_t first : {uint8_t(0), uint8_t(60)}) {  // 60 channels a request: 2400..2480 MHz in two
      Reply r;
      if (!Command(kScan, &first, 1, &r, 1500)) { ok = false; break; }
      all.insert(all.end(), r.data.begin(), r.data.begin() + 60);
    }
    if (ok) {
      all.resize(81);
      std::lock_guard lock(mutex_);
      state_.scan = all;
      state_.scanFirst = 0;
      state_.scanTime = Now();
    }
    if (done) done(ok, ok ? "Band scanned" : "the dongle didn't answer the scan");
  });
}

void Dongle::Reset(Done done) {
  Queue([=, this] {
    const bool ok = Command(kReset, nullptr, 0, nullptr);
    if (done) done(ok, ok ? "Dongle restarting" : "the dongle didn't answer");
  });
}

void Dongle::EnterBootloader(Done done) {
  Queue([=, this] {
    const bool ok = Command(kBootloader, nullptr, 0, nullptr);
    if (done) done(ok, ok ? "Dongle in update mode" : "the dongle didn't answer");
  });
}

void Dongle::ClearCrash(Done done) {
  Queue([=, this] {
    const bool ok = Command(kClearCrash, nullptr, 0, nullptr);
    if (done) done(ok, ok ? "Crash record cleared" : "the dongle didn't answer");
  });
}

// ---------------------------------------------------------------------------------------------
DongleManager::DongleManager() { hid_init(); }
DongleManager::~DongleManager() {
  dongles_.clear();
  hid_exit();
}

void DongleManager::Poll(double now) {
  std::erase_if(dongles_, [](const auto& d) { return !d->Alive() && d->Snapshot().error.empty(); });
  if (now < nextScan_) return;
  nextScan_ = now + 2.0;
  hid_device_info* list = hid_enumerate(kVid, kPid);
  for (hid_device_info* d = list; d; d = d->next) {
    if (Narrow(d->product_string) != "openhouse dongle" || d->interface_number > 0) continue;
    const std::string serial = "OHDG-" + Narrow(d->serial_number);
    bool have = false;
    for (auto& x : dongles_)
      if (x->Serial() == serial) {
        have = true;
        if (!x->Alive()) x = std::make_shared<Dongle>(d->path, serial);  // retry one that failed to open
      }
    if (!have) dongles_.push_back(std::make_shared<Dongle>(d->path, serial));
  }
  hid_free_enumeration(list);
}

}  // namespace openhouse::dongle
