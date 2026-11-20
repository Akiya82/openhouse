// Doorman Dongle (6767:0727, nRF52840): up to 6 trackers over our own radio link
// (doorman: doorman/common/doorman_link.h, ai/doorman/protocol.md).
//
// The dongle streams each device's main-MCU frames (the radio SPI protocol shared by our
// Tracker 1.0 firmware and Valve's main firmwares on our radio). They are the wired reports with
// another first byte, so each is re-labelled and fed to the wired parsers:
//   'p' -> 0x20 IMU slot     'q' -> 0x21 LH1 light records   's' -> 0x28 LH2 stream
//   'Q' -> buttons 0x0100    'n' -> buttons 0x0700 (Index: input + 0xa1 record)
//   'V' -> other buttons-interface reports (0x0400 battery, ...)   'B' -> battery (ours)
//   'P' -> feature report replies (config read etc.; requests go down as 'P' frames)
// Identity: our tracker firmware sends 'H' (serial, IMU ranges, hw id); any other device is
// identified from itself: feature 0x01 (IMU ranges) and the config's device_serial_number.
// Each device becomes virtual wired endpoints (Imu + Optical + Buttons).
//
// Runs on the UsbTransport reader thread (Pump); config reads run on a worker thread.
// Several dongles can run side by side: one DoormanDongle each, keyed by USB serial.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lh/input_sink.hpp"
#include "lh/log_level.hpp"
#include "raw_capture.hpp"

struct hid_device_;
struct hid_device_info;

namespace lh::usb {

class ReportDecoder;

// One Doorman Dongle as hidapi lists it: interface 0 (commands, replies, even slots' stream)
// and, with newer dongle firmware, interface 1 (odd slots' stream) with the same USB serial.
struct DoormanDongleFound {
  std::string serial;  // "OHDG-" + USB serial: the dongle's receiver name
  std::string path;    // interface 0
  std::string path1;   // interface 1; "" if it has none (older firmware: every slot on interface 0)
};
// The Doorman Dongles in an enumeration, by serial. Interface 1 goes with the interface 0 of
// the same USB serial, wherever each is in the list. A serial carried by more than one
// interface 0 or interface 1 (an empty serial counts as one) can't be matched: it is left
// out, and *shared gets it with how many dongles share it.
std::vector<DoormanDongleFound> FindDoormanDongles(const hid_device_info* list,
                                                   std::map<std::string, int>* shared = nullptr);
// hid_enumerate for the dongle's USB id, then FindDoormanDongles.
std::vector<DoormanDongleFound> ScanDoormanDongles(std::map<std::string, int>* shared = nullptr);

// Which dongle carries each device. A device paired to several dongles can show up on two at
// once (it moves between them): the first keeps it, the others wait until it lets go, so a
// device is never set up twice. Reader thread only (and Stop, after the reader has ended).
class DoormanCarriers {
 public:
  struct Carrier { std::string dongle; int slot = -1; };
  // Takes `device` for dongle/slot; if another dongle or slot has it, that one (nothing taken).
  std::optional<Carrier> Take(const DeviceId& device, const std::string& dongle, int slot);
  void Release(const DeviceId& device, const std::string& dongle, int slot);

 private:
  std::map<DeviceId, Carrier> carriers_;
};

class DoormanDongle {
 public:
  using LogFn = std::function<void(LogLevel, const std::string&)>;
  // serial: "OHDG-..." (DoormanDongleFound::serial). carriers: shared by every dongle of one
  // transport (nullptr: no check).
  DoormanDongle(std::string serial, IInputSink* sink, ReportDecoder* decoder, RawCaptureWriter* capture,
                std::atomic<uint16_t>* nextEp, std::string steamConfigDir, LogFn log, DoormanCarriers* carriers = nullptr);
  ~DoormanDongle();

