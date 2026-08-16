#include <stdio.h>
#include <cstdlib>
#include <string>
#include <sstream>
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

std::vector<int> parsePinString(const std::string& pinText) {
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

        // Parse integer-like pin names without exceptions, since Pico builds disable C++ exceptions.
        bool valid = true;
        if (normalized.empty()) {
            valid = false;
        }

        for (char ch : normalized) {
            if (ch < '0' || ch > '9') {
                valid = false;
                break;
            }
        }

        if (valid) {
            pins.push_back(static_cast<int>(std::strtol(normalized.c_str(), nullptr, 10)));
        }
    }

    return pins;
}

std::vector<int> gMatrixRows;
std::vector<int> gMatrixCols;
bool gMatrixEnabled = false;
size_t gMatrixRowsCount = 0;
size_t gMatrixColsCount = 0;
std::string gPendingRowPins;
std::string gPendingColPins;

bool InitLedMatrix(const std::string& rowPinString, const std::string& colPinString) {
    const std::vector<int> rowPins = parsePinString(rowPinString);
    const std::vector<int> colPins = parsePinString(colPinString);

    if (rowPins.empty() || colPins.empty()) {
        if (!rowPins.empty()) {
            gPendingRowPins = rowPinString;
        }
        if (!colPins.empty()) {
            gPendingColPins = colPinString;
        }
        return false;
    }

    return InitLedMatrixImpl(rowPinString, colPinString, rowPins.size(), colPins.size());
}

bool InitLedMatrixImpl(const std::string& rowPinString, const std::string& colPinString, size_t rows, size_t cols) {
    const std::vector<int> rowPins = parsePinString(rowPinString);
    const std::vector<int> colPins = parsePinString(colPinString);

    if (rowPins.size() != rows || colPins.size() != cols) {
        gPlatform.IO.Printf("LED matrix config mismatch: rows=%zu/%zu cols=%zu/%zu\n",
            rowPins.size(), rows, colPins.size(), cols);
        return false;
    }

    for (const int pin : rowPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

    for (const int pin : colPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

    gMatrixRows = rowPins;
    gMatrixCols = colPins;
    gMatrixRowsCount = rows;
    gMatrixColsCount = cols;
    gPendingRowPins = rowPinString;
    gPendingColPins = colPinString;
    gMatrixEnabled = true;

    return true;
}

void SetLedMatrixEnabled(bool enabled) {
    gMatrixEnabled = enabled;
    if (!enabled) {
        ClearLedMatrix();
    }
}

void ClearLedMatrix() {
    for (const int pin : gMatrixRows) {
        gpio_put(pin, 0);
    }
    for (const int pin : gMatrixCols) {
        gpio_put(pin, 0);
    }
}

bool SetLedMatrixFrame(const std::string& frameText) {
    if (!gMatrixEnabled || gMatrixRows.empty() || gMatrixCols.empty()) {
        return false;
    }

    std::vector<std::vector<bool>> frame;
    std::stringstream rowStream(frameText);
    std::string rowText;

    while (std::getline(rowStream, rowText, ';')) {
        std::vector<bool> row;
        std::stringstream cellStream(rowText);
        std::string cellText;

        while (std::getline(cellStream, cellText, ',')) {
            const std::string trimmed = cellText;
            const bool value = trimmed == "1" || trimmed == "true" || trimmed == "TRUE" || trimmed == "T";
            row.push_back(value);
        }

        if (!row.empty()) {
            frame.push_back(row);
        }
    }

    if (frame.empty() || frame.size() != gMatrixRowsCount || frame[0].size() != gMatrixColsCount) {
        return false;
    }

    RenderLedMatrixFrame(frame);
    return true;
}

void RenderLedMatrixFrame(const std::vector<std::vector<bool>>& frame) {
    if (!gMatrixEnabled) {
        return;
    }

    if (frame.size() != gMatrixRowsCount) {
        return;
    }

    for (size_t row = 0; row < gMatrixRowsCount; ++row) {
        if (frame[row].size() != gMatrixColsCount) {
            return;
        }
    }

    for (size_t row = 0; row < gMatrixRowsCount; ++row) {
        for (const int colPin : gMatrixCols) {
            gpio_put(colPin, 0);
        }

        gpio_put(gMatrixRows[row], 1);

        for (size_t col = 0; col < gMatrixColsCount; ++col) {
            gpio_put(gMatrixCols[col], frame[row][col] ? 1 : 0);
        }

        sleep_us(800);
        gpio_put(gMatrixRows[row], 0);
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

    // -------------------------------------------------------------------------
    // LED matrix example:
    // Supply the row and column pins as comma-delimited strings and then render
    // a 2D boolean matrix on the Pico itself. This keeps the high-speed row scan
    // timing local to the firmware instead of relying on USB/VISA timing.
    //
    // Example:
    //   const std::string rowPins = "0,1,2,3,4,5,6,7";
    //   const std::string colPins = "8,9,10,11,12,13,14,15";
    //   const std::vector<std::vector<bool>> frame = {
    //       {1,0,0,0,0,0,0,1},
    //       {0,1,0,0,0,0,1,0},
    //       // ...
    //   };
    //
    //   if (InitLedMatrix(rowPins, colPins, 8, 8)) {
    //       RenderLedMatrixFrame(frame);
    //   }
    //
    // Uncomment and edit the lines below to enable this behavior on your board.
    // -------------------------------------------------------------------------
    // const std::string rowPins = "0,1,2,3,4,5,6,7";
    // const std::string colPins = "8,9,10,11,12,13,14,15";
    // const std::vector<std::vector<bool>> frame = {
    //     {1,0,0,0,0,0,0,1},
    //     {0,1,0,0,0,0,1,0},
    //     {0,0,1,0,0,1,0,0},
    //     {0,0,0,1,1,0,0,0},
    //     {0,0,0,1,1,0,0,0},
    //     {0,0,1,0,0,1,0,0},
    //     {0,1,0,0,0,0,1,0},
    //     {1,0,0,0,0,0,0,1}
    // };
    // if (InitLedMatrix(rowPins, colPins, 8, 8)) {
    //     while (true) {
    //         RenderLedMatrixFrame(frame);
    //     }
    // }

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