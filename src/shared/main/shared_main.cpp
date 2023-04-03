#include "shared_main.h"

#include "Arduino.h"
#include "bt.h"
#include "config.h"
#include "controllers.h"
#include "endpoints.h"
#include "fxpt_math.h"
#include "hid.h"
#include "io.h"
#include "io_define.h"
#include "pins.h"
#include "ps2.h"
#include "rf.h"
#include "usbhid.h"
#include "util.h"
#include "wii.h"
#define DJLEFT_ADDR 0x0E
#define DJRIGHT_ADDR 0x0D
#define DJ_BUTTONS_PTR 0x12
#define GH5NECK_ADDR 0x0D
#define GH5NECK_BUTTONS_PTR 0x12
USB_Report_Data_t combined_report;
PS3_REPORT bt_report;
uint8_t debounce[DIGITAL_COUNT];
uint8_t drumVelocity[8];
long lastSentPacket = 0;
long lastSentGHLPoke = 0;
long lastTap;
long lastTapShift;
uint8_t ghl_sequence_number_host = 1;
uint16_t wiiControllerType = WII_NO_EXTENSION;
uint8_t ps2ControllerType = PSX_NO_DEVICE;
uint8_t lastSuccessfulPS2Packet[BUFFER_SIZE];
uint8_t lastSuccessfulWiiPacket[8];
uint8_t lastSuccessfulGH5Packet[2];
uint8_t lastSuccessfulTurntablePacketLeft[3];
uint8_t lastSuccessfulTurntablePacketRight[3];
long lastSuccessfulGHWTPacket;
bool lastGHWTWasSuccessful = false;
bool lastGH5WasSuccessful = false;
bool lastTurntableWasSuccessfulLeft = false;
bool lastTurntableWasSuccessfulRight = false;
bool lastWiiWasSuccessful = false;
bool lastPS2WasSuccessful = false;
bool overrideR2 = false;
bool lastXboxOneGuide = false;
uint8_t overriddenR2 = 0;
USB_LastReport_Data_t last_report_usb;
USB_LastReport_Data_t last_report_bt;
USB_LastReport_Data_t last_report_rf;
USB_LastReport_Data_t temp_report_usb_host;
uint8_t address_tx_to_rx[][6] = {"First", "Secon", "Third", "Fourt", "Fifth", "Sixth"};
uint8_t address_rx_to_tx[][6] = {"Seven", "Eight", "Ninth", "Tenth", "Eleve", "Twelv"};
long initialWt[5] = {0};
uint8_t rawWt;
bool auth_ps4_controller_found = false;
bool seen_ps4_console = false;

/* Magic data taken from GHLtarUtility:
 * https://github.com/ghlre/GHLtarUtility/blob/master/PS3Guitar.cs
 * Note: The Wii U and PS3 dongles happen to share the same!
 */
const PROGMEM uint8_t ghl_ps3wiiu_magic_data[] = {
    0x02, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};

/* Magic data for the PS4 dongles sniffed with a USB protocol
 * analyzer.
 */
const PROGMEM uint8_t ghl_ps4_magic_data[] = {
    0x30, 0x02, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};
