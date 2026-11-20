// Several Doorman Dongles at once, against fake HID entry points: no call here can reach a real
// device. Each fake dongle has interface 0 (commands, replies, even slots' stream) and, unless a
// test leaves it out, interface 1 (odd slots' stream), listed in whatever order a test chooses.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <hidapi/hidapi.h>

#include "usb_transport.hpp"

namespace fake {
struct Interface {
  std::string path;
  std::wstring serial;
  int number = 0;
};
struct Handle {
  std::string path;
  std::deque<std::vector<uint8_t>> input;
  std::vector<std::vector<uint8_t>> written;
  bool gone = false, closed = false;
};
std::vector<Interface> plugged;           // what hid_enumerate lists, in this order
std::vector<hid_device_info> listed;      // the last enumeration
std::vector<std::unique_ptr<Handle>> handles;
std::function<void()> endPass;

void Reset() {
  plugged.clear();
  listed.clear();
  handles.clear();
  endPass = {};
}
// The open handle of an interface (the newest, after a replug).
Handle* Open(const std::string& path) {
  for (auto it = handles.rbegin(); it != handles.rend(); ++it)
    if ((*it)->path == path && !(*it)->closed) return it->get();
  return nullptr;
}
// Unplugged: gone from the list, and its open handles fail.
void Unplug(const std::string& prefix) {
  std::erase_if(plugged, [&](const Interface& i) { return i.path.rfind(prefix, 0) == 0; });
  for (auto& h : handles)
    if (h->path.rfind(prefix, 0) == 0) h->gone = true;
}
}  // namespace fake

extern "C" {
int HID_API_CALL hid_init() { return 0; }
hid_device_info* HID_API_CALL hid_enumerate(unsigned short, unsigned short) {
  fake::listed.assign(fake::plugged.size(), hid_device_info{});
  for (size_t i = 0; i < fake::plugged.size(); ++i) {
    hid_device_info& d = fake::listed[i];
    d.path = const_cast<char*>(fake::plugged[i].path.c_str());
    d.vendor_id = 0x6767;
    d.product_id = 0x0727;
    d.serial_number = const_cast<wchar_t*>(fake::plugged[i].serial.c_str());
    d.product_string = const_cast<wchar_t*>(L"Doorman Dongle");
    d.interface_number = fake::plugged[i].number;
    d.next = i + 1 < fake::listed.size() ? &fake::listed[i + 1] : nullptr;
  }
  return fake::listed.empty() ? nullptr : fake::listed.data();
}
void HID_API_CALL hid_free_enumeration(hid_device_info*) {}
hid_device* HID_API_CALL hid_open_path(const char* path) {
  const bool present = std::any_of(fake::plugged.begin(), fake::plugged.end(), [&](const auto& i) { return i.path == path; });
  if (!present) return nullptr;
  fake::handles.push_back(std::make_unique<fake::Handle>());
  fake::handles.back()->path = path;
  return reinterpret_cast<hid_device*>(fake::handles.back().get());
}
void HID_API_CALL hid_close(hid_device* d) { reinterpret_cast<fake::Handle*>(d)->closed = true; }
int HID_API_CALL hid_set_nonblocking(hid_device*, int) { return 0; }
int HID_API_CALL hid_send_feature_report(hid_device*, const unsigned char*, size_t) { return -1; }
int HID_API_CALL hid_get_feature_report(hid_device*, unsigned char*, size_t) { return -1; }
int HID_API_CALL hid_read(hid_device* d, unsigned char* data, size_t n) {
  auto* h = reinterpret_cast<fake::Handle*>(d);
  if (h->gone) return -1;
  if (h->input.empty()) {
    if (fake::endPass) fake::endPass();
    return 0;
  }
  const auto r = h->input.front();
  h->input.pop_front();
  const size_t count = std::min(n, r.size());
  std::memcpy(data, r.data(), count);
  return int(count);
}
int HID_API_CALL hid_write(hid_device* d, const unsigned char* data, size_t n) {
  auto* h = reinterpret_cast<fake::Handle*>(d);
  if (h->gone) return -1;
  h->written.emplace_back(data, data + n);
  return int(n);
}
}

