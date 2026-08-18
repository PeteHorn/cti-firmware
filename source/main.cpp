#include <stdio.h>
#include <array>
#include <cstring>

#ifdef CTI_VISA
    #include "visa.h"
#else
    #ifdef CTI_UDAQ
        #include "udaq.h"
    #else
        #error "No firmware mode set. Ensure a define has been configured for CTI_VISA or CTI_UDAQ"
    #endif
#endif

using namespace CTI;
using namespace Visa;

namespace CTI {

static const size_t LED_MATRIX_SIZE = 8;

// HT16K33 command opcodes.
static const uint8_t HT16K33_SYSTEM_ON = 0x21;
static const uint8_t HT16K33_DISPLAY_ON = 0x81; // display on, blink off
static const uint8_t HT16K33_BRIGHTNESS = 0xE0;

static uint8_t gLedMatrixBus = 0;
static uint8_t gLedMatrixAddr = LED_MATRIX_DEFAULT_ADDR;
static bool gLedMatrixReady = false;

// Display RAM mirror: even bytes are the left panel (ROW0-7), odd bytes the right panel (ROW8-15).
static std::array<uint8_t, 2 * LED_MATRIX_SIZE> gLedMatrixRam = {};

// Matches Freenove_VK16K33::setRow/setPixel: RAM byte c holds bit (7-c) of every source row.
static void packPanel(const uint8_t* src, size_t byteOffset) {
    for (size_t c = 0; c < LED_MATRIX_SIZE; ++c) {
        uint8_t packed = 0;

        for (size_t r = 0; r < LED_MATRIX_SIZE; ++r) {
            if (src[r] & (1u << (7 - c))) {
                packed |= (1u << r);
            }
        }

        gLedMatrixRam[2 * c + byteOffset] = packed;
    }
}

static bool ledMatrixCommand(uint8_t cmd) {
    return gPlatform.I2C.write(gLedMatrixBus, gLedMatrixAddr, 1, &cmd) == 1;
}

bool LedMatrixInit(uint8_t bus, uint8_t sclPin, uint8_t sdaPin, uint8_t addr, uint8_t brightness) {
    gLedMatrixBus = bus;
    gLedMatrixAddr = addr;
    gLedMatrixReady = false;

    if (brightness > 15) {
        brightness = 15;
    }

    gPlatform.I2C.init(bus, 100000, sclPin, sdaPin);

    if (!ledMatrixCommand(HT16K33_SYSTEM_ON)) {
        return false;
    }

    // Oscillator needs a moment before the display driver accepts setup.
    gPlatform.Timer.SleepMilliseconds(1);

    if (!ledMatrixCommand(HT16K33_DISPLAY_ON)) {
        return false;
    }

    if (!ledMatrixCommand(HT16K33_BRIGHTNESS | brightness)) {
        return false;
    }

    gLedMatrixReady = true;
    gLedMatrixRam.fill(0);

    return LedMatrixRefresh();
}

bool LedMatrixRefresh() {
    if (!gLedMatrixReady) {
        return false;
    }

    uint8_t frame[1 + gLedMatrixRam.size()];
    frame[0] = 0x00; // display RAM start address
    std::memcpy(&frame[1], gLedMatrixRam.data(), gLedMatrixRam.size());

    return gPlatform.I2C.write(gLedMatrixBus, gLedMatrixAddr, sizeof(frame), frame) == sizeof(frame);
}

bool SetLedMatrixX(const uint8_t* data, size_t len) {
    if (len != LED_MATRIX_SIZE || data == nullptr) {
        return false;
    }

    packPanel(data, 0);

    return LedMatrixRefresh();
}

bool SetLedMatrixY(const uint8_t* data, size_t len) {
    if (len != LED_MATRIX_SIZE || data == nullptr) {
        return false;
    }

    packPanel(data, 1);

    return LedMatrixRefresh();
}

} // namespace CTI

int main() {

    //Allow any board specific initialization to happen.
    gPlatform.BoardInit();

    //First interfacing for debugging capabilities is a built in LED, if available.
    // this can be blinked and much more robust than other comm mechanisms for
    // complete initialization failures.
    gPlatform.IO.InitStatusLED();

    //This kicks off any initialization the platform specific code needs to do
    // before initializing the platform abstraction code. This usually means
    // ensuring any communication and debugging hooks are available and
    // and customized startup logic for a specific platform.
    gPlatform.Preinit();

    //Initialize the global platform abstractions
    gPlatform.Setup();

    //TODO: Launch different engines on different cores when available.
    gPlatform.IO.Print("Starting engine\n");
    int status = gPlatform.pEngine->Ready();
    if (status == 0) {
        gPlatform.pEngine->MainLoop();
    } else {
        gPlatform.IO.Printf("Engine error: %s\n", gPlatform.pEngine->StatusText(status));
        gPlatform.IO.Flush();
        gPlatform.Timer.SleepMilliseconds(1000);
    }
    
    return 0;
}