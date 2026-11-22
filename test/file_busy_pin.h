#pragma once
// Test-only: BUSY pin backed by a file the emulator writes ("1" = playing).
#include <cstdio>
#include "esphome/core/gpio.h"

class FileBusyPin : public esphome::GPIOPin {
 public:
  void setup() override {}
  void pin_mode(esphome::gpio::Flags) override {}
  esphome::gpio::Flags get_flags() const override { return esphome::gpio::FLAG_INPUT; }
  bool digital_read() override {
    FILE *f = fopen("/tmp/dfa_busy", "r");
    if (f == nullptr)
      return true;
    int c = fgetc(f);
    fclose(f);
    return c != '1';  // BUSY is LOW while playing
  }
  void digital_write(bool) override {}
};