namespace lh::usb {
struct UsbTransportTestAccess {
  static void Scan(UsbTransport& t) { t.ScanDoorman(); }
  static void Pump(UsbTransport& t, double now) { t.PumpDoorman(now); }
  // One reader pass (commands, then the dongles); ends at the first empty read.
  static void Pass(UsbTransport& t) {
    fake::endPass = [&t] { t.stop_ = true; };
    t.stop_ = false;
    t.Run();
    fake::endPass = {};
  }
  static RawCaptureWriter& Capture(UsbTransport& t) { return t.capture_; }
};
}  // namespace lh::usb

namespace {
using Access = lh::usb::UsbTransportTestAccess;
namespace fs = std::filesystem;

// Made-up devices: LHR-0000A001 ... (a config for each in the fake Steam folder).
constexpr uint32_t kA1 = 0xA001, kA2 = 0xA002, kB1 = 0xB001;
std::string Serial(uint32_t id) {
  char s[16];
  std::snprintf(s, sizeof s, "LHR-%08X", unsigned(id));
  return s;
}

struct Sink : lh::InputSinkBase {
  lh::usb::UsbTransport* transport = nullptr;
  std::vector<std::string> attached, detached, logs;
  void OnDeviceAttached(const lh::DeviceId& id, const std::string&) override { attached.push_back(id); }
  // SessionImpl forwards detach notifications to the transport.
  void OnDeviceDetached(const lh::DeviceId& id) override {
    detached.push_back(id);
    if (transport) transport->OnDeviceGone(id);
  }
  void OnTransportLog(lh::LogLevel, const std::string& text) override { logs.push_back(text); }
  size_t Count(const std::string& line) const { return size_t(std::count(logs.begin(), logs.end(), line)); }
  size_t CountContaining(const std::string& part) const {
    return size_t(std::count_if(logs.begin(), logs.end(), [&](const auto& l) { return l.find(part) != std::string::npos; }));
  }
};

// Dongle -> host reports.
std::vector<uint8_t> Live(uint8_t active, uint8_t used = 0x3f) {   // command 0x29's reply: each slot's state
  std::vector<uint8_t> r(63);
  r[0] = 0x29;
  for (int i = 0; i < 6; ++i) r[3 + i * 10] = uint8_t(((used >> i) & 1) | (((active >> i) & 1) << 1));
  return r;
}
std::vector<uint8_t> IdentFrame(uint32_t id) {   // our tracker firmware's 'H': serial, IMU ranges, hw id
  return {'H', 1, uint8_t(id), uint8_t(id >> 8), uint8_t(id >> 16), uint8_t(id >> 24), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}
std::vector<uint8_t> Stream(int slot, const std::vector<uint8_t>& frame) {   // one whole frame in a 0x80 report
  std::vector<uint8_t> r = {0x80, uint8_t(slot), uint8_t(frame.size() + 1), uint8_t(frame.size())};
  r.insert(r.end(), frame.begin(), frame.end());
  return r;
}
std::vector<uint8_t> Ident(int slot, uint32_t id) { return Stream(slot, IdentFrame(id)); }
std::vector<uint8_t> Imu(int slot, uint8_t mark) {   // 'p': one IMU sample (17 bytes)
  std::vector<uint8_t> f(18, mark);
  f[0] = 'p';
  return Stream(slot, f);
}
// Host -> dongle commands on interface 0: [0][cmd][seq][args...].
size_t Sent(const std::string& path, uint8_t cmd, std::function<bool(const std::vector<uint8_t>&)> match = {}) {
  size_t n = 0;
  for (const auto& h : fake::handles)
    if (h->path == path)
      for (const auto& w : h->written)
        if (w.size() > 3 && w[1] == cmd && (!match || match(w))) ++n;
  return n;
}
// A radio downlink (0x21) to a slot carrying a 'P' frame (a feature report or command).
size_t Downlinks(const std::string& path, int slot) {
  return Sent(path, 0x21, [&](const auto& w) { return w[3] == slot && w[5] == 'P'; });
}
size_t PairCommands(const std::string& path, int slotArg) {
  return Sent(path, 0x24, [&](const auto& w) { return w[4] == slotArg; });
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

class Doorman : public testing::Test {
 protected:
  fs::path configs;
  Sink sink;
  std::unique_ptr<lh::usb::UsbTransport> transport;
  double t = 0;

  void SetUp() override {
    fake::Reset();
    configs = fs::temp_directory_path() / ("lh-doorman-dongles-test-" + std::to_string(uint64_t(Now() * 1e6)));
    for (uint32_t id : {kA1, kA2, kB1}) {   // the fake Steam config cache: no config is read over the radio
      std::string lower = Serial(id);
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      fs::create_directories(configs / lower);
      std::ofstream(configs / lower / "config.json") << "{\"device_serial_number\":\"" << Serial(id) << "\"}";
    }
    lh::usb::UsbTransportOptions options;
    options.steamLighthouseConfigDir = configs.string();
    transport = std::make_unique<lh::usb::UsbTransport>(&sink, options);
    sink.transport = transport.get();
    t = Now();
  }
  void TearDown() override {
    if (transport) transport->Stop();   // null if SetUp failed
    transport.reset();
    std::error_code ec;
    fs::remove_all(configs, ec);
    fake::Reset();
  }
  // A dongle's interfaces: <name>0, and <name>1 unless it has none.
  static void Plug(const std::string& name, const std::wstring& serial, bool interface1 = true) {
    fake::plugged.push_back({name + "0", serial, 0});
    if (interface1) fake::plugged.push_back({name + "1", serial, 1});
  }
  // The probe's scan, then a reader pass of the dongles, 0.3 s later than the last.
  void Step() {
    t += 0.3;
    Access::Scan(*transport);
    Access::Pump(*transport, t);
  }
  static void Queue(const std::string& path, std::vector<uint8_t> report) {
    fake::Handle* h = fake::Open(path);
    ASSERT_NE(h, nullptr) << path << " is not open";
    h->input.push_back(std::move(report));
  }
  std::map<std::string, std::string> Slots() const {   // receiver -> device ("" none), Doorman slots only
    std::map<std::string, std::string> out;
    for (const auto& r : transport->Receivers())
      if (r.kind == "openhouse") out[r.serial] = r.connected;
    return out;
  }
  size_t EndpointsOf(const std::string& device) const {
    const auto eps = transport->Endpoints();
    return size_t(std::count_if(eps.begin(), eps.end(), [&](const auto& e) { return e.serial == device; }));
  }
  std::string ImuConfigOf(const std::string& device) const {   // the config its Imu endpoint was set up with
    for (const auto& e : transport->Endpoints())
      if (e.serial == device && e.kind == lh::usb::EndpointKind::Imu) return e.configJson;
    return "";
  }
  // `device` set up in `slot` of the dongle whose interfaces are <name>0/<name>1.
  void Connect(const std::string& name, int slot, uint32_t device, uint8_t active) {
    Queue(name + "0", Live(active));
    Step();
    Queue(name + (slot % 2 && fake::Open(name + "1") ? "1" : "0"), Ident(slot, device));
    Step();
    Step();   // the slot table (Receivers) follows on the next stats refresh
  }
};

TEST(DoormanFind, InterfacesArePairedBySerialWhateverTheOrder) {
  fake::Reset();
  fake::plugged = {{"B1", L"TESTB", 1}, {"A0", L"TESTA", 0}, {"B0", L"TESTB", 0}, {"A1", L"TESTA", 1}};
  std::map<std::string, int> shared;
  const auto found = lh::usb::ScanDoormanDongles(&shared);
  ASSERT_EQ(found.size(), 2u);
  EXPECT_EQ(found[0].serial, "OHDG-TESTA");
  EXPECT_EQ(found[0].path, "A0");
  EXPECT_EQ(found[0].path1, "A1");
  EXPECT_EQ(found[1].serial, "OHDG-TESTB");
  EXPECT_EQ(found[1].path, "B0");
  EXPECT_EQ(found[1].path1, "B1");
  EXPECT_TRUE(shared.empty());
  fake::Reset();
}

TEST(DoormanFind, SharedOrMissingSerialsAreNotGuessed) {
  fake::Reset();
  // Two dongles without a serial, two with the same one, one alone without a serial is fine
  // elsewhere (EmptySerialDongleWorksAlone); interface 1 without its interface 0 waits.
  fake::plugged = {{"E0", L"", 0}, {"F1", L"", 1}, {"F0", L"", 0}, {"E1", L"", 1},
                   {"T0", L"TWIN", 0}, {"U0", L"TWIN", 0}, {"T1", L"TWIN", 1}, {"U1", L"TWIN", 1},
                   {"L1", L"LATE", 1}, {"A0", L"TESTA", 0}};
  std::map<std::string, int> shared;
  const auto found = lh::usb::ScanDoormanDongles(&shared);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].serial, "OHDG-TESTA");
  EXPECT_EQ(found[0].path1, "");
  EXPECT_EQ(shared, (std::map<std::string, int>{{"OHDG-", 2}, {"OHDG-TWIN", 2}}));
  fake::Reset();
}

TEST_F(Doorman, TwoDonglesEnumeratedInterleavedEachReadItsOwnOddSlots) {
  fake::plugged = {{"B1", L"TESTB", 1}, {"A0", L"TESTA", 0}, {"B0", L"TESTB", 0}, {"A1", L"TESTA", 1}};
  Step();
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTA opened"), 1u);
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTB opened"), 1u);
  ASSERT_NE(fake::Open("A1"), nullptr);
  ASSERT_NE(fake::Open("B1"), nullptr);
  Queue("A0", Live(0b10));
  Queue("B0", Live(0b10));
  Step();
  Queue("A1", Ident(1, kA1));   // slot 1's stream comes on each dongle's own interface 1
  Queue("B1", Ident(1, kB1));
  Step();
  Step();
  EXPECT_EQ(sink.Count(Serial(kA1) + ": connected on Doorman Dongle OHDG-TESTA slot 1"), 1u);
  EXPECT_EQ(sink.Count(Serial(kB1) + ": connected on Doorman Dongle OHDG-TESTB slot 1"), 1u);
  const auto slots = Slots();
  EXPECT_EQ(slots.size(), 12u);   // six per dongle
  EXPECT_EQ(slots.at("OHDG-TESTA/1"), Serial(kA1));
  EXPECT_EQ(slots.at("OHDG-TESTB/1"), Serial(kB1));
}

TEST_F(Doorman, EmptySlotsOfEveryDongleAreOfferedBeforePairedOnes) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Queue("A0", Live(0, 0b111111));   // A: all six paired
  Queue("B0", Live(0, 0b000001));   // B: slot 0 paired
  Step();
  Step();
  std::vector<std::string> order;
  for (const auto& r : transport->Receivers()) order.push_back(r.serial + (r.paired ? "*" : ""));
  ASSERT_EQ(order.size(), 12u);
  EXPECT_EQ(std::vector<std::string>(order.begin(), order.begin() + 5),
            (std::vector<std::string>{"OHDG-TESTB/1", "OHDG-TESTB/2", "OHDG-TESTB/3", "OHDG-TESTB/4", "OHDG-TESTB/5"}));
  EXPECT_EQ(order[5], "OHDG-TESTA/0*");
  EXPECT_EQ(order.back(), "OHDG-TESTB/0*");
}