#ifdef RF_TX
RfInputPacket_t rf_report = {Input};
RfHeartbeatPacket_t rf_heartbeat = {Heartbeat};
void send_rf_console_type() {
    if (rf_initialised) {
        RfConsoleTypePacket_t packet = {
            ConsoleType, consoleType};
        radio.write(&packet, sizeof(packet));
    }
}
#endif
#ifdef RF
#include "SPI.h"
#include "rf.h"
RF24 radio(RADIO_CE, RADIO_CSN);
uint8_t rf_data[32];
#endif
#ifdef RF_RX
RfInputPacket_t last_rf_inputs[RF_COUNT];
#endif
typedef struct {
    // If this bit is set, then an led effect (like star power) has overridden the leds
    uint8_t select;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Led_t;
Led_t ledState[LED_COUNT];
#define UP 1 << 0
#define DOWN 1 << 1
#define LEFT 1 << 2
#define RIGHT 1 << 3
static const uint8_t dpad_bindings[] = {0x08, 0x00, 0x04, 0x08, 0x06, 0x07, 0x05, 0x08, 0x02, 0x01, 0x03};
static const uint8_t dpad_bindings_reverse[] = {UP, UP | RIGHT, RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT};

#ifdef INPUT_WT_NECK
uint8_t gh5_mapping[] = {
    0x00,
    0x95,
    0xCD,
    0xB0,
    0x1A,
    0x19,
    0xE6,
    0xE5,
    0x49,
    0x47,
    0x48,
    0x46,
    0x2F,
    0x2D,
    0x2E,
    0x2C,
    0x7F,
    0x7B,
    0x7D,
    0x79,
    0x7E,
    0x7A,
    0x7C,
    0x78,
    0x66,
    0x62,
    0x64,
    0x60,
    0x65,
    0x61,
    0x63,
    0x5F,
};
bool checkWt(int pin) {
    return readWt(pin) > initialWt[pin];
}
uint8_t readWtAnalog() {
    return gh5_mapping[rawWt];
}
#endif
void init_main(void) {
    initPins();
    twi_init();
    spi_begin();
    memset(ledState, 0, sizeof(ledState));
#ifdef INPUT_PS2
    init_ack();
#endif
#ifdef RADIO_MOSI
    SPI.setTX(RADIO_MOSI);
    SPI.setRX(RADIO_MISO);
    SPI.setSCK(RADIO_SCK);
#endif
#ifdef RF
    rf_initialised = radio.begin();
    radio.setPALevel(RF_POWER_LEVEL);
    radio.enableDynamicPayloads();
    radio.enableAckPayload();
#endif
#ifdef RF_TX
    // TX Writes to the
    radio.openWritingPipe(address_tx_to_rx[RF_DEVICE_ID]);  // always uses pipe 0

    // set the RX address of the TX node into a RX pipe
    radio.openReadingPipe(1, address_rx_to_tx[RF_CHANNEL]);
    radio.stopListening();
    send_rf_console_type();
#endif
#ifdef RF_RX
    radio.openWritingPipe(address_rx_to_tx[RF_CHANNEL]);  // always uses pipe 0

    // set the RX address of the TX node into a RX pipe
    for (int i = 0; i < RF_COUNT; i++) {
        radio.openReadingPipe(i + 1, address_tx_to_rx[i]);
    }
    radio.startListening();
#endif
#ifdef INPUT_WT_NECK
    memset(initialWt, 0, sizeof(initialWt));
    for (int j = 0; j < 50; j++) {
        for (int i = 0; i < 5; i++) {
            long reading = readWt(i) + WT_SENSITIVITY;
            if (reading > initialWt[i]) {
                initialWt[i] = reading;
            }
        }
    }
#endif
}
int16_t adc_i(uint8_t pin) {
    int32_t ret = adc(pin);
    return ret - 32767;
}
int16_t handle_calibration_xbox(int16_t orig_val, int16_t offset, int16_t min, int16_t multiplier, int16_t deadzone) {
    int32_t val = orig_val;
    int16_t val_deadzone = orig_val - offset;
    if (val_deadzone < deadzone && val_deadzone > -deadzone) {
        return 0;
    }
    if (val < 0) {
        deadzone = -deadzone;
    }
    val -= deadzone;
    val -= min;
    val *= multiplier;
    val /= 512;
    val += INT16_MIN;
    if (val > INT16_MAX) {
        val = INT16_MAX;
    }
    if (val < INT16_MIN) {
        val = INT16_MIN;
    }
    return val;
}

int16_t handle_calibration_xbox_whammy(uint16_t orig_val, uint16_t min, int16_t multiplier, uint16_t deadzone) {
    int32_t val = orig_val;
    if (multiplier > 0) {
        if ((val - min) < deadzone) {
            return INT16_MIN;
        }
    } else {
        if (val > min) {
            return INT16_MIN;
        }
    }
    val -= min;
    val *= multiplier;
    val /= 512;
    val -= INT16_MAX;
    if (val > INT16_MAX) {
        val = INT16_MAX;
    }
    if (val < INT16_MIN) {
        val = INT16_MIN;
    }
    return val;
}
uint16_t handle_calibration_xbox_one_trigger(uint16_t orig_val, uint16_t min, int16_t multiplier, uint16_t deadzone) {
    int32_t val = orig_val;
    if (multiplier > 0) {
        if ((val - min) < deadzone) {
            return 0;
        }
    } else {
        if (val > min) {
            return 0;
        }
    }
    val -= min;
    val *= multiplier;
    val /= 512;
    if (val > UINT16_MAX) {
        val = UINT16_MAX;
    }
    if (val < 0) {
        val = 0;
    }
    return val;
}
uint8_t handle_calibration_ps3(int16_t orig_val, int16_t offset, int16_t min, int16_t multiplier, int16_t deadzone) {
    int8_t ret = handle_calibration_xbox(orig_val, offset, min, multiplier, deadzone) >> 8;
    return (uint8_t)(ret - PS3_STICK_CENTER);
}

uint8_t handle_calibration_ps3_360_trigger(uint16_t orig_val, uint16_t min, int16_t multiplier, uint16_t deadzone) {
    return handle_calibration_xbox_one_trigger(orig_val, min, multiplier, deadzone) >> 8;
}

uint16_t handle_calibration_ps3_accel(uint16_t orig_val, int16_t offset, uint16_t min, int16_t multiplier, uint16_t deadzone) {
#if DEVICE_TYPE == GUITAR
    // For whatever reason, the acceleration for guitars swings between -128 to 128, not -512 to 512
    int16_t ret = (handle_calibration_xbox(orig_val, offset, min, multiplier, deadzone) >> 8) - GUITAR_ONE_G;
#else
    int16_t ret = handle_calibration_xbox(orig_val, offset, min, multiplier, deadzone) >> 6;
#endif
    return PS3_ACCEL_CENTER + ret;
}

uint8_t handle_calibration_ps3_whammy(uint16_t orig_val, uint16_t min, int16_t multiplier, uint16_t deadzone) {
    int8_t ret = handle_calibration_xbox_whammy(orig_val, min, multiplier, deadzone) >> 8;
    return (uint8_t)(ret - PS3_STICK_CENTER);
}

uint8_t tick_xbox_one() {
    switch (xbox_one_state) {
        case Announce:
            xbox_one_state = Waiting1;
            memcpy(&combined_report, announce, sizeof(announce));
            return sizeof(announce);
        case Ident1:
            xbox_one_state = Waiting2;
            memcpy(&combined_report, identify_1, sizeof(identify_1));
            return sizeof(identify_1);
        case Ident2:
            xbox_one_state = Ident3;
            memcpy(&combined_report, identify_2, sizeof(identify_2));
            return sizeof(identify_2);
        case Ident3:
            xbox_one_state = Ident4;
            memcpy(&combined_report, identify_3, sizeof(identify_3));
            return sizeof(identify_3);
        case Ident4:
            xbox_one_state = Waiting5;
            memcpy(&combined_report, identify_4, sizeof(identify_4));
            return sizeof(identify_4);
        case Ident5:
            xbox_one_state = Auth;
            memcpy(&combined_report, identify_5, sizeof(identify_5));
            return sizeof(identify_5);
        case Auth:
            if (data_from_controller_size) {
                uint8_t size = data_from_controller_size;
                data_from_controller_size = 0;
                memcpy(&combined_report, data_from_controller, size);
                return size;
            }
            return 0;
        case Ready:
            return 0;
        default:
            return 0;
    }
}

long lastTick;
uint8_t keyboard_report = 0;
#if defined(RF_RX) || BLUETOOTH
// When we do RF and Bluetooth, the reports are ALWAYS in PS3 Instrument format, so we need to convert
void convert_ps3_to_type(uint8_t *buf, PS3_REPORT *report, uint8_t output_console_type) {
    PS3Dpad_Data_t *dpad_report = (PS3Dpad_Data_t *)report;
    uint8_t dpad = dpad_report->dpad > sizeof(dpad_bindings_reverse) ? 0 : dpad_bindings_reverse[dpad_report->dpad];
    bool up = dpad & UP;
    bool down = dpad & DOWN;
    bool left = dpad & LEFT;
    bool right = dpad & RIGHT;
    bool universal_report = output_console_type == UNIVERSAL || output_console_type == REAL_PS3;
#if DEVICE_TYPE_IS_INSTRUMENT
    universal_report |= output_console_type == PS3;
#elif DEVICE_TYPE_IS_GAMEPAD
    if (consoleType == PS3) {
        PS3Gamepad_Data_t *out = (PS3Gamepad_Data_t *)buf;
        if (report->leftStickX != PS3_STICK_CENTER) {
            out->leftStickX = report->leftStickX;
        }
        if (report->leftStickY != PS3_STICK_CENTER) {
            out->leftStickY = report->leftStickY;
        }
        if (report->rightStickX != PS3_STICK_CENTER) {
            out->rightStickX = report->rightStickX;
        }
        if (report->rightStickY != PS3_STICK_CENTER) {
            out->rightStickY = report->rightStickY;
        }
        if (report->leftTrigger) {
            out->leftTrigger = report->leftTrigger;
        }
        if (report->rightTrigger) {
            out->rightTrigger = report->rightTrigger;
        }
        if (report->pressureDpadUp) {
            out->pressureDpadUp = report->pressureDpadUp;
        }
        if (report->pressureDpadRight) {
            out->pressureDpadRight = report->pressureDpadRight;
        }
        if (report->pressureDpadDown) {
            out->pressureDpadDown = report->pressureDpadDown;
        }
        if (report->pressureDpadLeft) {
            out->pressureDpadLeft = report->pressureDpadLeft;
        }
        if (report->pressureL1) {
            out->pressureL1 = report->pressureL1;
        }
        if (report->pressureR1) {
            out->pressureR1 = report->pressureR1;
        }
        if (report->pressureTriangle) {
            out->pressureTriangle = report->pressureTriangle;
        }
        if (report->pressureCircle) {
            out->pressureCircle = report->pressureCircle;
        }
        if (report->pressureCross) {
            out->pressureCross = report->pressureCross;
        }
        if (report->pressureSquare) {
            out->pressureSquare = report->pressureSquare;
        }
        out->dpadUp |= up;
        out->dpadDown |= down;
        out->dpadLeft |= left;
        out->dpadRight |= right;
        out->x |= report->x;
        out->a |= report->a;
        out->b |= report->b;
        out->y |= report->y;

        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
        out->l2 |= report->l2;
        out->r2 |= report->r2;

        out->back |= report->back;
        out->start |= report->start;
        out->leftThumbClick |= report->leftThumbClick;
        out->rightThumbClick |= report->rightThumbClick;

        out->guide |= report->guide;
    }
#endif
    if (consoleType == PS4) {
        PS4_REPORT *out = (PS4_REPORT *)buf;
        out->x |= report->x;
        out->a |= report->a;
        out->b |= report->b;
        out->y |= report->y;

        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
        out->l2 |= report->l2;
        out->r2 |= report->r2;

        out->back |= report->back;
        out->start |= report->start;
        out->leftThumbClick |= report->leftThumbClick;
        out->rightThumbClick |= report->rightThumbClick;

        out->guide |= report->guide;
        out->capture |= report->capture;
        if (dpad) {
            PS4Dpad_Data_t *out_dpad_report = (PS4Dpad_Data_t *)buf;
            out_dpad_report->dpad = dpad_report->dpad;
        }
#if DEVICE_TYPE_IS_LIVE_GUITAR
        if (report->tilt_pc != PS3_STICK_CENTER) {
            out->tilt = report->tilt_pc;
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = report->whammy;
        }
        if (report->strumBar != PS3_STICK_CENTER) {
            out->strumBar = report->strumBar;
        }
#elif DEVICE_TYPE_IS_GAMEPAD
        if (report->leftStickX != PS3_STICK_CENTER) {
            out->leftStickX = report->leftStickX;
        }
        if (report->leftStickY != PS3_STICK_CENTER) {
            out->leftStickY = report->leftStickY;
        }
        if (report->rightStickX != PS3_STICK_CENTER) {
            out->rightStickX = report->rightStickX;
        }
        if (report->rightStickY != PS3_STICK_CENTER) {
            out->rightStickY = report->rightStickY;
        }
        if (report->leftTrigger) {
            out->leftTrigger = report->leftTrigger;
        }
        if (report->rightTrigger) {
            out->rightTrigger = report->rightTrigger;
        }
#endif
    }
    if (consoleType == XBOXONE) {
        XBOX_ONE_REPORT *out = (XBOX_ONE_REPORT *)buf;
        out->x |= report->x;
        out->a |= report->a;
        out->b |= report->b;
        out->y |= report->y;

        out->dpadUp |= up;
        out->dpadDown |= down;
        out->dpadLeft |= left;
        out->dpadRight |= right;
        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;

        out->back |= report->back;
        out->start |= report->start;
        out->leftThumbClick |= report->leftThumbClick;
        out->rightThumbClick |= report->rightThumbClick;

        out->guide |= report->guide;
#if DEVICE_TYPE_IS_GAMEPAD
        if (report->leftStickX != PS3_STICK_CENTER) {
            out->leftStickX = (report->leftStickX - 128) << 8;
        }
        if (report->leftStickY != PS3_STICK_CENTER) {
            out->leftStickY = (report->leftStickY - 128) << 8;
        }
        if (report->rightStickX != PS3_STICK_CENTER) {
            out->rightStickX = (report->rightStickX - 128) << 8;
        }
        if (report->rightStickY != PS3_STICK_CENTER) {
            out->rightStickY = (report->rightStickY - 128) << 8;
        }
        if (report->leftTrigger) {
            out->leftTrigger = (report->leftTrigger) << 8;
        }
        if (report->rightTrigger) {
            out->rightTrigger = (report->rightTrigger) << 8;
        }
#elif DEVICE_TYPE_IS_LIVE_GUITAR
        if (report->tilt_pc != PS3_STICK_CENTER) {
            out->tilt = report->tilt_pc;
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = report->whammy;
        }
        if (report->strumBar != PS3_STICK_CENTER) {
            out->strumBar = report->strumBar;
        }
#elif DEVICE_TYPE_IS_GUITAR
        if (report->tilt_pc != PS3_STICK_CENTER) {
            out->tilt = report->tilt_pc;
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = report->whammy;
        }
        if (report->pickup != PS3_STICK_CENTER) {
            out->pickup = report->pickup;
        }
#elif DEVICE_TYPE_IS_DRUM
        if (report->yellowVelocity) {
            out->yellowVelocity = report->yellowVelocity >> 4;
        }
        if (report->redVelocity) {
            out->redVelocity = report->redVelocity >> 4;
        }
        if (report->greenVelocity) {
            out->greenVelocity = report->greenVelocity >> 4;
        }
        if (report->blueVelocity) {
            out->blueVelocity = report->blueVelocity >> 4;
        }
#endif
    }
    if (consoleType == WINDOWS_XBOX360) {
        XINPUT_REPORT *out = (XINPUT_REPORT *)buf;
        out->x |= report->x;
        out->a |= report->a;
        out->b |= report->b;
        out->y |= report->y;
        out->dpadUp |= up;
        out->dpadDown |= down;
        out->dpadLeft |= left;
        out->dpadRight |= right;

        out->back |= report->back;
        out->start |= report->start;

        out->guide |= report->guide;
#if DEVICE_TYPE_IS_GAMEPAD
        if (report->leftStickX != PS3_STICK_CENTER) {
            out->leftStickX = (report->leftStickX - 128) << 8;
        }
        if (report->leftStickY != PS3_STICK_CENTER) {
            out->leftStickY = (report->leftStickY - 128) << 8;
        }
        if (report->rightStickX != PS3_STICK_CENTER) {
            out->rightStickX = (report->rightStickX - 128) << 8;
        }
        if (report->rightStickY != PS3_STICK_CENTER) {
            out->rightStickY = (report->rightStickY - 128) << 8;
        }
        if (report->leftTrigger) {
            out->leftTrigger = report->leftTrigger;
        }
        if (report->rightTrigger) {
            out->rightTrigger = report->rightTrigger;
        }

        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
        out->leftThumbClick |= report->leftThumbClick;
        out->rightThumbClick |= report->rightThumbClick;
#elif DEVICE_TYPE == DRUMS && RHYTHM_TYPE == GUITAR_HERO
        if (report->yellowVelocity) {
            out->yellowVelocity = report->yellowVelocity;
        }
        if (report->redVelocity) {
            out->redVelocity = report->redVelocity;
        }
        if (report->greenVelocity) {
            out->greenVelocity = report->greenVelocity;
        }
        if (report->blueVelocity) {
            out->blueVelocity = report->blueVelocity;
        }
        if (report->kickVelocity) {
            out->kickVelocity = report->kickVelocity;
        }
        if (report->orangeVelocity) {
            out->orangeVelocity = report->orangeVelocity;
        }
#elif DEVICE_TYPE == DRUMS && RHYTHM_TYPE == ROCK_BAND
        if (report->yellowVelocity) {
            out->yellowVelocity = -(0x7FFF - (report->yellowVelocity << 7));
        }
        if (report->redVelocity) {
            out->redVelocity = (0x7FFF - (report->redVelocity << 7));
        }
        if (report->greenVelocity) {
            out->greenVelocity = -(0x7FFF - (report->greenVelocity << 7));
        }
        if (report->blueVelocity) {
            out->blueVelocity = (0x7FFF - (report->blueVelocity << 7));
        }
        out->padFlag = report->padFlag;
        out->cymbalFlag = report->cymbalFlag;
#elif DEVICE_TYPE_IS_LIVE_GUITAR
        if (report->tilt_pc != PS3_STICK_CENTER) {
            out->tilt = (report->tilt_pc - 128) << 8;
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = (report->whammy - 128) << 8;
        }
        if (report->strumBar != PS3_STICK_CENTER) {
            out->strumBar = (report->strumBar - 128) << 8;
        }
#elif DEVICE_TYPE == GUITAR && RHYTHM_TYPE == GUITAR_HERO
        if (report->tilt_pc != PS3_STICK_CENTER) {
            out->tilt = (report->tilt_pc - 128) << 8;
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = (report->whammy - 128) << 8;
        }
        if (report->slider != PS3_STICK_CENTER) {
            out->slider = (report->slider) << 8;
        }
#elif DEVICE_TYPE == GUITAR && RHYTHM_TYPE == ROCK_BAND
        if (report->tilt_pc != PS3_STICK_CENTER) {
            out->tilt = (report->tilt_pc - 128) << 8;
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = (report->whammy - 128) << 8;
        }
        if (report->pickup != PS3_STICK_CENTER) {
            out->pickup = report->pickup;
        }
#elif DEVICE_TYPE == DJ_HERO_TURNTABLE

        if (report->leftTableVelocity != PS3_STICK_CENTER) {
            out->leftTableVelocity = (report->leftTableVelocity - 128) << 8;
        }
        if (report->rightTableVelocity != PS3_STICK_CENTER) {
            out->rightTableVelocity = (report->rightTableVelocity - 128) << 8;
        }
        if (report->effectsKnob != PS3_ACCEL_CENTER) {
            out->effectsKnob = (report->effectsKnob - 128) << 8;
        }
        if (report->crossfader != PS3_ACCEL_CENTER) {
            out->crossfader = (report->crossfader - 128) << 8;
        }
        out->leftBlue |= report->leftBlue;
        out->leftRed |= report->leftRed;
        out->leftGreen |= report->leftGreen;
        out->rightBlue |= report->rightBlue;
        out->rightRed |= report->rightRed;
        out->rightGreen |= report->rightGreen;
#endif
    }
    if (universal_report) {
        PS3_REPORT *out = (PS3_REPORT *)buf;
        out->x |= report->x;
        out->a |= report->a;
        out->b |= report->b;
        out->y |= report->y;

        out->back |= report->back;
        out->start |= report->start;

        out->guide = report->guide;
        out->capture = report->capture;

        if (dpad) {
            PS3Dpad_Data_t *out_dpad_report = (PS3Dpad_Data_t *)buf;
            out_dpad_report->dpad = dpad_report->dpad;
        }
#if DEVICE_TYPE_IS_GAMEPAD
        if (report->leftStickX != PS3_STICK_CENTER) {
            out->leftStickX = report->leftStickX;
        }
        if (report->leftStickY != PS3_STICK_CENTER) {
            out->leftStickY = report->leftStickY;
        }
        if (report->rightStickX != PS3_STICK_CENTER) {
            out->rightStickX = report->rightStickX;
        }
        if (report->rightStickY != PS3_STICK_CENTER) {
            out->rightStickY = report->rightStickY;
        }
        if (report->leftTrigger) {
            out->leftTrigger = report->leftTrigger;
        }
        if (report->rightTrigger) {
            out->rightTrigger = report->rightTrigger;
        }
        if (report->pressureDpadUp) {
            out->pressureDpadUp = report->pressureDpadUp;
        }
        if (report->pressureDpadRight) {
            out->pressureDpadRight = report->pressureDpadRight;
        }
        if (report->pressureDpadDown) {
            out->pressureDpadDown = report->pressureDpadDown;
        }
        if (report->pressureDpadLeft) {
            out->pressureDpadLeft = report->pressureDpadLeft;
        }
        if (report->pressureL1) {
            out->pressureL1 = report->pressureL1;
        }
        if (report->pressureR1) {
            out->pressureR1 = report->pressureR1;
        }
        if (report->pressureTriangle) {
            out->pressureTriangle = report->pressureTriangle;
        }
        if (report->pressureCircle) {
            out->pressureCircle = report->pressureCircle;
        }
        if (report->pressureCross) {
            out->pressureCross = report->pressureCross;
        }
        if (report->pressureSquare) {
            out->pressureSquare = report->pressureSquare;
        }
        out->l2 |= report->l2;
        out->r2 |= report->r2;
        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
        out->leftThumbClick |= report->leftThumbClick;
        out->rightThumbClick |= report->rightThumbClick;
#elif DEVICE_TYPE == DRUMS && RHYTHM_TYPE == GUITAR_HERO
        if (report->yellowVelocity) {
            out->yellowVelocity = report->yellowVelocity;
        }
        if (report->redVelocity) {
            out->redVelocity = report->redVelocity;
        }
        if (report->greenVelocity) {
            out->greenVelocity = report->greenVelocity;
        }
        if (report->blueVelocity) {
            out->blueVelocity = report->blueVelocity;
        }
        if (report->kickVelocity) {
            out->kickVelocity = report->kickVelocity;
        }
        if (report->orangeVelocity) {
            out->orangeVelocity = report->orangeVelocity;
        }
        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
#elif DEVICE_TYPE == DRUMS && RHYTHM_TYPE == ROCK_BAND
        if (report->yellowVelocity) {
            out->yellowVelocity = report->yellowVelocity;
        }
        if (report->redVelocity) {
            out->redVelocity = report->redVelocity;
        }
        if (report->greenVelocity) {
            out->greenVelocity = report->greenVelocity;
        }
        if (report->blueVelocity) {
            out->blueVelocity = report->blueVelocity;
        }
        out->padFlag = report->padFlag;
        out->cymbalFlag = report->cymbalFlag;
        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
#elif DEVICE_TYPE_IS_LIVE_GUITAR
        if (report->tilt_pc != PS3_STICK_CENTER) {
            if (output_console_type == PS3 || output_console_type == REAL_PS3) {
                out->tilt = (PS3_ACCEL_CENTER + (report->tilt_pc - 128) - GUITAR_ONE_G);
            } else {
                out->tilt_pc = report->tilt_pc;
            }
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = report->whammy;
        }
        if (report->strumBar != PS3_STICK_CENTER) {
            out->strumBar = report->strumBar;
        }
        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
#elif DEVICE_TYPE == GUITAR && RHYTHM_TYPE == GUITAR_HERO
        if (report->tilt_pc != PS3_STICK_CENTER) {
            if (output_console_type == PS3 || output_console_type == REAL_PS3) {
                out->tilt = (PS3_ACCEL_CENTER + (report->tilt_pc - 128) - GUITAR_ONE_G);
            } else {
                out->tilt_pc = report->tilt_pc;
            }
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = report->whammy;
        }
        if (report->slider != PS3_STICK_CENTER) {
            out->slider = report->slider;
        }
        out->leftShoulder |= report->leftShoulder;
        out->rightShoulder |= report->rightShoulder;
#elif DEVICE_TYPE == GUITAR && RHYTHM_TYPE == ROCK_BAND
        if (report->tilt_pc != PS3_STICK_CENTER) {
            // TODO: what are we actually setting tilt_pc to these days?
            if (output_console_type == PS3 || output_console_type == REAL_PS3) {
                out->tilt = report->tilt_pc > 128;
            } else {
                out->tilt_pc = report->tilt_pc;
            }
        }
        if (report->whammy != PS3_STICK_CENTER) {
            out->whammy = report->whammy;
        }
        if (report->pickup != PS3_STICK_CENTER) {
            out->pickup = report->pickup;
        }
        out->leftShoulder |= report->leftShoulder;
#elif DEVICE_TYPE == DJ_HERO_TURNTABLE

        if (report->leftTableVelocity != PS3_STICK_CENTER) {
            out->leftTableVelocity = report->leftTableVelocity;
        }
        if (report->rightTableVelocity != PS3_STICK_CENTER) {
            out->rightTableVelocity = report->rightTableVelocity;
        }
        if (report->effectsKnob != PS3_ACCEL_CENTER) {
            out->effectsKnob = report->effectsKnob;
        }
        if (report->crossfader != PS3_ACCEL_CENTER) {
            out->crossfader = report->crossfader;
        }
        out->leftBlue |= report->leftBlue;
        out->leftRed |= report->leftRed;
        out->leftGreen |= report->leftGreen;
        out->rightBlue |= report->rightBlue;
        out->rightRed |= report->rightRed;
        out->rightGreen |= report->rightGreen;
        out->tableNeutral |= report->tableNeutral;
#endif
    }
}
#endif
#define COPY_BUTTON(in_button, out_button) \
    if (in_button) out_button = true;
#ifndef RF_RX
uint8_t tick_inputs(void *buf, USB_LastReport_Data_t *last_report, uint8_t output_console_type) {
    uint8_t size = 0;
// Tick Inputs
#include "inputs/gh5_neck.h"
#include "inputs/ps2.h"
#include "inputs/turntable.h"
#include "inputs/usb_host_shared.h"
#include "inputs/wii.h"
#include "inputs/wt_neck.h"
    TICK_SHARED;
    // We tick the guitar every 5ms to handle inputs if nothing is attempting to read, but this doesn't need to output that data anywhere.
    if (!buf) return 0;
    // Handle button combos for detection logic
    if (millis() < 2000 && consoleType == UNIVERSAL) {
        TICK_DETECTION;
    }
    // Tick all three reports, and then go for the first one that has changes
    // We prioritise NKRO, then Consumer, because these are both only buttons
    // Then mouse, as it is an axis so it is more likley to have changes
#if CONSOLE_TYPE == KEYBOARD_MOUSE
    void *lastReportToCheck;
    for (int i = 1; i < REPORT_ID_END; i++) {
#ifdef TICK_MOUSE
        if (i == REPORT_ID_MOUSE) {
            size = sizeof(USB_Mouse_Data_t);
            memset(buf, 0, size);
            USB_Mouse_Data_t *report = (USB_Mouse_Data_t *)buf;
            report->rid = REPORT_ID_MOUSE;
            TICK_MOUSE;
            if (last_report) {
                lastReportToCheck = &last_report->lastMouseReport;
            }
        }
#endif
#ifdef TICK_CONSUMER
        if (i == REPORT_ID_CONSUMER) {
            size = sizeof(USB_ConsumerControl_Data_t);
            memset(buf, 0, size);
            USB_ConsumerControl_Data_t *report = (USB_ConsumerControl_Data_t *)buf;
            report->rid = REPORT_ID_CONSUMER;
            TICK_CONSUMER;
            if (last_report) {
                lastReportToCheck = &last_report->lastConsumerReport;
            }
        }
#endif
#ifdef TICK_NKRO
        if (i == REPORT_ID_NKRO) {
            size = sizeof(USB_NKRO_Data_t);
            memset(buf, 0, size);
            USB_NKRO_Data_t *report = (USB_NKRO_Data_t *)buf;
            report->rid = REPORT_ID_NKRO;
            TICK_NKRO;
            if (last_report) {
                lastReportToCheck = &last_report->lastNKROReport;
            }
        }
#endif
        // If we are directly asked for a HID report, always just reply with the NKRO one
        if (lastReportToCheck) {
            uint8_t cmp = memcmp(lastReportToCheck, buf, size);
            if (cmp == 0) {
                size = 0;
                continue;
            }
            memcpy(lastReportToCheck, buf, size);
            break;
        } else {
            break;
        }
    }
#else
    bool rf_or_bluetooth = false;
#ifdef RF_RX
    rf_or_bluetooth = buf == &last_report_rf.lastControllerReport;
#endif
#if BLUETOOTH
    rf_or_bluetooth = buf == &last_bt_report;
#endif
    USB_Report_Data_t *report_data = (USB_Report_Data_t *)buf;
    uint8_t report_size;
    bool updateSequence = false;
    bool updateHIDSequence = false;
    // GUITAR_HERO + XB1 + DRUMS or GUITAR is an invalid state that wont compile, same with turntable
#if DEVICE_TYPE != DJ_HERO_TURNTABLE && ((DEVICE_TYPE != DRUMS && DEVICE_TYPE != GUITAR) || RHYTHM_TYPE == ROCK_BAND)
    if (output_console_type == XBOXONE) {
        // The GHL guitar is special. It uses a standard nav report in the xbox menus, but then in game, it uses the ps3 report.
        // To switch modes, a poke command is sent every 8 seconds
        // In nav mode, we handle things like a controller, while in ps3 mode, we fall through and just set the report using ps3 mode.

        if (!DEVICE_TYPE_IS_LIVE_GUITAR || millis() - last_ghl_poke_time < 8000) {
            XBOX_ONE_REPORT *report = (XBOX_ONE_REPORT *)buf;
            size = sizeof(XBOX_ONE_REPORT);
            report_size = size - sizeof(GipHeader_t);
            memset(buf, 0, size);
            GIP_HEADER(report, GIP_INPUT_REPORT, false, report_sequence_number);
            TICK_XBOX_ONE;

#if DEVICE_TYPE == GUITAR
#define XB1_SOLO
#define COPY_TILT(tilt_in) \
    if (tilt_in) report->tilt = tilt_in;
#endif
#if DEVICE_TYPE == DRUMS
// xb1, drum is 4bit, so 8bit -> 4bit
#define COPY_DRUM_VELOCITY_GREEN(velocity_in) report->greenVelocity = velocity_in >> 4;
#define COPY_DRUM_VELOCITY_YELLOW(velocity_in) report->yellowVelocity = velocity_in >> 4;
#define COPY_DRUM_VELOCITY_RED(velocity_in) report->redVelocity = velocity_in >> 4;
#define COPY_DRUM_VELOCITY_BLUE(velocity_in) report->blueVelocity = velocity_in >> 4;
#define COPY_DRUM_VELOCITY_GREEN_CYMBAL(velocity_in) report->greenCymbalVelocity = velocity_in >> 4;
#define COPY_DRUM_VELOCITY_YELLOW_CYMBAL(velocity_in) report->yellowCymbalVelocity = velocity_in >> 4;
#define COPY_DRUM_VELOCITY_BLUE_CYMBAL(velocity_in) report->blueCymbalVelocity = velocity_in >> 4;
#endif
#if !DEVICE_TYPE_IS_LIVE_GUITAR
// Map from int16_t to xb1 (so keep it the same)
#define COPY_AXIS_NORMAL(in, out) \
    if (in) out = in;
// Map from uint16_t to xb1 (so keep it the same)
#define COPY_AXIS_TRIGGER(in, out) \
    if (in) out = in;
#include "inputs/usb_host.h"
#undef COPY_AXIS_NORMAL
#undef COPY_AXIS_TRIGGER
#undef COPY_TILT
#undef XB1_SOLO
#undef COPY_DRUM_VELOCITY_GREEN
#undef COPY_DRUM_VELOCITY_YELLOW
#undef COPY_DRUM_VELOCITY_RED
#undef COPY_DRUM_VELOCITY_BLUE
#undef COPY_DRUM_VELOCITY_GREEN_CYMBAL
#undef COPY_DRUM_VELOCITY_YELLOW_CYMBAL
#undef COPY_DRUM_VELOCITY_BLUE_CYMBAL
#endif
            if (report->guide != lastXboxOneGuide) {
                lastXboxOneGuide = report->guide;
                GipKeystroke_t *keystroke = (GipKeystroke_t *)buf;
                GIP_HEADER(keystroke, GIP_VIRTUAL_KEYCODE, true, keystroke_sequence_number++);
                keystroke->pressed = report->guide;
                keystroke->keycode = GIP_VKEY_LEFT_WIN;
                return sizeof(GipKeystroke_t);
            }
            // We use an unused bit as a flag for sending the guide key code, so flip it back
            report->guide = false;
            GipPacket_t *packet = (GipPacket_t *)buf;
            report_data = (USB_Report_Data_t *)packet->data;
            updateSequence = true;
        } else {
            XboxOneGHLGuitar_Data_t *report = (XboxOneGHLGuitar_Data_t *)buf;
            size = sizeof(XboxOneGHLGuitar_Data_t);
            report_size = sizeof(PS3_REPORT);
            memset(buf, 0, sizeof(XboxOneGHLGuitar_Data_t));
            GIP_HEADER(report, GHL_HID_REPORT, false, hid_sequence_number);
            report_data = (USB_Report_Data_t *)&report->report;
            updateHIDSequence = true;
        }
    }
#endif
    if (output_console_type == WINDOWS_XBOX360 || output_console_type == STAGE_KIT) {
        XINPUT_REPORT *report = (XINPUT_REPORT *)report_data;
        memset(report_data, 0, sizeof(XINPUT_REPORT));
        report->rid = 0;
        report->rsize = sizeof(XINPUT_REPORT);
// Whammy on the xbox guitars goes from min to max, so it needs to default to min
#if DEVICE_TYPE_IS_GUITAR
        report->whammy = INT16_MIN;
#endif
        TICK_XINPUT;

#if DEVICE_TYPE_IS_GUITAR
        // tilt_in is uint8, report->tilt is int16_t
#define COPY_TILT(tilt_in) \
    if (tilt_in) report->tilt = (tilt_in - 128) << 8;
#endif

// xb360 is stupid
#if DEVICE_TYPE == DRUMS
#if RHYTHM_TYPE == ROCK_BAND
#define COPY_DRUM_VELOCITY_GREEN(velocity_in) report->greenVelocity = -((0x7fff - (velocity_in << 8)));
#define COPY_DRUM_VELOCITY_YELLOW(velocity_in) report->yellowVelocity = -((0x7fff - (velocity_in << 8)));
#define COPY_DRUM_VELOCITY_RED(velocity_in) report->redVelocity = ((0x7fff - (velocity_in << 8)));
#define COPY_DRUM_VELOCITY_BLUE(velocity_in) report->blueVelocity = ((0x7fff - (velocity_in << 8)));
#else
#define COPY_DRUM_VELOCITY_GREEN(velocity_in) report->greenVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_YELLOW(velocity_in) report->yellowVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_RED(velocity_in) report->redVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_BLUE(velocity_in) report->blueVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_ORANGE(velocity_in) report->orangeVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_KICK(velocity_in) report->kickVelocity = velocity_in;
#endif
#define COPY_DRUM_VELOCITY_GREEN_CYMBAL(velocity_in)
#define COPY_DRUM_VELOCITY_YELLOW_CYMBAL(velocity_in)
#define COPY_DRUM_VELOCITY_BLUE_CYMBAL(velocity_in)
#endif
// Map from int16_t to xb360
#define COPY_AXIS_NORMAL(in, out) \
    if (in) out = in;
// Map from uint16_t to xb360 (to shift to get to uint8_t)
#define COPY_AXIS_TRIGGER(in, out) \
    if (in) out = in >> 8;
#include "inputs/usb_host.h"
#undef COPY_AXIS_NORMAL
#undef COPY_AXIS_TRIGGER
#undef COPY_TILT
#undef COPY_DRUM_VELOCITY_GREEN
#undef COPY_DRUM_VELOCITY_YELLOW
#undef COPY_DRUM_VELOCITY_RED
#undef COPY_DRUM_VELOCITY_BLUE
#undef COPY_DRUM_VELOCITY_ORANGE
#undef COPY_DRUM_VELOCITY_KICK
#undef COPY_DRUM_VELOCITY_GREEN_CYMBAL
#undef COPY_DRUM_VELOCITY_YELLOW_CYMBAL
#undef COPY_DRUM_VELOCITY_BLUE_CYMBAL
        report_size = size = sizeof(XINPUT_REPORT);
    }
// Guitars and Drums can fall back to their PS3 versions, so don't even include the PS4 version there.
// DJ Hero was never on ps4, so we can't really implement that either, so just fall back to PS3 there too.
#if SUPPORTS_PS4
    if (output_console_type == PS4) {
        if (millis() > 450000 && !auth_ps4_controller_found) {
            reset_usb();
        }
        PS4_REPORT *report = (PS4_REPORT *)report_data;
        memset(report, 0, sizeof(PS4_REPORT));
        PS4Dpad_Data_t *gamepad = (PS4Dpad_Data_t *)report;
        gamepad->report_id = 0x01;
        gamepad->leftStickX = PS3_STICK_CENTER;
        gamepad->leftStickY = PS3_STICK_CENTER;
        gamepad->rightStickX = PS3_STICK_CENTER;
        gamepad->rightStickY = PS3_STICK_CENTER;
        // PS4 does not start using the controller until it sees a PS button press.
        if (!seen_ps4_console) {
            report->guide = true;
        }
#if DEVICE_TYPE == LIVE_GUITAR
        // tilt_in is uint8, report->tilt is int16_t
#define COPY_TILT(tilt_in) \
    if (tilt_in) report->tilt = tilt_in;
#endif
// Map from int16_t to ps4 (uint8_t)
#define COPY_AXIS_NORMAL(in, out) \
    if (in) out = (in >> 8) + 128;
// Map from uint16_t to ps4 (uint8_t)
#define COPY_AXIS_TRIGGER(in, out) \
    if (in) out = in >> 8;
#define HAS_L2_R2_BUTTON
#include "inputs/usb_host.h"
#undef COPY_AXIS_NORMAL
#undef COPY_AXIS_TRIGGER
#undef HAS_L2_R2_BUTTON
#undef COPY_TILT
        TICK_PS4;
        gamepad->dpad = (gamepad->dpad & 0xf) > 0x0a ? 0x08 : dpad_bindings[gamepad->dpad];
        report_size = size = sizeof(PS4_REPORT);
    }
#endif
// If we are dealing with a non instrument controller (like a gamepad) then we use the proper ps3 controller report format, to allow for emulator support and things like that
// This also gives us PS2 support via PADEMU and wii support via fakemote for standard controllers.
// However, actual ps3 support was being a pain so we use the instrument descriptor there, since the ps3 doesn't care.
#if !(DEVICE_TYPE_IS_INSTRUMENT)
    if (output_console_type == PS3) {
        PS3Gamepad_Data_t *report = (PS3Gamepad_Data_t *)report_data;
        memset(report, 0, sizeof(PS3_REPORT));
        report->reportId = 1;
        report->accelX = PS3_ACCEL_CENTER;
        report->accelY = PS3_ACCEL_CENTER;
        report->accelZ = PS3_ACCEL_CENTER;
        report->gyro = PS3_ACCEL_CENTER;
        report->leftStickX = PS3_STICK_CENTER;
        report->leftStickY = PS3_STICK_CENTER;
        report->rightStickX = PS3_STICK_CENTER;
        report->rightStickY = PS3_STICK_CENTER;

// Map from int16_t to ps3 (uint8_t)
#define COPY_AXIS_NORMAL(in, out) \
    if (in) out = (in >> 8) + 128;
// Map from uint16_t to ps3 (uint8_t)
#define COPY_AXIS_TRIGGER(in, out) \
    if (in) out = in >> 8;
#define HAS_L2_R2_BUTTON
#include "inputs/usb_host.h"
#undef HAS_L2_R2_BUTTON
#undef COPY_AXIS_NORMAL
#undef COPY_AXIS_TRIGGER
        TICK_PS3;
        report_size = size = sizeof(PS3Gamepad_Data_t);
    }
    if (output_console_type != WINDOWS_XBOX360 && output_console_type != PS3 && output_console_type != PS4 && output_console_type != STAGE_KIT && !updateHIDSequence) {
#else
    // For instruments, we instead use the below block, as our universal and PS3 descriptors use the same report format in that case
    if (output_console_type != WINDOWS_XBOX360 && output_console_type != PS4 && output_console_type != STAGE_KIT && !updateHIDSequence) {
#endif
        PS3_REPORT *report = (PS3_REPORT *)report_data;
        if (output_console_type == UNIVERSAL) {
            PS3Universal_Data_t *universal_report = (PS3Universal_Data_t *)report_data;
            report = (PS3_REPORT *)(universal_report->report);
            universal_report->report_id = 1;
        }
        memset(report, 0, sizeof(PS3_REPORT));
        PS3Dpad_Data_t *gamepad = (PS3Dpad_Data_t *)report;
        gamepad->accelX = PS3_ACCEL_CENTER;
        gamepad->accelY = PS3_ACCEL_CENTER;
        gamepad->accelZ = PS3_ACCEL_CENTER;
        gamepad->gyro = PS3_ACCEL_CENTER;
        gamepad->leftStickX = PS3_STICK_CENTER;
        gamepad->leftStickY = PS3_STICK_CENTER;
        gamepad->rightStickX = PS3_STICK_CENTER;
        gamepad->rightStickY = PS3_STICK_CENTER;
#if DEVICE_TYPE == DRUMS
#if RHYTHM_TYPE == ROCK_BAND
#define COPY_DRUM_VELOCITY_GREEN(velocity_in) report->greenVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_YELLOW(velocity_in) report->yellowVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_RED(velocity_in) report->redVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_BLUE(velocity_in) report->blueVelocity = velocity_in;
#else
#define COPY_DRUM_VELOCITY_GREEN(velocity_in) report->greenVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_YELLOW(velocity_in) report->yellowVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_RED(velocity_in) report->redVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_BLUE(velocity_in) report->blueVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_ORANGE(velocity_in) report->orangeVelocity = velocity_in;
#define COPY_DRUM_VELOCITY_KICK(velocity_in) report->kickVelocity = velocity_in;
#endif
#define COPY_DRUM_VELOCITY_GREEN_CYMBAL(velocity_in)
#define COPY_DRUM_VELOCITY_YELLOW_CYMBAL(velocity_in)
#define COPY_DRUM_VELOCITY_BLUE_CYMBAL(velocity_in)
#endif
#if DEVICE_TYPE == GUITAR && RHYTHM_TYPE == ROCK_BAND
// TODO: what do we want to use as our condition for tilt
#define COPY_TILT(tilt_in)                      \
    if (consoleType == UNIVERSAL) {             \
        if (tilt_in) report->tilt_pc = tilt_in; \
    } else if (tilt_in > 128) {                 \
        report->tilt = true;                    \
    }
#endif
#if (DEVICE_TYPE == GUITAR && RHYTHM_TYPE == GUITAR_HERO) || DEVICE_TYPE == LIVE_GUITAR
#define COPY_TILT(tilt_in)                   \
    if (consoleType == UNIVERSAL) {          \
        report->tilt_pc = tilt_in;           \
    } else if (tilt_in) {                    \
        report->tilt = (tilt_in + 128) << 2; \
    }
#endif

// Map from int16_t to ps3 (uint8_t)
#define COPY_AXIS_NORMAL(in, out) \
    if (in) out = (in >> 8) + 128;
// Map from uint16_t to ps3 (uint8_t)
#define COPY_AXIS_TRIGGER(in, out) \
    if (in) out = in >> 8;
#define HAS_L2_R2_BUTTON
#include "inputs/usb_host.h"
#undef COPY_AXIS_NORMAL
#undef COPY_AXIS_TRIGGER
#undef HAS_L2_R2_BUTTON
#undef COPY_TILT
#undef COPY_DRUM_VELOCITY_GREEN
#undef COPY_DRUM_VELOCITY_YELLOW
#undef COPY_DRUM_VELOCITY_RED
#undef COPY_DRUM_VELOCITY_BLUE
#undef COPY_DRUM_VELOCITY_ORANGE
#undef COPY_DRUM_VELOCITY_KICK
#undef COPY_DRUM_VELOCITY_GREEN_CYMBAL
#undef COPY_DRUM_VELOCITY_YELLOW_CYMBAL
#undef COPY_DRUM_VELOCITY_BLUE_CYMBAL
        TICK_PS3;
#if DEVICE_TYPE == DJ_HERO_TURNTABLE
        if (!report->leftBlue && !report->leftRed && !report->leftGreen && !report->rightBlue && !report->rightRed && !report->rightGreen) {
            report->tableNeutral = true;
        }
#endif
        gamepad->dpad = (gamepad->dpad & 0xf) > 0x0a ? 0x08 : dpad_bindings[gamepad->dpad];
        // Switch swaps a and b
        if (output_console_type == SWITCH) {
            bool a = gamepad->a;
            bool b = gamepad->b;
            gamepad->b = a;
            gamepad->a = b;
        }
        report_size = size = sizeof(PS3_REPORT);

        if (output_console_type == UNIVERSAL) {
            report_size += 1;
            size += 1;
        }
    }
    // If we are being asked for a HID report (aka via HID_GET_REPORT), then just send whatever inputs we have, do not compare
    if (last_report) {
        uint8_t cmp = memcmp(&last_report->lastControllerReport, report_data, report_size);
        if (cmp == 0) {
            return 0;
        }
        memcpy(&last_report->lastControllerReport, report_data, report_size);
    }
// Standard PS4 controllers need a report counter, but we don't want to include that when comparing so we add it here
#if DEVICE_TYPE_IS_GAMEPAD
    if (consoleType == PS4) {
        PS4Gamepad_Data_t *gamepad = (PS4Gamepad_Data_t *)report_data;
        gamepad->reportCounter = ps4_sequence_number++;
    }
#endif
#if CONSOLE_TYPE == UNIVERSAL || CONSOLE_TYPE == XBOXONE
    if (updateSequence) {
        report_sequence_number++;
        if (report_sequence_number == 0) {
            report_sequence_number = 1;
        }
    } else if (updateHIDSequence) {
        hid_sequence_number++;
        if (hid_sequence_number == 0) {
            hid_sequence_number = 1;
        }
    }
#endif
#endif
    return size;
}
#else
uint8_t tick_inputs(void *buf, USB_LastReport_Data_t *last_report, uint8_t output_console_type) {
    uint8_t rf_size;
    uint8_t size;
    uint8_t pipe;
    bool updated = false;
    if (radio.available(&pipe)) {
        rf_connected = true;
        radio.read(rf_data, sizeof(rf_data));
        rf_size = radio.getDynamicPayloadSize();
        switch (rf_data[0]) {
            case ConsoleType:
                set_console_type(rf_data[1]);
                if (last_report) {
                    return 0;
                }
                break;
            case Input:
                updated = true;
                break;
            case Heartbeat:
                if (last_report) {
                    return 0;
                }
                break;
        }
#if DEVICE_TYPE_KEYBOARD
        if (!updated) {
            size = sizeof(USB_NKRO_Data_t);
            memcpy(&last_report->lastNKROReport, buf, size);
            return size;
        }
#ifdef TICK_NKRO
        if (rf_data[1] == REPORT_ID_NKRO) {
            memcpy(&last_rf_inputs[pipe].lastNKROReport, rf_data + 1, rf_size - 1);
        }
#endif
#ifdef TICK_MOUSE
        if (rf_data[1] == REPORT_ID_MOUSE) {
            memcpy(&last_rf_inputs[pipe].lastMouseReport, rf_data + 1, rf_size - 1);
        }
#endif
#ifdef TICK_CONSUMER
        if (rf_data[1] == REPORT_ID_CONSUMER) {
            memcpy(&last_rf_inputs[pipe].lastConsumerReport, rf_data + 1, rf_size - 1);
        }
#endif
        void *lastReportToCheck;
        for (int i = 1; i < REPORT_ID_END; i++) {
            for (int dev = 0; dev < RF_COUNT; dev++) {
#ifdef TICK_MOUSE
                if (i == REPORT_ID_MOUSE) {
                    USB_Mouse_Data_t *report = (USB_Mouse_Data_t *)buf;
                    size = sizeof(USB_Mouse_Data_t);
                    if (dev == 0) {
                        memset(buf, 0, size);
                    }
                    report->rid = REPORT_ID_MOUSE;
                    // Since the mouse report has analog inputs we need to handle it manually
                    USB_Mouse_Data_t *report_from_rf = &last_rf_inputs[pipe].lastMouseReport;
                    report->left |= report_from_rf->left;
                    report->right |= report_from_rf->right;
                    report->middle |= report_from_rf->middle;
                    if (report_from_rf->X) {
                        report->X = report_from_rf->X;
                    }
                    if (report_from_rf->Y) {
                        report->Y = report_from_rf->Y;
                    }
                    if (report_from_rf->ScrollX) {
                        report->ScrollX = report_from_rf->ScrollX;
                    }
                    if (report_from_rf->ScrollY) {
                        report->ScrollY = report_from_rf->ScrollY;
                    }
                    if (last_report) {
                        lastReportToCheck = &last_report->lastMouseReport;
                    }
                }
#endif
#ifdef TICK_CONSUMER
                if (i == REPORT_ID_CONSUMER) {
                    size = sizeof(USB_ConsumerControl_Data_t);
                    if (dev == 0) {
                        memset(buf, 0, size);
                    }
                    // Consumer is easy, as we are only dealing with bits per button, so ORing is fine
                    uint8_t *current_report = (uint8_t *)&last_rf_inputs[pipe];
                    for (size_t j = 0; j < size; j++) {
                        buf[j] |= current_report[j];
                    }
                    if (last_report) {
                        lastReportToCheck = &last_report->lastConsumerReport;
                    }
                }
#endif
#ifdef TICK_NKRO
                if (i == REPORT_ID_NKRO) {
                    size = sizeof(USB_NKRO_Data_t);
                    if (dev == 0) {
                        memset(buf, 0, size);
                    }
                    // NKRO is easy, as we are only dealing with bits per button, so ORing is fine
                    uint8_t *current_report = (uint8_t *)&last_rf_inputs[pipe];
                    for (size_t j = 0; j < size; j++) {
                        buf[j] |= current_report[j];
                    }
                    if (last_report) {
                        lastReportToCheck = &last_report->lastNKROReport;
                    }
                }
#endif
            }
            // If we are directly asked for a HID report, always just reply with the NKRO one
            if (lastReportToCheck) {
                uint8_t cmp = memcmp(lastReportToCheck, buf, size);
                if (cmp == 0) {
                    size = 0;
                    continue;
                }
                memcpy(lastReportToCheck, buf, size);
                break;
            } else {
                break;
            }
        }
#else
        memcpy(&last_rf_inputs[pipe].lastControllerReport, rf_data + 1, rf_size - 1);
        USB_Report_Data_t *report_data = (USB_Report_Data_t *)buf;
        uint8_t report_size;
        bool updateSequence = false;
        bool updateHIDSequence = false;
        if (consoleType == XBOXONE) {
            // The GHL guitar is special. It uses a standard nav report in the xbox menus, but then in game, it uses the ps3 report.
            // To switch modes, a poke command is sent every 8 seconds
            // In nav mode, we handle things like a controller, while in ps3 mode, we fall through and just set the report using ps3 mode.

            if (!DEVICE_TYPE_IS_LIVE_GUITAR || millis() - last_ghl_poke_time < 8000) {
                XBOX_ONE_REPORT *report = (XBOX_ONE_REPORT *)buf;
                size = sizeof(XBOX_ONE_REPORT);
                report_size = size - sizeof(GipHeader_t);
                if (updated) {
                    memset(buf, 0, size);
                }
                GIP_HEADER(report, GIP_INPUT_REPORT, false, report_sequence_number);
                if (updated) {
                    for (int dev = 0; dev < RF_COUNT; dev++) {
                        convert_ps3_to_type((uint8_t *)buf, &last_rf_inputs[dev].lastControllerReport, consoleType);
                    }
                }
                if (report->guide != lastXboxOneGuide) {
                    lastXboxOneGuide = report->guide;
                    GipKeystroke_t *keystroke = (GipKeystroke_t *)buf;
                    GIP_HEADER(keystroke, GIP_VIRTUAL_KEYCODE, true, keystroke_sequence_number++);
                    keystroke->pressed = report->guide;
                    keystroke->keycode = GIP_VKEY_LEFT_WIN;
                    return sizeof(GipKeystroke_t);
                }
                // We use an unused bit as a flag for sending the guide key code, so flip it back
                report->guide = false;
                GipPacket_t *packet = (GipPacket_t *)buf;
                report_data = (USB_Report_Data_t *)packet->data;
                updateSequence = true;
            } else {
                XboxOneGHLGuitar_Data_t *report = (XboxOneGHLGuitar_Data_t *)buf;
                size = sizeof(XboxOneGHLGuitar_Data_t);
                report_size = sizeof(PS3_REPORT);
                if (updated) {
                    memset(buf, 0, sizeof(XboxOneGHLGuitar_Data_t));
                }
                GIP_HEADER(report, GHL_HID_REPORT, false, hid_sequence_number);
                report_data = (USB_Report_Data_t *)&report->report;
                updateHIDSequence = true;
            }
        }
        if (consoleType == WINDOWS_XBOX360 || consoleType == STAGE_KIT) {
            XINPUT_REPORT *report = (XINPUT_REPORT *)report_data;
            if (updated) {
                memset(report_data, 0, sizeof(XINPUT_REPORT));
                report->rid = 0;
                report->rsize = sizeof(XINPUT_REPORT);
// Whammy on the xbox guitars goes from min to max, so it needs to default to min
#if DEVICE_TYPE_IS_GUITAR
                report->whammy = INT16_MIN;
#endif
                for (int dev = 0; dev < RF_COUNT; dev++) {
                    convert_ps3_to_type((uint8_t *)report_data, &last_rf_inputs[dev].lastControllerReport, WINDOWS_XBOX360);
                }
            }
            report_size = size = sizeof(XINPUT_REPORT);
        }
// Guitars and Drums can fall back to their PS3 versions, so don't even include the PS4 version there.
// DJ Hero was never on ps4, so we can't really implement that either, so just fall back to PS3 there too.
#if SUPPORTS_PS4
        if (consoleType == PS4) {
            PS4_REPORT *report = (PS4_REPORT *)report_data;
            if (updated) {
                PS4Gamepad_Data_t *gamepad = (PS4Gamepad_Data_t *)report;
                gamepad->report_id = 0x01;
                gamepad->leftStickX = PS3_STICK_CENTER;
                gamepad->leftStickY = PS3_STICK_CENTER;
                gamepad->rightStickX = PS3_STICK_CENTER;
                gamepad->rightStickY = PS3_STICK_CENTER;
#if !DEVICE_TYPE_IS_LIVE_GUITAR
                gamepad->reportCounter = ps4_sequence_number;
#endif

                for (int dev = 0; dev < RF_COUNT; dev++) {
                    convert_ps3_to_type((uint8_t *)report_data, &last_rf_inputs[dev].lastControllerReport, consoleType);
                }
            }
            report_size = size = sizeof(PS4_REPORT);
        }
#endif
// If we are dealing with a non instrument controller (like a gamepad) then we use the proper ps3 controller report format, to allow for emulator support and things like that
// This also gives us PS2 support via PADEMU and wii support via fakemote for standard controllers.
// However, actual ps3 support was being a pain so we use the instrument descriptor there, since the ps3 doesn't care.
#if !(DEVICE_TYPE_IS_INSTRUMENT)
        if (consoleType == PS3) {
            PS3Gamepad_Data_t *report = (PS3Gamepad_Data_t *)report_data;

            if (updated) {
                memset(report, 0, sizeof(PS3_REPORT));
                report->reportId = 1;
                for (int dev = 0; dev < RF_COUNT; dev++) {
                    convert_ps3_to_type((uint8_t *)report_data, &last_rf_inputs[dev].lastControllerReport, consoleType);
                }
                report_size = size = sizeof(PS3Gamepad_Data_t);
            }
            if (consoleType != WINDOWS_XBOX360 && consoleType != PS3 && consoleType != PS4 && consoleType != STAGE_KIT && !updateHIDSequence) {
#else
        // For instruments, we instead use the below block, as our universal and PS3 descriptors use the same report format in that case
        if (consoleType != WINDOWS_XBOX360 && consoleType != PS4 && consoleType != STAGE_KIT && !updateHIDSequence) {
#endif
                PS3_REPORT *report = (PS3_REPORT *)report_data;
                memset(report, 0, sizeof(PS3_REPORT));
                PCGamepad_Data_t *gamepad = (PCGamepad_Data_t *)report;
                gamepad->accelX = PS3_ACCEL_CENTER;
                gamepad->accelY = PS3_ACCEL_CENTER;
                gamepad->accelZ = PS3_ACCEL_CENTER;
                gamepad->gyro = PS3_ACCEL_CENTER;
                gamepad->leftStickX = PS3_STICK_CENTER;
                gamepad->leftStickY = PS3_STICK_CENTER;
                gamepad->rightStickX = PS3_STICK_CENTER;
                gamepad->rightStickY = PS3_STICK_CENTER;
                for (int dev = 0; dev < RF_COUNT; dev++) {
                    convert_ps3_to_type((uint8_t *)report_data, &last_rf_inputs[dev].lastControllerReport, REAL_PS3);
                }
#if DEVICE_TYPE == DJ_HERO_TURNTABLE
                if (!report->leftBlue && !report->leftRed && !report->leftGreen && !report->rightBlue && !report->rightRed && !report->rightGreen) {
                    report->tableNeutral = true;
                }
#endif
                // Switch swaps a and b
                if (consoleType == SWITCH) {
                    bool a = report->a;
                    bool b = report->b;
                    report->b = a;
                    report->a = b;
                }
            }
            report_size = size = sizeof(PS3_REPORT);
        }
        // If we are being asked for a HID report (aka via HID_GET_REPORT), then just send whatever inputs we have, do not compare
        if (last_report) {
            uint8_t cmp = memcmp(&last_report->lastControllerReport, report_data, report_size);
            if (cmp == 0) {
                return 0;
            }
            memcpy(&last_report->lastControllerReport, report_data, report_size);
        }
        if (consoleType == PS4) {
            ps4_sequence_number++;
        }
#if CONSOLE_TYPE == UNIVERSAL || CONSOLE_TYPE == XBOXONE
        if (updateSequence) {
            report_sequence_number++;
            if (report_sequence_number == 0) {
                report_sequence_number = 1;
            }
        } else if (updateHIDSequence) {
            hid_sequence_number++;
            if (hid_sequence_number == 0) {
                hid_sequence_number = 1;
            }
        }
#endif
#endif
    }
    return size;
}
#endif

#ifdef RF_TX
void tick_rf_tx(void) {
    uint8_t size = 0;
    uint8_t pipe;
    if (radio.available(&pipe)) {
        rf_connected = true;
        radio.read(rf_data, sizeof(rf_data));
        switch (rf_data[0]) {
            case AckAuthLed:
                handle_auth_led();
                break;
            case AckPlayerLed:
                handle_player_leds(rf_data[1]);
                break;
            case AckRumble:
                handle_rumble(rf_data[1], rf_data[2]);
                break;
            case AckKeyboardLed:
                handle_keyboard_leds(rf_data[1]);
                break;
        }
    }
    size = tick_inputs(&rf_report.lastControllerReport, &last_report_rf, UNIVERSAL);
    if (size > 0) {
        rf_connected = radio.write(&rf_report, size + 1);
    } else {
        rf_connected = radio.write(&rf_heartbeat, sizeof(rf_heartbeat));
    }
    return;
}
#endif
#if BLUETOOTH
bool tick_bluetooth(void) {
    uint8_t size = tick_inputs&bt_report, &last_report_bt, UNIVERSAL);
    if (size) {
        send_report(size, (uint8_t *)&bt_report);
    }
    return size;
}
#endif
#ifndef RF_ONLY
bool tick_usb(void) {
    uint8_t size = 0;
    bool ready = ready_for_next_packet();
    if (!ready) return 0;
    if (data_from_console_size) {
        send_report_to_controller(get_device_address_for(XBOXONE), data_from_console, data_from_console_size);
        data_from_console_size = 0;
    }
    // If we have something pending to send to the xbox one controller, send it
    if (consoleType == XBOXONE && xbox_one_state != Ready) {
        size = tick_xbox_one();
    }
    if (!size) {
        size = tick_inputs(&combined_report, &last_report_usb, consoleType);
    }
    send_report_to_pc(&combined_report, size);
    seen_ps4_console = true;
    return size;
}
#endif
void tick(void) {
#ifdef TICK_LED
    TICK_LED;
#endif
#ifndef RF_ONLY
    bool ready = tick_usb();
#endif
#ifdef RF_TX
    tick_rf_tx();
#endif
#if BLUETOOTH
    tick_bluetooth();
#endif
#if !defined(RF_TX) && !BLUETOOTH
    // Tick the controller every 5ms if this device is usb only, and usb is not ready
    if (!ready && millis() - lastSentPacket > 5) {
        lastSentPacket = millis();
        tick_inputs(NULL, NULL, consoleType);
    }
#endif
}

void device_reset(void) {
    xbox_one_state = Announce;
    data_from_controller_size = 0;
    data_from_console_size = 0;
    hid_sequence_number = 1;
    report_sequence_number = 1;
    last_ghl_poke_time = 0;
}

uint8_t last_len = false;
void receive_report_from_controller(uint8_t const *report, uint16_t len) {
    if (xbox_one_state != Auth) {
        return;
    }
    data_from_controller_size = len;
    memcpy(data_from_controller, report, len);
    if (report[0] == GIP_INPUT_REPORT) {
        report_sequence_number = report[2] + 1;
    }
}

void xinput_controller_connected(uint8_t vid, uint8_t pid, uint8_t subtype) {
    if (subtype == XINPUT_STAGE_KIT) {
        passthrough_stage_kit = true;
    }
    if (xbox_360_state == Authenticated) return;
    xbox_360_vid = vid;
    xbox_360_pid = pid;
}

void xone_controller_connected(uint8_t dev_addr) {
    GipPowerMode_t *powerMode = (GipPowerMode_t *)data_from_console;
    GIP_HEADER(powerMode, GIP_POWER_MODE_DEVICE_CONFIG, true, 1);
    powerMode->subcommand = 0x00;
    send_report_to_controller(dev_addr, data_from_console, sizeof(GipPowerMode_t));
}

void ps4_controller_connected(uint8_t dev_addr, uint16_t vid, uint16_t pid) {
    if (vid == SONY_VID && (pid == PS4_DS_PID_1 || pid == PS4_DS_PID_2 || pid == PS4_DS_PID_3)) {
        ps4_output_report report = {
            report_id : 0x05,
            valid_flag0 : 0xFF,
            valid_flag1 : 0x00,
            reserved1 : 0x00,
            motor_right : 0x00,
            motor_left : 0x00,
            lightbar_red : 0x00,
            lightbar_green : 0x00,
            lightbar_blue : 0xFF,
            lightbar_blink_on : 0,
            lightbar_blink_off : 0,
            reserved : {0}
        };
        send_report_to_controller(dev_addr, (uint8_t *)&report, sizeof(report));
    }
    auth_ps4_controller_found = true;
}

void ps4_controller_disconnected(void) {
    auth_ps4_controller_found = false;
}

void set_console_type(uint8_t new_console_type) {
    if (consoleType == new_console_type) return;
    consoleType = new_console_type;
    reset_usb();
}

#ifdef USB_HOST_STACK
USB_Device_Type_t get_usb_device_type_for(uint16_t vid, uint16_t pid) {
    USB_Device_Type_t type = {0, GAMEPAD};
    switch (vid) {
        case SONY_VID:
            switch (pid) {
                case SONY_DS3_PID:
                    type.console_type = PS3;
                    break;
                case PS4_DS_PID_1:
                case PS4_DS_PID_2:
                case PS4_DS_PID_3:
                    type.console_type = PS3;
                    break;
            }
            break;
        case REDOCTANE_VID:
            switch (pid) {
                case PS3_GH_GUITAR_PID:
                    type.console_type = PS3;
                    type.sub_type = GUITAR;
                    type.rhythm_type = GUITAR_HERO;
                    break;
                case PS3_GH_DRUM_PID:
                    type.console_type = PS3;
                    type.sub_type = DRUMS;
                    type.rhythm_type = GUITAR_HERO;
                    break;
                case PS3_RB_GUITAR_PID:
                    type.console_type = PS3;
                    type.sub_type = GUITAR;
                    type.rhythm_type = ROCK_BAND;
                    break;
                case PS3_RB_DRUM_PID:
                    type.console_type = PS3;
                    type.sub_type = DRUMS;
                    type.rhythm_type = ROCK_BAND;
                    break;
                case PS3_DJ_TURNTABLE_PID:
                    type.console_type = PS3;
                    type.sub_type = DJ_HERO_TURNTABLE;
                    break;
                case PS3WIIU_GHLIVE_DONGLE_PID:
                    type.console_type = PS3;
                    type.sub_type = LIVE_GUITAR;
                    break;
            }

        case WII_RB_VID:
            // Polled the same as PS3, so treat them as PS3 instruments
            if (pid == WII_RB_GUITAR_PID) {
                type.console_type = PS3;
                type.sub_type = GUITAR;
            } else if (pid == WII_RB_DRUM_PID) {
                type.console_type = PS3;
                type.sub_type = DRUMS;
            }
            break;

        case XBOX_ONE_RB_VID:
            if (pid == XBOX_ONE_RB_GUITAR_PID) {
                type.console_type = XBOXONE;
                type.sub_type = GUITAR;
            } else if (pid == XBOX_ONE_RB_DRUM_PID) {
                type.console_type = XBOXONE;
                type.sub_type = DRUMS;
            }
            break;

        case XBOX_ONE_GHLIVE_DONGLE_VID:
            if (pid == XBOX_ONE_GHLIVE_DONGLE_PID) {
                type.console_type = XBOXONE;
                type.sub_type = LIVE_GUITAR;
            }
            break;
    }
    return type;
}
#endif