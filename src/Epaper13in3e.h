#pragma once

#include <Arduino.h>

namespace epaper {

constexpr uint16_t kWidth = 1200;
constexpr uint16_t kHeight = 1600;
constexpr size_t kRowBytes = kWidth / 2;
constexpr size_t kFrameSize = kRowBytes * kHeight;

bool begin();
void display(const uint8_t *image);
void powerOff();

}  // namespace epaper