TEST_F(Doorman, DongleWithoutInterface1StreamsEverySlotOnInterface0) {
  Plug("A", L"TESTA", false);   // older dongle firmware
  Step();
  Connect("A", 1, kA1, 0b10);
  EXPECT_EQ(sink.Count(Serial(kA1) + ": connected on Doorman Dongle OHDG-TESTA slot 1"), 1u);
  EXPECT_EQ(Slots().at("OHDG-TESTA/1"), Serial(kA1));
  Queue("A0", Imu(1, 7));
  Step();
  EXPECT_TRUE(fake::Open("A0")->input.empty());
  // An interface 1 listed only after interface 0 was opened (plugged in between two scans).
  fake::plugged.push_back({"A1", L"TESTA", 1});
  Step();
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTA: interface 1 opened"), 1u);
  Queue("A0", Live(0b1010));
  Step();
  Queue("A1", Ident(3, kA2));
  Step();
  EXPECT_EQ(sink.Count(Serial(kA2) + ": connected on Doorman Dongle OHDG-TESTA slot 3"), 1u);
}

TEST_F(Doorman, EmptySerialDongleWorksAlone) {
  Plug("E", L"");
  Step();
  Connect("E", 1, kA1, 0b10);
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG- opened"), 1u);
  EXPECT_EQ(Slots().at("OHDG-/1"), Serial(kA1));
  ASSERT_TRUE(transport->SetPairing("OHDG-/2", 5000));
  Step();
  EXPECT_EQ(PairCommands("E0", 3), 1u);
}

