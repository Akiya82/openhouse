#include "base_channels.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <memory>
#include <utility>

#include "platform/platform.hpp"

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace openhouse::overlay {
namespace {

auto BackendPath() {
  const auto override = lh::platform::Env("OPENHOUSE_LIGHTHOUSE_BLE");
#ifdef _WIN32
  constexpr const char* filename = "lighthouse.dll";
#else
  constexpr const char* filename = "liblighthouse.so";
#endif
  return lh::platform::FsPath(override.empty() ? lh::platform::DataDirectory() + "/" + filename : override);
}

constexpr int kChannels = 16;
using Handle = uint64_t;

// The optional ShayBox/Lighthouse 2.1 C ABI. Keep the module loaded after it starts
// its async runtime so its worker threads can finish safely on process exit.
struct Ble {
#ifdef _WIN32
  HMODULE module = nullptr;
#else
  void* module = nullptr;
#endif
  uint32_t (*version)() = nullptr;
  int (*init)() = nullptr;
  int (*adapters)(Handle*, uint32_t*) = nullptr;
  int (*scan)(Handle, uint32_t, const char* const*, uint32_t, Handle*, uint32_t*) = nullptr;
  int (*name)(Handle, const char**) = nullptr;
  int (*id)(Handle, const char**) = nullptr;
  int (*set)(Handle, Handle, uint32_t, const char*, uint32_t, uint32_t) = nullptr;
  int (*release)(Handle) = nullptr;
  void (*freeString)(char*) = nullptr;
  const char* (*lastError)() = nullptr;

  template<class T>
  void Load(T& fn, const char* symbol) {
#ifdef _WIN32
    fn = reinterpret_cast<T>(GetProcAddress(module, symbol));
#else
    fn = reinterpret_cast<T>(dlsym(module, symbol));
#endif
  }

  Ble() {
    const auto path = std::filesystem::absolute(BackendPath());
#ifdef _WIN32
    module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!module) return;
    Load(version, "lighthouse_abi_version");
    Load(init, "lighthouse_init");
    Load(adapters, "lighthouse_discover_adapters");
    Load(scan, "lighthouse_scan");
    Load(name, "lighthouse_peripheral_name");
    Load(id, "lighthouse_peripheral_id");
    Load(set, "lighthouse_set_channel");
    Load(release, "lighthouse_release_handle");
    Load(freeString, "lighthouse_free_string");
    Load(lastError, "lighthouse_last_error");
  }

  bool Ready() const {
    return version && version() == 1 && init && adapters && scan && name && id && set && release && freeString && lastError;
  }

  std::string Error(const char* fallback) const {
    const char* error = lastError();
    return error && *error ? error : fallback;
  }
};

template <size_t N>
struct Handles {
  const Ble& api;
  std::array<Handle, N> values{};
  uint32_t count = N;
  ~Handles() { for (Handle handle : values) if (handle) api.release(handle); }
  Handle Take(Handle handle) {
    return std::exchange(*std::find(values.begin(), values.end(), handle), 0);
  }
};

struct Target {
  const Ble& api;
  Handle adapter = 0, peripheral = 0;
  ~Target() {
    if (peripheral) api.release(peripheral);
    if (adapter) api.release(adapter);
  }
};

bool RecentObservation(const viewer::Base& base, double now) {
  return base.seen && base.channelObservedAt >= 0 && base.channelObservedAt <= now &&
         now - base.channelObservedAt <= 30.0;
}
}  // namespace

int BaseChannel(const viewer::Base& base) {
  if (!base.lh2) return 0;
  int number = 0;
  const char* end = base.channel.data() + base.channel.size();
  const auto parsed = std::from_chars(base.channel.data(), end, number);
  return parsed.ec == std::errc{} && parsed.ptr == end && number >= 1 && number <= kChannels ? number : 0;
}

std::vector<int> FreeBaseChannels(const std::vector<viewer::Base>& bases) {
  std::bitset<kChannels + 1> used;
  for (const auto& base : bases) used.set(BaseChannel(base));
  std::vector<int> free;
  for (int ch = 1; ch <= kChannels; ++ch)
    if (!used[ch]) free.push_back(ch);
  return free;
}

