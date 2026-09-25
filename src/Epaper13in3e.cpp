/*
 * Adapted from the Waveshare 13.3-inch Spectra 6 reference driver.
 * The pin mapping and AN_TM values were recovered from sd-app0.bin.
 *
 * Copyright (c) Waveshare
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#include "Epaper13in3e.h"

namespace epaper {
namespace {

constexpr uint8_t kPinSck = 9;
constexpr uint8_t kPinMosi = 41;
constexpr uint8_t kPinCsMaster = 18;
constexpr uint8_t kPinCsSlave = 17;
constexpr uint8_t kPinDc = 2;
constexpr uint8_t kPinReset = 6;
constexpr uint8_t kPinBusy = 7;
constexpr uint8_t kPinPower = 45;

constexpr uint8_t kPsr[] = {0xDF, 0x69};
constexpr uint8_t kPower[] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
constexpr uint8_t kPowerOff[] = {0x00};
constexpr uint8_t kDisplayRefresh[] = {0x00};
constexpr uint8_t kCdi[] = {0xF7};
constexpr uint8_t kTcon[] = {0x03, 0x03};
constexpr uint8_t kResolution[] = {0x04, 0xB0, 0x03, 0x20};
constexpr uint8_t kCommand66[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
constexpr uint8_t kEnableBuffer[] = {0x07};
constexpr uint8_t kCcset[] = {0x01};
constexpr uint8_t kPws[] = {0x22};
constexpr uint8_t kAnalogTiming[] = {
    0xC0, 0x1E, 0x1E, 0xCE, 0xCE, 0xCE, 0x15, 0x15, 0x55,
};
constexpr uint8_t kAgid[] = {0x10};
constexpr uint8_t kBtstPositive[] = {0xE8, 0x28};
constexpr uint8_t kBoostVddpEnable[] = {0x01};
constexpr uint8_t kBtstNegative[] = {0xE8, 0x28};
constexpr uint8_t kBuckBoostVddn[] = {0x01};
constexpr uint8_t kTftVcomPower[] = {0x02};

void selectBoth(bool selected) {
    digitalWrite(kPinCsMaster, selected ? LOW : HIGH);
    digitalWrite(kPinCsSlave, selected ? LOW : HIGH);
}

void writeByte(uint8_t value) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
        digitalWrite(kPinMosi, (value & 0x80) != 0 ? HIGH : LOW);
        value <<= 1;
        digitalWrite(kPinSck, HIGH);
        digitalWrite(kPinSck, LOW);
    }
}

void writeBytes(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        writeByte(data[i]);
    }
}

template <size_t N>
void writeCommand(uint8_t command, const uint8_t (&data)[N]) {
    writeByte(command);
    writeBytes(data, N);
}

void reset() {
    for (uint8_t i = 0; i < 2; ++i) {
        digitalWrite(kPinReset, HIGH);
        delay(30);
        digitalWrite(kPinReset, LOW);
        delay(30);
    }
    digitalWrite(kPinReset, HIGH);
    delay(30);
}

void waitUntilIdle() {
    Serial.println("e-Paper busy");
    while (digitalRead(kPinBusy) == LOW) {
        delay(10);
    }
    delay(20);
    Serial.println("e-Paper busy release");
}

void initializeController() {
    reset();

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0x74, kAnalogTiming);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0xF0, kCommand66);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0x00, kPsr);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0x50, kCdi);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0x60, kTcon);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0x86, kAgid);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0xE3, kPws);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0xE0, kCcset);
    selectBoth(false);

    selectBoth(true);
    writeCommand(0x61, kResolution);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0x01, kPower);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0xB6, kEnableBuffer);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0x06, kBtstPositive);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0xB7, kBoostVddpEnable);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0x05, kBtstNegative);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0xB0, kBuckBoostVddn);
    selectBoth(false);

    digitalWrite(kPinCsMaster, LOW);
    writeCommand(0xB1, kTftVcomPower);
    selectBoth(false);
}

void refresh() {
    Serial.println("Write PON");
    selectBoth(true);
    writeByte(0x04);
    selectBoth(false);
    waitUntilIdle();

    Serial.println("Write DRF");
    delay(50);
    selectBoth(true);
    writeCommand(0x12, kDisplayRefresh);
    selectBoth(false);
    waitUntilIdle();

    Serial.println("Write POF");
    selectBoth(true);
    writeCommand(0x02, kPowerOff);
    selectBoth(false);
    Serial.println("Display Done!!");
}

}  // namespace

bool begin() {
    pinMode(kPinBusy, INPUT);
    pinMode(kPinReset, OUTPUT);
    pinMode(kPinDc, OUTPUT);
    pinMode(kPinPower, OUTPUT);
    pinMode(kPinSck, OUTPUT);
    pinMode(kPinMosi, OUTPUT);
    pinMode(kPinCsMaster, OUTPUT);
    pinMode(kPinCsSlave, OUTPUT);

    digitalWrite(kPinCsMaster, HIGH);
    digitalWrite(kPinCsSlave, HIGH);
    digitalWrite(kPinSck, LOW);
    digitalWrite(kPinPower, HIGH);
    return true;
}

void display(const uint8_t *image) {
    initializeController();

    digitalWrite(kPinCsMaster, LOW);
    writeByte(0x10);
    for (uint16_t row = 0; row < kHeight; ++row) {
        writeBytes(image + row * kRowBytes, kRowBytes / 2);
        delay(1);
    }
    selectBoth(false);

    digitalWrite(kPinCsSlave, LOW);
    writeByte(0x10);
    for (uint16_t row = 0; row < kHeight; ++row) {
        writeBytes(image + row * kRowBytes + kRowBytes / 2, kRowBytes / 2);
        delay(1);
    }
    selectBoth(false);

    refresh();
}

void powerOff() {
    selectBoth(false);
    digitalWrite(kPinPower, LOW);
    digitalWrite(kPinReset, LOW);
}

}  // namespace epaper

