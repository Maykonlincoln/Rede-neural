#include <ATen/core/Generator.h>

namespace at {

/*
* Generator class implementation
*/
Generator::Generator(Device device_in, uint64_t seed_in)
  : device_(device_in), current_seed_(seed_in) {}

Generator::Generator(const Generator& other)
  : Generator(other, std::lock_guard<std::mutex>(other.mutex)) {}

Generator::Generator(const Generator &other, const std::lock_guard<std::mutex> &)
    : device_(other.device_), current_seed_(other.current_seed_) {}

Generator::Generator(Generator&& other)
  : Generator(other, std::lock_guard<std::mutex>(other.mutex)) {}

Generator::Generator(const Generator &&other, const std::lock_guard<std::mutex> &)
    : device_(std::move(other.device_)), 
      current_seed_(std::move(other.current_seed_)) {}

void Generator::setCurrentSeed(uint64_t seed) {
  std::lock_guard<std::mutex> lock(mutex);
  current_seed_ = seed;
}

uint64_t Generator::getCurrentSeed() {
  std::lock_guard<std::mutex> lock(mutex);
  return current_seed_;
}

Device Generator::getDevice() {
  std::lock_guard<std::mutex> lock(mutex);
  return device_;
}

// stubbed. please ignore
Generator& Generator::manualSeedAll(uint64_t seed) {
  return *this;
}

} // namespace at