TEST_F(Doorman, DonglesThatCantBeToldApartAreNotOpened) {
  Plug("E", L"");
  Plug("F", L"");
  Plug("T", L"TWIN");
  Plug("U", L"TWIN");
  Plug("A", L"TESTA");
  Step();
  Step();
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTA opened"), 1u);
  EXPECT_EQ(sink.CountContaining("Doorman Dongle OHDG"), 1u);
  EXPECT_EQ(sink.Count("2 Doorman Dongles share the name OHDG- (the same USB serial): their interfaces can't be told "
                       "apart, so none of them is used"), 1u);   // warned once, not on every scan
  EXPECT_EQ(sink.Count("2 Doorman Dongles share the name OHDG-TWIN (the same USB serial): their interfaces can't be "
                       "told apart, so none of them is used"), 1u);
  for (const char* path : {"E0", "E1", "F0", "F1", "T0", "T1", "U0", "U1"}) EXPECT_EQ(fake::Open(path), nullptr) << path;
  // One of the twins goes: the other is unambiguous now.
  fake::Unplug("U");
  Step();
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TWIN opened"), 1u);
  EXPECT_NE(fake::Open("T1"), nullptr);
}

TEST_F(Doorman, UnplugOneOfTwoKeepsTheOtherAndReplugReopensIt) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Connect("A", 0, kA1, 0b1);
  Connect("B", 0, kB1, 0b1);
  ASSERT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1), Serial(kB1)}));

  fake::Unplug("A");
  Step();
  EXPECT_EQ(sink.detached, (std::vector<std::string>{Serial(kA1)}));
  EXPECT_EQ(sink.Count(Serial(kA1) + ": disconnected from Doorman Dongle OHDG-TESTA slot 0"), 1u);
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTA closed"), 1u);
  auto slots = Slots();
  EXPECT_EQ(slots.size(), 6u);
  EXPECT_EQ(slots.at("OHDG-TESTB/0"), Serial(kB1));
  EXPECT_FALSE(transport->SetPairing("OHDG-TESTA", 5000));
  EXPECT_EQ(EndpointsOf(Serial(kA1)), 0u);
  EXPECT_EQ(EndpointsOf(Serial(kB1)), 3u);
  // B carries on: its data is read, its device takes commands.
  Queue("B0", Imu(0, 1));
  transport->Haptic(Serial(kB1), 160, 1.0, 0.01);
  Access::Pass(*transport);
  EXPECT_EQ(Downlinks("B0", 0), 1u);
  EXPECT_TRUE(fake::Open("B0")->input.empty());

  // Back: opened again, as a new dongle (its devices identify again).
  Plug("A", L"TESTA");
  for (int i = 0; i < 8 && !fake::Open("A0"); ++i) Step();   // at most one open attempt every 2 s, as before
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTA opened"), 2u);
  Connect("A", 0, kA1, 0b1);
  EXPECT_EQ(sink.Count(Serial(kA1) + ": connected on Doorman Dongle OHDG-TESTA slot 0"), 2u);
  EXPECT_EQ(Slots().at("OHDG-TESTA/0"), Serial(kA1));
  EXPECT_EQ(EndpointsOf(Serial(kA1)), 3u);
}

