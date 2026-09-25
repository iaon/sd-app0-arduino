#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <esp32-hal-psram.h>

#include "Epaper13in3e.h"

namespace {

constexpr uint8_t kSdSck = 8;
constexpr uint8_t kSdMiso = 5;
constexpr uint8_t kSdMosi = 3;
constexpr uint8_t kSdCs = 15;
constexpr uint32_t kSdFrequency = 4000000;
constexpr char kSdMountPoint[] = "/sd";
constexpr char kImagePath[] = "/images.bin";
constexpr uint32_t kDefaultIntervalMs = 1000;
constexpr size_t kHeaderSize = 18;

#pragma pack(push, 1)
struct ImageHeader {
    char magic[8];
    uint16_t width;
    uint16_t height;
    uint16_t frames;
    uint32_t intervalMs;
};
#pragma pack(pop)

static_assert(sizeof(ImageHeader) == kHeaderSize);

SPIClass sdSpi(FSPI);
uint8_t *frameBuffer = nullptr;
bool sdReady = false;
uint32_t intervalMs = kDefaultIntervalMs;
uint16_t frameCount = 0;
RTC_DATA_ATTR uint32_t currentFrame = 0;

bool readHeader(File &file, ImageHeader &header) {
    if (file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
        sizeof(header)) {
        return false;
    }

    return memcmp(header.magic, "EPD6BIN", 7) == 0;
}

bool openAndReadHeader(File &file, ImageHeader &header) {
    file = SD.open(kImagePath, FILE_READ);
    if (!file) {
        Serial.println("open images.bin failed");
        return false;
    }

    if (!readHeader(file, header)) {
        Serial.println("invalid images.bin header");
        file.close();
        return false;
    }
    return true;
}

bool readFrame(File &file, uint32_t index) {
    const uint64_t offset =
        kHeaderSize + static_cast<uint64_t>(index) * epaper::kFrameSize;
    if (!file.seek(offset)) {
        return false;
    }

    return file.read(frameBuffer, epaper::kFrameSize) == epaper::kFrameSize;
}

void listRootDirectory() {
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("Failed to open SD card root directory!");
        return;
    }

    Serial.println("SD card root directory file list:");
    for (File file = root.openNextFile(); file;
         file = root.openNextFile()) {
        Serial.print("File: ");
        Serial.print(file.name());
        Serial.print(", Size: ");
        Serial.println(file.size());
        file.close();
    }
    root.close();
}

bool initializeSdCard() {
    Serial.println("Starting SPI initialization...");
    sdSpi.begin(kSdSck, kSdMiso, kSdMosi, -1);
    Serial.println("SPI initialization completed");

    Serial.println("Starting SD card initialization...");
    if (!SD.begin(kSdCs, sdSpi, kSdFrequency, kSdMountPoint, 5, false)) {
        Serial.println("SD card initialization failed!");
        return false;
    }
    Serial.println("SD card initialization successful");
    listRootDirectory();
    return true;
}

[[noreturn]] void halt() {
    while (true) {
        delay(1000);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1000);

    epaper::begin();
    sdReady = initializeSdCard();
    if (!sdReady) {
        halt();
    }

    if (!psramInit()) {
        Serial.println("PSRAM initialization failed!");
        halt();
    }

    frameBuffer =
        static_cast<uint8_t *>(ps_malloc(epaper::kFrameSize));
    if (frameBuffer == nullptr) {
        Serial.println("Memory allocation failed!");
        halt();
    }

    File file;
    ImageHeader header{};
    if (!openAndReadHeader(file, header)) {
        halt();
    }

    frameCount = header.frames;
    intervalMs =
        header.intervalMs == 0 ? kDefaultIntervalMs : header.intervalMs;

    Serial.print("Frames: ");
    Serial.println(frameCount);
    Serial.print("IntervalMs: ");
    Serial.println(intervalMs);
    file.close();
}

void loop() {
    if (!sdReady) {
        return;
    }

    File file;
    ImageHeader header{};
    if (!openAndReadHeader(file, header)) {
        delay(kDefaultIntervalMs);
        return;
    }

    const uint32_t usableFrames = max<uint32_t>(header.frames, 1);
    currentFrame %= usableFrames;

    if (readFrame(file, currentFrame)) {
        epaper::display(frameBuffer);
        delay(500);
    } else {
        Serial.print("Failed to read frame: ");
        Serial.println(currentFrame);
    }

    file.close();
    currentFrame = (currentFrame + 1) % usableFrames;
    delay(intervalMs);
}

