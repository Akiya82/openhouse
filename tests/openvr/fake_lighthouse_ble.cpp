#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C"
#endif

namespace {
int scenario = 0, writes = 0;
const char* error = "";
int Text(const char** out, const char* value) {
  auto* copy = static_cast<char*>(std::malloc(std::strlen(value) + 1));
  if (!copy) return -7;
  std::memcpy(copy, value, std::strlen(value) + 1);
  *out = copy;
  return 0;
}
}  // namespace

EXPORT void lh_fake_ble_scenario(int value) { scenario = value; writes = 0; error = ""; }
EXPORT int lh_fake_ble_writes() { return writes; }
EXPORT uint32_t lighthouse_abi_version() { return 1; }
EXPORT int lighthouse_init() { return 0; }
EXPORT int lighthouse_discover_adapters(uint64_t* handles, uint32_t* count) {
  handles[0] = 1; handles[1] = 2;
  *count = 2;
  return 0;
}
EXPORT int lighthouse_scan(uint64_t adapter, uint32_t, const char* const*, uint32_t, uint64_t* handles, uint32_t* count) {
  if (adapter == 2 && scenario == 2) { error = "simulated scan failure"; return -4; }
  handles[0] = adapter == 1 ? 11 : 21;
  *count = 1;
  return 0;
}
EXPORT int lighthouse_peripheral_name(uint64_t handle, const char** out) {
  if (scenario == 3) return Text(out, "LHB-ABCDEF010");
  return Text(out, handle == 11 || scenario == 1 ? "LHB-ABCDEF01" : "Heart rate monitor");
}
EXPORT int lighthouse_peripheral_id(uint64_t handle, const char** out) {
  return Text(out, handle == 11 ? "first" : "second");
}
EXPORT int lighthouse_set_channel(uint64_t adapter, uint64_t peripheral, uint32_t channel,
                                  const char*, uint32_t attempts, uint32_t) {
  ++writes;
  if (adapter == 1 && peripheral == 11 && channel == 4 && attempts == 1 && scenario != 4) return 0;
  error = "simulated BLE write failure";
  return -5;
}
EXPORT int lighthouse_release_handle(uint64_t) { return 0; }
EXPORT void lighthouse_free_string(char* value) { std::free(value); }
EXPORT const char* lighthouse_last_error() { return error; }
