#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>
#include "driver/gpio.h"
#include "xiao_radio.h"

static gpio_config_t configured;
static unsigned configs = 0;
static std::vector<std::pair<gpio_num_t, uint32_t>> levels;
static int fail_on_call = -1;
static int calls = 0;
static constexpr esp_err_t failure = -77;
esp_err_t gpio_config(const gpio_config_t *pins) {
  ++configs;
  configured = *pins;
  return calls++ == fail_on_call ? failure : ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value) {
  levels.emplace_back(pin, value);
  return calls++ == fail_on_call ? failure : ESP_OK;
}
int main() {
#if defined(ES_PET_ULTIMATE_XIAO)
  assert(xiao_radio_begin() == ESP_OK);
  assert(configs == 1);
  assert(configured.pin_bit_mask == ((1ULL << 3) | (1ULL << 14)));
  assert(configured.mode == GPIO_MODE_OUTPUT);
  assert(levels.size() == 2);
  assert(levels[0] == std::make_pair(GPIO_NUM_14, uint32_t{0}));
  assert(levels[1] == std::make_pair(GPIO_NUM_3, uint32_t{0}));
  // Reapply on radio wake; surface every configuration/output error.
  for (int failing = 0; failing < 3; ++failing) {
    calls = 0;
    fail_on_call = failing;
    assert(xiao_radio_begin() == failure);
    assert(calls == failing + 1);
  }
#else
  assert(xiao_radio_begin() == ESP_OK);
  assert(configs == 0 && levels.empty() && calls == 0);
#endif
  std::puts("XIAO antenna selection/classic isolation checks passed");
}