TEST_F(Doorman, SameDeviceOnTwoDonglesIsSetUpOnce) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Connect("A", 0, kA1, 0b1);
  const auto before = transport->Endpoints();
  // The same tracker shows up on B (paired to both, B's slot still active).
  Connect("B", 2, kA1, 0b100);
  Step();
  Step();
  const std::string duplicate =
      Serial(kA1) + ": also on Doorman Dongle OHDG-TESTB slot 2; kept on Doorman Dongle OHDG-TESTA slot 0";
  EXPECT_EQ(sink.Count(duplicate), 1u);   // once, not on every pass
  EXPECT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1)}));
  EXPECT_EQ(EndpointsOf(Serial(kA1)), 3u);
  EXPECT_EQ(Slots().at("OHDG-TESTB/2"), "");
  transport->Haptic(Serial(kA1), 160, 1.0, 0.01);
  Access::Pass(*transport);
  EXPECT_EQ(Downlinks("A0", 0), 1u);
  EXPECT_EQ(Downlinks("B0", 2), 0u);

  // It leaves A: B takes it over.
  Queue("A0", Live(0));
  Step();
  Step();
  Step();
  EXPECT_EQ(sink.Count(Serial(kA1) + ": disconnected from Doorman Dongle OHDG-TESTA slot 0"), 1u);
  EXPECT_EQ(sink.Count(Serial(kA1) + ": connected on Doorman Dongle OHDG-TESTB slot 2"), 1u);
  EXPECT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1), Serial(kA1)}));
  EXPECT_EQ(sink.detached, (std::vector<std::string>{Serial(kA1)}));
  EXPECT_EQ(EndpointsOf(Serial(kA1)), 3u);
  EXPECT_EQ(sink.CountContaining("no config"), 0u);   // set up on B with the config it waited with
  EXPECT_NE(ImuConfigOf(Serial(kA1)), "");
  const auto after = transport->Endpoints();
  for (const auto& e : after)
    for (const auto& b : before) EXPECT_NE(e.ep, b.ep);   // new endpoint ids, never reused
  const auto slots = Slots();
  EXPECT_EQ(slots.at("OHDG-TESTA/0"), "");
  EXPECT_EQ(slots.at("OHDG-TESTB/2"), Serial(kA1));
  transport->Haptic(Serial(kA1), 160, 1.0, 0.01);
  Access::Pass(*transport);
  EXPECT_EQ(Downlinks("B0", 2), 1u);
  // Back on A while B has it: now A waits.
  Queue("A0", Live(0b1));
  Step();
  Queue("A0", Ident(0, kA1));
  Step();
  EXPECT_EQ(sink.Count(Serial(kA1) + ": also on Doorman Dongle OHDG-TESTA slot 0; kept on Doorman Dongle OHDG-TESTB slot 2"), 1u);
  EXPECT_EQ(EndpointsOf(Serial(kA1)), 3u);
}

