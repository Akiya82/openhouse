// openhouse-installer's window: the home screen (install, uninstall, flash, restore) and each
// guided flow. Long work (copying, flashing, looking for devices) runs on worker threads; the
// UI only draws their state.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "drives.hpp"
#include "driver_install.hpp"
#include "radio.hpp"

namespace openhouse::installer {

// One piece of long work, on its own thread.
class Job {
 public:
  ~Job() { Join(); }
  void Start(std::function<bool(Job&)> work);
  bool Running() const { return running_; }
  bool Finished() const { return finished_; }
  bool Ok() const { return ok_; }
  void Reset() { Join(); finished_ = false; ok_ = false; std::lock_guard l(m_); log_.clear(); status_.clear(); error_.clear(); progress_ = 0; }
  // From the work:
  void Log(const std::string& line) { std::lock_guard l(m_); log_.push_back(line); }
  void Status(float progress, const std::string& text) { std::lock_guard l(m_); progress_ = progress; status_ = text; }
  void Fail(const std::string& error) { std::lock_guard l(m_); error_ = error; }
  // For the UI:
  std::vector<std::string> LogLines() const { std::lock_guard l(m_); return log_; }
  std::string StatusText() const { std::lock_guard l(m_); return status_; }
  std::string Error() const { std::lock_guard l(m_); return error_; }
  float Progress() const { std::lock_guard l(m_); return progress_; }

 private:
  void Join() { if (thread_.joinable()) thread_.join(); }
  std::thread thread_;
  std::atomic<bool> running_{false}, finished_{false}, ok_{false};
  mutable std::mutex m_;
  std::vector<std::string> log_;
  std::string status_, error_;
  float progress_ = 0;
};

// Runs `look` every `seconds` on a thread, keeping the latest result.
template <class T>
class Watcher {
 public:
  ~Watcher() { Stop(); }
  void Start(std::function<T()> look, double seconds) {
    Stop();
    stop_ = false;
    have_ = false;
    thread_ = std::thread([this, look, seconds] {
      while (!stop_) {
        T value = look();
        { std::lock_guard l(m_); value_ = std::move(value); have_ = true; }
        for (int i = 0; i < int(seconds * 10) && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  void Stop() { stop_ = true; if (thread_.joinable()) thread_.join(); }
  T Value() const { std::lock_guard l(m_); return value_; }
  bool Have() const { return have_; }

 private:
  std::thread thread_;
  std::atomic<bool> stop_{true}, have_{false};
  mutable std::mutex m_;
  T value_{};
};

class Ui {
 public:
  Ui();
  static void SetUpImGui();
  void SetLogo(uint64_t texture) { logo_ = texture; }
  void Draw(float width, float height);

 private:
  enum class Page {
    Home, Install, Uninstall, FlashMenu, BaseStation, DoormanMenu, Dongle, RadioMenu, RadioWired, RadioWireless,
    RestoreMenu, RestoreBase, RestoreRadioMenu, RestoreRadio, RestoreRadioWireless
  };
  void Go(Page page);
  void Header();
  void Footer();
  void SteamVRPrompt();
  bool RuntimesClosed();
  bool DeviceAccess();  // Linux: draws the udev notice; true when this user can open devices  // draws the "close SteamVR" notice; true when nothing is running
  void Back(Page to);

  void Home();
  void InstallPage(bool install);
  void FlashMenu();
  void DoormanMenu();
  void RadioMenu();
  void RestoreMenu();
  void BaseStationPage(bool restore);
  void DonglePage();
  void RadioWiredPage(bool restore);
  void RadioWirelessPage(bool restore);
  void RestoreRadioMenu();

  void RefreshStatus();

  Page page_ = Page::Home;
  uint64_t logo_ = 0;
  SteamVR steamvr_;
  DriverStatus status_;
  bool askSteamVR_ = false;
  char steamvrInput_[1024] = {};
  std::string steamvrError_;
  std::vector<std::string> running_;
  double runningChecked_ = -1, accessChecked_ = -1;
  bool haveAccess_ = true;
  Job job_, closeJob_;
  bool monadoCheck_ = true;
  bool statusFresh_ = true;
  int installAction_ = 0;  // 0 install, 1 update, 2 reinstall

  // Base station flow
  enum class BaseStep { Warning, Waiting, Ready, Writing, Replug, Verifying, Done } baseStep_ = BaseStep::Warning;
  bool confirmed10_ = false;
  std::string baseImage_;      // what's written
  std::string baseImageName_;
  bool alreadyInstalled_ = false;
  Watcher<std::vector<Drive>> baseDrives_;
  Drive baseDrive_;
  bool sawUnplug_ = false;

  // Dongle flow
  enum class DongleStep { Board, Detect, Flashing, Done } dongleStep_ = DongleStep::Board;
  struct DongleSeen { std::vector<Uf2Drive> drives; std::vector<DoormanDongleInfo> dongles; };
  Watcher<DongleSeen> dongleSeen_;
  std::string dongleResult_;

  // Radio flows
  enum class RadioStep { Choose, Flashing, Done } radioStep_ = RadioStep::Choose;
  Watcher<std::vector<WiredDevice>> wired_;
  Watcher<std::vector<WirelessDevice>> wireless_;
  std::set<std::string> checked_;
  std::set<std::string> accepted_;  // unknown devices the user accepted to flash anyway
  std::vector<StockModel> stockModels_;
  std::map<std::string, int> stockChoice_;  // doorman-dongle-manager device -> stockModels_ index
  std::vector<std::pair<std::string, std::string>> results_;  // device, result
  std::atomic<int> current_{0};
  int total_ = 0;
};

}  // namespace openhouse::installer