  // Reader thread. Opens the dongle's interfaces (at most one attempt every 2 s); true if open.
  bool Open(const DoormanDongleFound& found, double now);
  // Reader thread. Interface 1 listed after interface 0 was opened without it (plugged in
  // between two enumerations): opened now.
  void OpenInterface1(const std::string& path1);
  bool IsOpen() const { return dev_ != nullptr; }   // reader thread
  bool HasInterface1() const { return dev1_ != nullptr; }   // reader thread
  void Pump(double now);   // reader thread: read/dispatch; closes the dongle on a read or write error
  void Close();            // detaches every tracker
  std::vector<EndpointInfo> Endpoints() const;
  // A feature report (SET) for a device on this dongle, report id first. [0xff][cmd][len]...
  // reports are sized by their len byte, others by `size`. False: not ours.
  bool Command(const DeviceId& serial, const std::array<uint8_t, 64>& report, const char** status, size_t size = 0);
  // Pairing mode; slot >= 0: into that slot (replacing what's paired there; dongle fw >= 4),
  // -1: the dongle's first free slot. False while the dongle isn't open.
  bool Pair(int milliseconds, int slot = -1);
  const std::string& Serial() const { return serial_; }   // any thread; also while it isn't open
  std::vector<DeviceId> Connected() const;
  // The device was reported gone: its slot shows none until the next stats refresh says otherwise.
  void DeviceGone(const DeviceId& serial);
  // Link stats of each paired slot (dongle command 0x23, polled each second). Any thread.
  struct SlotStat {
    int slot = 0; bool used = false, active = false; DeviceId serial;
    double kBytesPerSec = 0, framesPerSec = 0, lossPercent = -1, dropsPerSec = 0;
    int rssiDongle = 0, rssiDevice = 0, devicesOnDongle = 0;
  };
  std::vector<SlotStat> SlotStats() const;

 private:
  struct Slot {
    bool used = false, active = false;
    int identSeq = -1;     // seq of the pending ident request
    double identAskT = 0;
    int identTries = 0;
    bool noIdent = false;  // no 'H' identity: identify from feature 0x01 + config
    bool attached = false;
    // Its device is on another dongle or slot (DoormanCarriers): set up here once that one
    // lets go, with this config.
    bool waiting = false;
    std::string waitConfig;
    DeviceId serial;
    int8_t gyro = -1, accel = -1;
    int sensorType = -1;
    uint16_t epImu = 0, epOpt = 0, epBtn = 0;
    std::array<std::array<uint8_t, 17>, 3> ring{};
    int ringN = 0, ringPos = 0;
    // config read over the radio
    std::thread worker;
    std::atomic<bool> workerDone{false};
    std::string fetchedConfig, fetchedSerial;
    int8_t fetchedGyro = -1, fetchedAccel = -1;
    int fetchedSensorType = -1;
    // link stats: last raw 0x23 counters and the rates derived from them
    bool haveStats = false; double statsT = 0;
    uint32_t polls = 0, respOk = 0, framesUp = 0, bytesUp = 0, drops = 0;
    double kBps = 0, fps = 0, loss = -1, dps = 0; int rssiD = 0, rssiT = 0;   // from feature 0x05 (devices without 'H', e.g. stock main firmware)
  };
  struct FeatureWait {
    int slot = -1;
    uint8_t id = 0, tag = 0;
    bool sent = false, done = false;
    std::vector<uint8_t> reply;
  };

  void Drop();
  void Send(uint8_t cmd, const uint8_t* args, size_t n);
  void OnReport(double t, const uint8_t* r, int n, bool last);
  void OnFrame(int slot, double t, const uint8_t* f, int n, bool last);
  void OnIdent(int slot, const uint8_t* f, int n);
  void Attach(int slot, const std::string& config);
  void Detach(int slot);
  void StartConfigFetch(int slot);
  int FeatureRead(int slot, uint8_t* data, size_t size);  // worker thread
  void Downlink(int slot, const uint8_t* frame, size_t n);
  void RefreshStatsCache();

  const std::string serial_;
  IInputSink* sink_;
  ReportDecoder* decoder_;
  RawCaptureWriter* capture_;
  std::atomic<uint16_t>* nextEp_;
  std::string steamConfigDir_;
  LogFn log_;
  DoormanCarriers* carriers_;

  hid_device_* dev_ = nullptr;
  hid_device_* dev1_ = nullptr;   // interface 1: odd slots' stream (newer dongle firmware)
  double nextOpen_ = 0, nextLive_ = 0, nextStats_ = 0;
  int statsSlot_ = 0, statsSeq_ = -1;
  uint8_t seq_ = 0;
  Slot slots_[6];
  std::vector<uint8_t> frag_[6];      // a frame split over several stream reports, per slot

  mutable std::mutex mutex_;          // featureWaits_, downlinks_, pairMs_, opened_, slot serials
  bool opened_ = false;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<FeatureWait>> featureWaits_;
  uint8_t nextTag_ = 0;
  std::array<double, 6> readsAfter_{};   // Pump thread
  std::deque<std::pair<int, std::vector<uint8_t>>> downlinks_;
  std::optional<int> pairMs_;
  int pairSlot_ = -1;
  std::vector<EndpointInfo> eps_;
  std::vector<SlotStat> statsCache_;   // under mutex_
};

}  // namespace lh::usb
