#include "cti/platform.h"
#include "visa/visa_core.h"

#include <hardware/adc.h>
#include <pico/time.h>

#include <cstring>

namespace CTI {
namespace Visa {

    using namespace SCPI;

    namespace {
        constexpr uint8_t MATRIX_DEFAULT_BUS = 0;
        constexpr uint8_t MATRIX_DEFAULT_ADDR = 0x71;
        constexpr uint8_t MATRIX_DEFAULT_SCL = 5;
        constexpr uint8_t MATRIX_DEFAULT_SDA = 4;
        constexpr uint8_t MATRIX_DEFAULT_BRIGHTNESS = 10;
        constexpr uint16_t MATRIX_DEFAULT_REFRESH_MS = 20;
        constexpr uint16_t MATRIX_MIN_REFRESH_MS = 1;
        constexpr uint16_t MATRIX_MAX_REFRESH_MS = 1000;

        struct MatrixController {
            uint8_t left[8] = {0};
            uint8_t right[8] = {0};
            uint8_t bus = MATRIX_DEFAULT_BUS;
            uint8_t addr = MATRIX_DEFAULT_ADDR;
            uint8_t scl = MATRIX_DEFAULT_SCL;
            uint8_t sda = MATRIX_DEFAULT_SDA;
            uint8_t brightness = MATRIX_DEFAULT_BRIGHTNESS;
            uint16_t refreshMs = MATRIX_DEFAULT_REFRESH_MS;
            int64_t nextRefreshUs = 0;
            bool enabled = false;
            bool dirty = false;
            bool i2cReady = false;

            void initBus() {
                gPlatform.I2C.init(bus, 400000, scl, sda);
                i2cReady = true;
            }

            void writeCommand(uint8_t cmd) {
                if (!i2cReady) {
                    initBus();
                }

                gPlatform.I2C.write(bus, addr, 1, &cmd, false);
            }

            void writeDisplayState() {
                const uint8_t cmd = (uint8_t)(0x80 | (enabled ? 0x01 : 0x00));
                writeCommand(cmd);
            }

            void writeBrightness() {
                uint8_t level = brightness;
                if (level > 15) {
                    level = 15;
                }

                const uint8_t cmd = (uint8_t)(0xE0 | level);
                writeCommand(cmd);
            }

            void writeFrame() {
                uint8_t payload[17];
                payload[0] = 0x00;

                for (int i = 0; i < 8; ++i) {
                    payload[1 + i * 2] = left[i];
                    payload[2 + i * 2] = right[i];
                }

                gPlatform.I2C.write(bus, addr, sizeof(payload), payload, false);
                dirty = false;
            }

            void begin() {
                initBus();

                // HT16K33 oscillator on
                writeCommand(0x21);
                writeBrightness();
                writeDisplayState();
                dirty = true;
                nextRefreshUs = to_us_since_boot(get_absolute_time());
            }

            void setEnabled(bool val) {
                enabled = val;

                if (enabled) {
                    begin();
                } else if (i2cReady) {
                    writeDisplayState();
                }
            }

            void clear() {
                std::memset(left, 0, sizeof(left));
                std::memset(right, 0, sizeof(right));
                dirty = true;
            }

            void setPixel(uint8_t eye, uint8_t row, uint8_t col, bool on) {
                uint8_t* frame = (eye == 0) ? left : right;
                const uint8_t bit = (uint8_t)(1u << col);

                if (on) {
                    frame[row] |= bit;
                } else {
                    frame[row] &= (uint8_t)(~bit);
                }

                dirty = true;
            }

            void tick() {
                if (!enabled) {
                    return;
                }

                const int64_t nowUs = to_us_since_boot(get_absolute_time());

                if (nowUs < nextRefreshUs) {
                    return;
                }

                nextRefreshUs = nowUs + (int64_t)refreshMs * 1000;

                if (dirty) {
                    writeFrame();
                }
            }

            void getFrameInterleaved(uint8_t out[16]) {
                for (int i = 0; i < 8; ++i) {
                    out[i * 2] = left[i];
                    out[i * 2 + 1] = right[i];
                }
            }

            void setFrameInterleaved(const uint8_t in[16]) {
                for (int i = 0; i < 8; ++i) {
                    left[i] = in[i * 2];
                    right[i] = in[i * 2 + 1];
                }

                dirty = true;
            }
        };

        MatrixController gMatrix;

        ScpiChoice matrixModeOptions[] = {
            {"OFF", 0},
            {"ON", 1},
            EndScpiChoice
        };

