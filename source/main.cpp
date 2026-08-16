#include <stdio.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <pico/stdlib.h>
#include <hardware/gpio.h>

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

std::vector<int> gLedMatrixRows;
std::vector<int> gLedMatrixCols;
std::string gLedMatrixRowPins;
std::string gLedMatrixColPins;
std::array<uint8_t, LED_MATRIX_SIZE> gLedMatrixX = {};
std::array<uint8_t, LED_MATRIX_SIZE> gLedMatrixY = {};
bool gLedMatrixReady = false;

std::vector<int> parsePinList(const std::string& pinText) {
    std::vector<int> pins;
    std::stringstream stream(pinText);
    std::string token;

    while (std::getline(stream, token, ',')) {
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }

        const auto last = token.find_last_not_of(" \t\r\n");
        token = token.substr(first, last - first + 1);

        if (token.empty()) {
            continue;
        }

        std::string normalized = token;
        if (normalized.size() > 2 && normalized[0] == 'G' && normalized[1] == 'P') {
            normalized = normalized.substr(2);
        }
        if (normalized.size() > 2 && normalized[0] == 'g' && normalized[1] == 'p') {
            normalized = normalized.substr(2);
        }
        if (normalized.size() > 4 && normalized[0] == 'G' && normalized[1] == 'P' && normalized[2] == 'I' && normalized[3] == 'O') {
            normalized = normalized.substr(4);
        }
        if (normalized.size() > 4 && normalized[0] == 'g' && normalized[1] == 'p' && normalized[2] == 'i' && normalized[3] == 'o') {
            normalized = normalized.substr(4);
        }

        char* end = nullptr;
        long value = std::strtol(normalized.c_str(), &end, 10);
        if (end != nullptr && *end == '\0') {
            pins.push_back(static_cast<int>(value));
        }
    }

    return pins;
}

bool ConfigureLedMatrixPins(const std::string& rowPinString, const std::string& colPinString) {
    std::vector<int> rowPins = parsePinList(rowPinString);
    std::vector<int> colPins = parsePinList(colPinString);

    if (rowPins.size() != LED_MATRIX_SIZE || colPins.size() != LED_MATRIX_SIZE) {
        return false;
    }

    for (int pin : rowPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

    for (int pin : colPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

    gLedMatrixRows = rowPins;
    gLedMatrixCols = colPins;
    gLedMatrixRowPins = rowPinString;
    gLedMatrixColPins = colPinString;
    gLedMatrixReady = true;

    return true;
}

bool SetLedMatrixX(const uint8_t* data, size_t len) {
    if (len != LED_MATRIX_SIZE || data == nullptr) {
        return false;
    }

    std::memcpy(gLedMatrixX.data(), data, len);
    return true;
}

bool SetLedMatrixY(const uint8_t* data, size_t len) {
    if (len != LED_MATRIX_SIZE || data == nullptr) {
        return false;
    }

    std::memcpy(gLedMatrixY.data(), data, len);
    return true;
}

void RenderLedMatrixScan() {
    if (!gLedMatrixReady || gLedMatrixRows.size() != LED_MATRIX_SIZE || gLedMatrixCols.size() != LED_MATRIX_SIZE) {
        return;
    }

    for (size_t row = 0; row < LED_MATRIX_SIZE; ++row) {
        for (size_t col = 0; col < LED_MATRIX_SIZE; ++col) {
            gpio_put(gLedMatrixCols[col], 0);
        }

        gpio_put(gLedMatrixRows[row], 0);

        for (size_t col = 0; col < LED_MATRIX_SIZE; ++col) {
            bool on = (gLedMatrixY[row] & (1u << col)) != 0;
            gpio_put(gLedMatrixCols[col], on ? 1 : 0);
        }

        gpio_put(gLedMatrixRows[row], 1);
        sleep_us(800);
        gpio_put(gLedMatrixRows[row], 0);
    }
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