TEST_F(Doorman, AnotherIdentityOnAWaitingSlotChangesNothing) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Connect("A", 0, kA1, 0b1);
  Connect("B", 2, kA1, 0b100);   // waits: A has it
  const std::string duplicate =
      Serial(kA1) + ": also on Doorman Dongle OHDG-TESTB slot 2; kept on Doorman Dongle OHDG-TESTA slot 0";
  ASSERT_EQ(sink.Count(duplicate), 1u);
  // The device sends its identity again: the slot keeps waiting, quietly.
  Queue("B0", Ident(2, kA1));
  Step();
  Step();
  EXPECT_EQ(sink.Count(duplicate), 1u);
  EXPECT_EQ(sink.CountContaining("reading device config"), 0u);
  EXPECT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1)}));
  EXPECT_EQ(Slots().at("OHDG-TESTB/2"), "");
}

TEST_F(Doorman, UnplugAndReplugADongleThatWaitsForADevice) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Connect("A", 0, kA1, 0b1);
  Connect("B", 2, kA1, 0b100);   // waits: A has it
  // B goes while its slot waits: nothing is set up for it, under any name.
  fake::Unplug("B");
  Step();
  Step();
  EXPECT_EQ(sink.Count("Doorman Dongle OHDG-TESTB closed"), 1u);
  EXPECT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1)}));
  EXPECT_EQ(EndpointsOf(""), 0u);
  // Back: the slot starts over (its device identifies again), waits again, and takes the
  // device over once A lets it go.
  Plug("B", L"TESTB");
  for (int i = 0; i < 8 && !fake::Open("B0"); ++i) Step();
  ASSERT_NE(fake::Open("B0"), nullptr);
  Queue("B0", Live(0b100));
  Step();
  Step();
  EXPECT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1)}));
  EXPECT_EQ(EndpointsOf(""), 0u);
  Queue("B0", Ident(2, kA1));
  Step();
  EXPECT_EQ(sink.Count(Serial(kA1) + ": also on Doorman Dongle OHDG-TESTB slot 2; kept on Doorman Dongle OHDG-TESTA slot 0"), 2u);
  Queue("A0", Live(0));
  Step();
  Step();
  EXPECT_EQ(sink.Count(Serial(kA1) + ": connected on Doorman Dongle OHDG-TESTB slot 2"), 1u);
  EXPECT_EQ(sink.attached, (std::vector<std::string>{Serial(kA1), Serial(kA1)}));
  EXPECT_EQ(EndpointsOf(Serial(kA1)), 3u);
  EXPECT_EQ(sink.CountContaining("no config"), 0u);
  EXPECT_NE(ImuConfigOf(Serial(kA1)), "");
}

