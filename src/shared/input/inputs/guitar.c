#include "guitar.h"
#include "../pins/pins.h"
#include "config/eeprom.h"
#include "direct.h"
#include "mpu6050/inv_mpu.h"
#include "mpu6050/inv_mpu_dmp_motion_driver.h"
#include "mpu6050/mpu_math.h"
#include "util/util.h"
#include <stdbool.h>
#include <util/delay.h>
// Constants used by the mpu 6050
#define FSR 2000
//#define GYRO_SENS       ( 131.0f * 250.f / (float)FSR )
#define GYRO_SENS 16.375f
#define QUAT_SENS 1073741824.f // 2^30
union u_quat q;
int32_t z;
static float ypr[3];
AnalogInfo_t analog;
volatile bool ready = false;
void tickMPUTilt(Controller_t *controller) {
  static short sensors;
  static unsigned char fifoCount;
  if (ready) {
    ready = false;
    dmp_read_fifo(NULL, NULL, q._l, NULL, &sensors, &fifoCount);

    q._f.w = (float)q._l[0] / (float)QUAT_SENS;
    q._f.x = (float)q._l[1] / (float)QUAT_SENS;
    q._f.y = (float)q._l[2] / (float)QUAT_SENS;
    q._f.z = (float)q._l[3] / (float)QUAT_SENS;

    quaternionToEuler(&q._f, &ypr[2], &ypr[1], &ypr[0]);
    ypr[0] = wrap_pi(ypr[0]);
    ypr[1] = wrap_pi(ypr[1]);
    ypr[2] = wrap_pi(ypr[2]);
    z = (ypr[config.axis.mpu6050Orientation / 2] * (65535 / M_PI));
    if (config.axis.mpu6050Orientation & 1) { z = -z; }
    if (z > 32767) { z = 65535 - z; }
    z += config.axis.tiltSensitivity;
    z = constrain(z, 0, 32767);
    if (isnan(z)) { z = 0; }
  }
  controller->r_y = z;
}
void tickDigitalTilt(Controller_t *controller) {
  controller->r_y = (!digitalRead(config.pins.r_y.pin)) * 32767;
}
void (*tick)(Controller_t *controller) = NULL;
// Would it be worth only doing this check once for speed?
#define DRUM 1
#define GUITAR 2
uint8_t types[MIDI_ROCK_BAND_DRUMS+1] = {
    [PS3_GUITAR_HERO_DRUMS] = DRUM,     [PS3_ROCK_BAND_DRUMS] = DRUM,
    [WII_ROCK_BAND_DRUMS] = DRUM,       [XINPUT_ROCK_BAND_DRUMS] = DRUM,
    [XINPUT_GUITAR_HERO_DRUMS] = DRUM,  [MIDI_ROCK_BAND_DRUMS] = DRUM,
    [MIDI_GUITAR_HERO_DRUMS] = DRUM,    [PS3_GUITAR_HERO_GUITAR] = GUITAR,
    [PS3_ROCK_BAND_GUITAR] = GUITAR,    [WII_ROCK_BAND_GUITAR] = GUITAR,
    [XINPUT_ROCK_BAND_GUITAR] = GUITAR, [XINPUT_GUITAR_HERO_GUITAR] = GUITAR,
    [XINPUT_LIVE_GUITAR] = GUITAR,      [MIDI_ROCK_BAND_GUITAR] = GUITAR,
    [MIDI_GUITAR_HERO_GUITAR] = GUITAR, [MIDI_LIVE_GUITAR] = GUITAR,
    [KEYBOARD_GUITAR_HERO_DRUMS] = DRUM, [KEYBOARD_GUITAR_HERO_GUITAR] = GUITAR,
    [KEYBOARD_ROCK_BAND_DRUMS] = DRUM, [KEYBOARD_ROCK_BAND_GUITAR] = GUITAR,
    [KEYBOARD_LIVE_GUITAR] = GUITAR
};
bool isDrum(void) { return types[config.main.subType] == DRUM; }
bool isGuitar(void) { return types[config.main.subType] == GUITAR; }
void initMPU6050(unsigned int rate) {
  mpu_init(NULL);
  mpu_set_sensors(INV_XYZ_GYRO | INV_XYZ_ACCEL);
  mpu_set_gyro_fsr(FSR);
  mpu_set_accel_fsr(2);
  mpu_configure_fifo(INV_XYZ_GYRO | INV_XYZ_ACCEL);
  dmp_load_motion_driver_firmware();
  dmp_set_fifo_rate(rate);
  mpu_set_dmp_state(1);
  dmp_enable_feature(DMP_FEATURE_6X_LP_QUAT);
  dmp_set_interrupt_mode(DMP_INT_CONTINUOUS);
}
void initGuitar(void) {
  if (!isGuitar()) return;
  if (config.main.tiltType == MPU_6050) {
    initMPU6050(15);
    enablePCI(config.pins.r_y.pin);
    tick = tickMPUTilt;
  } else if (config.main.tiltType == DIGITAL) {
    pinMode(config.pins.r_y.pin, INPUT_PULLUP);
    tick = tickDigitalTilt;
  } else if (config.main.tiltType == ANALOGUE && config.main.inputType == WII) {
    initDirectInput();
    tick = tickDirectInput;
  }
}
int16_t r_x;
void tickGuitar(Controller_t *controller) {
  if (!isGuitar()) return;
  r_x = controller->r_x;
  // Whammy needs to be scaled so that it is picked up
  if (r_x > 0) r_x = 0;
  r_x = r_x << 1;
  if (r_x > 0) r_x = -32767;
  controller->r_x = -r_x;
  if (tick == NULL) return;
  tick(controller);
}
ISR(PCINT0_vect) { ready = true; }
#if defined(PCINT1_vect)
ISR(PCINT1_vect) { ready = true; }
ISR(PCINT2_vect) { ready = true; }
#endif