std::vector<BaseChannelChange> ResolveBaseChannels(const viewer::State& state) {
  const auto& bases = state.bases;
  std::array<std::vector<std::string>, kChannels + 1> owners;
  for (const auto& base : bases)
    if (const int ch = BaseChannel(base); ch && RecentObservation(base, state.time))
      owners[ch].push_back(base.serial);
  const auto free = FreeBaseChannels(bases);
  std::vector<BaseChannelChange> changes;
  for (int ch = 1; ch <= kChannels; ++ch) {
    auto& serials = owners[ch];
    std::sort(serials.begin(), serials.end());
    for (size_t n = 1; n < serials.size() && changes.size() < free.size(); ++n)
      changes.push_back({serials[n], ch, free[changes.size()]});
  }
  return changes;
}

bool CanChangeBaseChannel(const viewer::State& state, const BaseChannelChange& change, bool automatic) {
  if (automatic) {
    const auto plan = ResolveBaseChannels(state);
    return !plan.empty() && plan.front() == change;
  }
  const auto free = FreeBaseChannels(state.bases);
  return std::find(free.begin(), free.end(), change.to) != free.end() &&
         std::any_of(state.bases.begin(), state.bases.end(), [&](const viewer::Base& base) {
           return base.serial == change.serial && change.from > 0 && BaseChannel(base) == change.from &&
                  RecentObservation(base, state.time);
         });
}

bool BaseChannelObserved(const viewer::State& state, const BaseChannelChange& change, double after) {
  return std::any_of(state.bases.begin(), state.bases.end(), [&](const viewer::Base& base) {
    return base.serial == change.serial && BaseChannel(base) == change.to &&
           RecentObservation(base, state.time) && base.channelObservedAt > after;
  });
}

bool HaveBaseChannelBackend() {
  std::error_code error;
  return std::filesystem::is_regular_file(BackendPath(), error);
}

BaseChannelDiscovery DiscoverBaseChannel(const BaseChannelChange& change) {
  if (change.to < 1 || change.to > kChannels || change.serial.size() != 12 ||
      !change.serial.starts_with("LHB-") ||
      !std::all_of(change.serial.begin() + 4, change.serial.end(), [](unsigned char c) { return std::isxdigit(c); }))
    return {{}, "Invalid base serial or channel"};
  const static Ble api;
  if (!api.Ready()) return {{}, "Lighthouse BLE 2.1 library missing or incompatible"};
  if (api.init() != 0) return {{}, api.Error("Bluetooth initialization failed")};
  Handles<8> adapters{api};
  if (api.adapters(adapters.values.data(), &adapters.count) != 0) return {{}, api.Error("No BLE adapter")};
  if (adapters.count == adapters.values.size()) return {{}, "Too many BLE adapters to verify the target"};
  std::shared_ptr<Target> target;
  std::string targetId;
  for (Handle adapter : adapters.values) {
    if (!adapter) continue;
    Handles<128> peripherals{api};
    if (api.scan(adapter, 4, nullptr, 0, peripherals.values.data(), &peripherals.count) != 0)
      return {{}, api.Error("Bluetooth scan failed")};
    if (peripherals.count == peripherals.values.size()) return {{}, "Bluetooth scan results reached their limit"};
    for (Handle peripheral : peripherals.values) {
      if (!peripheral) continue;
      const char* name = nullptr;
      if (api.name(peripheral, &name) != 0 || !name) continue;
      const bool matches = change.serial == name;
      api.freeString(const_cast<char*>(name));
      if (!matches) continue;
      const char* id = nullptr;
      if (api.id(peripheral, &id) != 0 || !id) return {{}, api.Error("Could not verify BLE device identity")};
      const std::string foundId(id);
      api.freeString(const_cast<char*>(id));
      if (foundId.empty()) return {{}, "BLE device identity is empty"};
      if (target) {
        if (foundId != targetId) return {{}, "Multiple BLE devices advertise " + change.serial};
        continue;
      }
      target = std::make_shared<Target>(api);
      target->adapter = adapters.Take(adapter);
      target->peripheral = peripherals.Take(peripheral);
      targetId = foundId;
    }
  }
  if (!target) return {{}, "Base station not found over BLE: " + change.serial};
  return {[target = std::move(target), channel = uint32_t(change.to)] {
    return target->api.set(target->adapter, target->peripheral, channel, nullptr, 1, 0) == 0
               ? std::string{} : target->api.Error("Channel write failed");
  }, {}};
}

}  // namespace openhouse::overlay