        CommandResult matrix_set_mode(ScpiParser* scpi) {
            int32_t mode;
            if (scpi->parseChoice(matrixModeOptions, mode) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            gMatrix.setEnabled(mode != 0);
            return CommandResult::Success;
        }

        QueryResult matrix_get_mode(ScpiParser* scpi) {
            gPlatform.IO.Print(gMatrix.enabled ? "ON\n" : "OFF\n");
            return QueryResult::Success;
        }

        CommandResult matrix_set_brightness(ScpiParser* scpi) {
            uint8_t level;

            if (scpi->parseInt(level) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (level > 15) {
                errParamOutOfRange(scpi);
                return CommandResult::Error;
            }

            gMatrix.brightness = level;

            if (gMatrix.enabled) {
                gMatrix.writeBrightness();
            }

            return CommandResult::Success;
        }

        QueryResult matrix_get_brightness(ScpiParser* scpi) {
            gPlatform.IO.Printf("%u\n", gMatrix.brightness);
            return QueryResult::Success;
        }

        CommandResult matrix_set_rate(ScpiParser* scpi) {
            uint16_t refreshMs;

            if (scpi->parseInt(refreshMs) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (refreshMs < MATRIX_MIN_REFRESH_MS || refreshMs > MATRIX_MAX_REFRESH_MS) {
                errParamOutOfRange(scpi);
                return CommandResult::Error;
            }

            gMatrix.refreshMs = refreshMs;
            return CommandResult::Success;
        }

        QueryResult matrix_get_rate(ScpiParser* scpi) {
            gPlatform.IO.Printf("%u\n", gMatrix.refreshMs);
            return QueryResult::Success;
        }

        CommandResult matrix_clear(ScpiParser* scpi) {
            gMatrix.clear();
            return CommandResult::Success;
        }

        CommandResult matrix_set_pixel(ScpiParser* scpi) {
            uint8_t eye;
            uint8_t row;
            uint8_t col;
            uint8_t on;

            if (scpi->parseInt(eye) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (scpi->parseInt(row) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (scpi->parseInt(col) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (scpi->parseInt(on) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (eye > 1 || row > 7 || col > 7) {
                errParamOutOfRange(scpi);
                return CommandResult::Error;
            }

            gMatrix.setPixel(eye, row, col, on != 0);
            return CommandResult::Success;
        }

        CommandResult matrix_set_frame(ScpiParser* scpi) {
            char* buf;
            int len;

            if (scpi->parseBlock(&buf, &len) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            if (len != 16) {
                errParamOutOfRange(scpi);
                return CommandResult::Error;
            }

            gMatrix.setFrameInterleaved((const uint8_t*)buf);
            return CommandResult::Success;
        }

        QueryResult matrix_get_frame(ScpiParser* scpi) {
            uint8_t frame[16];
            gMatrix.getFrameInterleaved(frame);
            PrintBlock(sizeof(frame), frame);
            gPlatform.IO.Print('\n');

            return QueryResult::Success;
        }

        CommandResult matrix_set_left_row(ScpiParser* scpi) {
            ChanIndex row = scpi->nodeNum(3);
            if (row < 0 || row > 7) {
                errSuffixOutOfRange(scpi);
                return CommandResult::Error;
            }

            uint8_t value;
            if (scpi->parseInt(value) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            gMatrix.left[row] = value;
            gMatrix.dirty = true;
            return CommandResult::Success;
        }

        QueryResult matrix_get_left_row(ScpiParser* scpi) {
            ChanIndex row = scpi->nodeNum(3);
            if (row < 0 || row > 7) {
                errSuffixOutOfRange(scpi);
                return QueryResult::Error;
            }

            gPlatform.IO.Printf("%u\n", gMatrix.left[row]);
            return QueryResult::Success;
        }

        CommandResult matrix_set_right_row(ScpiParser* scpi) {
            ChanIndex row = scpi->nodeNum(3);
            if (row < 0 || row > 7) {
                errSuffixOutOfRange(scpi);
                return CommandResult::Error;
            }

            uint8_t value;
            if (scpi->parseInt(value) != ParseResult::Success) {
                return CommandResult::MissingParam;
            }

            gMatrix.right[row] = value;
            gMatrix.dirty = true;
            return CommandResult::Success;
        }

        QueryResult matrix_get_right_row(ScpiParser* scpi) {
            ChanIndex row = scpi->nodeNum(3);
            if (row < 0 || row > 7) {
                errSuffixOutOfRange(scpi);
                return QueryResult::Error;
            }

            gPlatform.IO.Printf("%u\n", gMatrix.right[row]);
            return QueryResult::Success;
        }

    } // namespace

    QueryResult scpi_pico_temp(ScpiParser* scpi) {
        adc_select_input(4);
        uint16_t raw = adc_read();

        float temp = 27 - ((raw / 4096.0) * 3.3 - 0.706) / 0.001721;
        gPlatform.IO.Printf("%f\n", temp);

        return QueryResult::Success;
    }

    void PlatformVisaTick() {
        gMatrix.tick();
    }

    uint32_t PlatformVisaPollTimeoutUs() {
        if (!gMatrix.enabled) {
            return 20000;
        }

        // Poll at least twice within each refresh interval while matrix mode is enabled.
        uint32_t timeoutUs = (uint32_t)gMatrix.refreshMs * 1000 / 2;
        if (timeoutUs < 500) {
            timeoutUs = 500;
        }

        return timeoutUs;
    }

    void Visa::_init() {
        addCommand("PICO:TEMP", nullptr, scpi_pico_temp);

        addCommand("PICO:MATRix:MODE", matrix_set_mode, matrix_get_mode);
        addCommand("PICO:MATRix:BRIGhtness", matrix_set_brightness, matrix_get_brightness);
        addCommand("PICO:MATRix:RATE", matrix_set_rate, matrix_get_rate);
        addCommand("PICO:MATRix:CLEar", matrix_clear, nullptr);
        addCommand("PICO:MATRix:PIXel", matrix_set_pixel, nullptr);
        addCommand("PICO:MATRix:FRAMe", matrix_set_frame, matrix_get_frame);
        addCommand("PICO:MATRix:LEFT:ROW#", matrix_set_left_row, matrix_get_left_row);
        addCommand("PICO:MATRix:RIGHt:ROW#", matrix_set_right_row, matrix_get_right_row);
    };
}
}