TEST_F(Doorman, EveryPerDeviceCallReachesTheDongleCarryingTheDevice) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Connect("A", 0, kA1, 0b1);
  Connect("B", 3, kB1, 0b1000);
  transport->Haptic(Serial(kA1), 160, 1.0, 0.01);
  transport->PowerOff(Serial(kB1));
  Access::Pass(*transport);
  EXPECT_EQ(Downlinks("A0", 0), 1u);
  EXPECT_EQ(Downlinks("B0", 3), 1u);
  EXPECT_EQ(Downlinks("A0", 3) + Downlinks("B0", 0), 0u);
  transport->SetOpticalSubProtocol(Serial(kB1), 1);   // light mode: two reports (mode, verbosity)
  Access::Pass(*transport);
  EXPECT_EQ(Downlinks("B0", 3), 3u);
  EXPECT_EQ(Downlinks("A0", 0), 1u);

  ASSERT_TRUE(transport->SetPairing("OHDG-TESTB/4", 5000));
  ASSERT_TRUE(transport->SetPairing("OHDG-TESTA", 5000));
  EXPECT_FALSE(transport->SetPairing("OHDG-TESTC", 5000));
  Step();
  EXPECT_EQ(PairCommands("B0", 5), 1u);   // slot + 1
  EXPECT_EQ(PairCommands("A0", 0), 1u);   // 0: the dongle's first free slot
  EXPECT_EQ(Sent("A0", 0x24) + Sent("B0", 0x24), 2u);

  // The session's OnDeviceGone frees the slot that showed the device at once.
  transport->OnDeviceGone(Serial(kB1));
  const auto slots = Slots();
  EXPECT_EQ(slots.at("OHDG-TESTB/3"), "");
  EXPECT_EQ(slots.at("OHDG-TESTA/0"), Serial(kA1));
}

TEST_F(Doorman, PairingNamesTheWholeDongleSerial) {
  Plug("P", L"12");
  Plug("Q", L"123");
  Step();
  ASSERT_TRUE(transport->SetPairing("OHDG-123/3", 5000));
  Step();
  EXPECT_EQ(PairCommands("Q0", 4), 1u);
  EXPECT_EQ(Sent("P0", 0x24), 0u);
  ASSERT_TRUE(transport->SetPairing("OHDG-12/3", 5000));
  EXPECT_FALSE(transport->SetPairing("OHDG-12/9", 5000));
  Step();
  EXPECT_EQ(PairCommands("P0", 4), 1u);
  EXPECT_EQ(Sent("Q0", 0x24), 1u);
}

TEST_F(Doorman, SplitFramesStayWithTheirDongle) {
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Queue("A0", Live(0b1));
  Queue("B0", Live(0b1));
  Step();
  // Each dongle's 'H' for slot 0 in two 0x81 parts, the parts of both in alternate passes.
  const auto part = [](uint32_t id, bool first) {
    std::vector<uint8_t> frame = IdentFrame(id);
    frame.insert(frame.begin(), uint8_t(frame.size()));
    std::vector<uint8_t> bytes = first ? std::vector<uint8_t>(frame.begin(), frame.begin() + 9)
                                       : std::vector<uint8_t>(frame.begin() + 9, frame.end());
    std::vector<uint8_t> r = {0x81, 0, uint8_t(bytes.size())};
    r.insert(r.end(), bytes.begin(), bytes.end());
    return r;
  };
  Queue("A0", part(kA1, true));
  Queue("B0", part(kB1, true));
  Step();
  Queue("A0", part(kA1, false));
  Queue("B0", part(kB1, false));
  Step();
  EXPECT_EQ(sink.Count(Serial(kA1) + ": connected on Doorman Dongle OHDG-TESTA slot 0"), 1u);
  EXPECT_EQ(sink.Count(Serial(kB1) + ": connected on Doorman Dongle OHDG-TESTB slot 0"), 1u);
  EXPECT_EQ(sink.attached.size(), 2u);
}

TEST_F(Doorman, ATwoDongleRecordingReplaysUnambiguously) {
  const fs::path file = configs / "two-dongles.lhraw";
  ASSERT_TRUE(Access::Capture(*transport).Open(file.string()));
  Plug("A", L"TESTA");
  Plug("B", L"TESTB");
  Step();
  Connect("A", 1, kA1, 0b10);   // the same slot number on both dongles
  Connect("B", 1, kB1, 0b10);
  Queue("A1", Imu(1, 0xA1));
  Queue("B1", Imu(1, 0xB1));
  Step();
  Access::Capture(*transport).Close();

  lh::usb::RawCaptureReader reader;
  ASSERT_TRUE(reader.Open(file.string()));
  std::map<uint16_t, lh::usb::EndpointInfo> eps;
  std::map<std::string, std::set<uint8_t>> marks;   // device -> the bytes its IMU reports carried
  ASSERT_TRUE(reader.ReadAll(
      [&](const lh::usb::EndpointInfo& e) { EXPECT_TRUE(eps.emplace(e.ep, e).second) << "endpoint id reused: " << e.ep; },
      [&](const lh::usb::RawReport& r) {
        ASSERT_TRUE(eps.count(r.ep)) << "report for an unknown endpoint " << r.ep;
        const auto& e = eps.at(r.ep);
        if (e.kind == lh::usb::EndpointKind::Imu && r.data.size() > 1) marks[e.serial].insert(r.data[1]);
      }));
  EXPECT_EQ(eps.size(), 6u);   // three per device
  std::map<std::string, int> perDevice;
  for (const auto& [ep, e] : eps) ++perDevice[e.serial];
  EXPECT_EQ(perDevice, (std::map<std::string, int>{{Serial(kA1), 3}, {Serial(kB1), 3}}));
  EXPECT_EQ(marks[Serial(kA1)], std::set<uint8_t>{0xA1});
  EXPECT_EQ(marks[Serial(kB1)], std::set<uint8_t>{0xB1});
}

}  // namespace
