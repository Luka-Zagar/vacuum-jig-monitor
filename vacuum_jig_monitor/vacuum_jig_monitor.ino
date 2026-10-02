/**
 * @file    vacuum_jig_monitor.ino
 * @brief   Vacuum jig pressure monitor - proof of concept.
 * @version 1.0.0
 * @date    2026-10-02
 * @author  Luka Zagar
 * @copyright Copyright (c) 2026 Luka Zagar. Released under the MIT License,
 *            see LICENSE in the repository root.
 *
 * Reads a XIDIBEI XDB401 pressure transducer (0-8 bar gauge, 5 V supply,
 * ratiometric ~0.5-4.5 V output) with an Arduino Nano and streams the
 * pressure in a format compatible with the Arduino IDE Serial Plotter.
 *
 * Pressure scale: atmospheric pressure is defined as 1.000 bar,
 * a perfect vacuum as 0.000 bar.
 *
 * Board settings (Arduino IDE, Tools menu)
 *   Board:     Arduino Nano
 *   Processor: ATmega328P (Old Bootloader)
 *   Port:      the Nano's COM / tty port
 *
 *   Most Nano boards, including nearly all clones, ship with the old
 *   bootloader. If "ATmega328P (Old Bootloader)" is not selected, the
 *   upload fails with "avrdude: stk500_getsync(): not in sync" errors.
 *   Boards with the newer Optiboot bootloader need "ATmega328P" instead.
 *
 * Wiring
 *   Sensor +5 V (supply) -> Nano 5V
 *   Sensor GND           -> Nano GND
 *   Sensor OUT (signal)  -> Nano A6
 *
 * Usage
 *   1. Power up with the vacuum OFF - the atmospheric zero point is
 *      captured automatically at startup.
 *   2. Open Tools > Serial Plotter (or Serial Monitor) at 115200 baud.
 *   3. Send 'z' at any time (vacuum OFF) to re-capture the zero point.
 *
 * Limitations
 *   The sensor is specified for 0-8 bar gauge. Readings below atmosphere
 *   are outside its calibrated range and the output saturates at deep
 *   vacuum. Suitable for proof of concept only.
 */

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
namespace Config {

// Hardware
constexpr uint8_t  kSensorPin   = A6;
constexpr uint32_t kBaudRate    = 115200;

// Acquisition
constexpr uint8_t  kOversampleCount = 64;    // ADC samples averaged per reading
constexpr uint16_t kOutputPeriodMs  = 50;    // output rate: 20 Hz
constexpr float    kFilterAlpha     = 0.2f;  // EMA smoothing, (0..1], lower = smoother

// Zero (atmosphere) calibration
constexpr bool     kAutoZeroOnStartup = true;
constexpr uint8_t  kZeroCalReadings   = 20;     // readings averaged for zero capture
constexpr float    kMaxZeroDeviation  = 30.0f;  // [counts] reject zero if further from nominal

// Sensor transfer function (ratiometric to the 5 V supply)
constexpr float kAdcFullScale    = 1024.0f;     // 10-bit ADC
constexpr float kSensorRangeBar  = 8.0f;        // sensor full scale [bar]
constexpr float kOutputZeroRatio = 0.1f;        // 0.5 V / 5 V
constexpr float kOutputSpanRatio = 0.8f;        // (4.5 V - 0.5 V) / 5 V

constexpr float kCountsPerBar      = kAdcFullScale * kOutputSpanRatio / kSensorRangeBar;  // 102.4
constexpr float kNominalZeroCounts = kAdcFullScale * kOutputZeroRatio;                    // 102.4

// Pressure scale
constexpr float kAtmosphereBar = 1.0f;
constexpr float kVacuumBar     = 0.0f;

// Diagnostics
constexpr float kSaturationCounts = 20.0f;      // below this the output is assumed clipped

}  // namespace Config

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static float    g_zeroCounts     = Config::kNominalZeroCounts;
static float    g_filteredBar    = Config::kAtmosphereBar;
static bool     g_filterPrimed   = false;
static bool     g_saturated      = false;
static uint32_t g_lastOutputMs   = 0;

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

/** Returns the oversampled ADC reading in counts (0..1023, fractional). */
static float readAveragedCounts() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < Config::kOversampleCount; ++i) {
    sum += analogRead(Config::kSensorPin);
  }
  return static_cast<float>(sum) / Config::kOversampleCount;
}

/** Converts ADC counts to pressure [bar], atmosphere = 1.000 bar. */
static float countsToBar(float counts) {
  return Config::kAtmosphereBar + (counts - g_zeroCounts) / Config::kCountsPerBar;
}

/** Exponential moving average; primes itself with the first sample. */
static float applyFilter(float sampleBar) {
  if (!g_filterPrimed) {
    g_filteredBar  = sampleBar;
    g_filterPrimed = true;
  } else {
    g_filteredBar += Config::kFilterAlpha * (sampleBar - g_filteredBar);
  }
  return g_filteredBar;
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

/**
 * Captures the sensor output at atmospheric pressure as the zero point.
 * Rejects the result if it deviates too far from the nominal 0.5 V,
 * which usually means the vacuum was on during calibration.
 *
 * @return true if the new zero point was accepted.
 */
static bool captureZero() {
  float sum = 0.0f;
  for (uint8_t i = 0; i < Config::kZeroCalReadings; ++i) {
    sum += readAveragedCounts();
  }
  const float zero = sum / Config::kZeroCalReadings;

  // Status lines start with '#' and contain no ':' or numbers,
  // so the Serial Plotter does not interpret them as data.
  if (fabs(zero - Config::kNominalZeroCounts) > Config::kMaxZeroDeviation) {
    Serial.println(F("# Zero rejected - check that the vacuum is OFF"));
    return false;
  }

  g_zeroCounts   = zero;
  g_filterPrimed = false;  // restart the filter from the new reference
  Serial.println(F("# Zero captured - atmosphere set to one bar"));
  return true;
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

/** Handles single-character commands from the serial port. */
static void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char cmd = static_cast<char>(Serial.read());
    if (cmd == 'z' || cmd == 'Z') {
      captureZero();
    }
  }
}

/** Reports transitions into and out of output saturation. */
static void updateSaturationStatus(float counts) {
  const bool saturated = counts < Config::kSaturationCounts;
  if (saturated != g_saturated) {
    g_saturated = saturated;
    Serial.println(saturated ? F("# Sensor output saturated - vacuum beyond measurable range")
                             : F("# Sensor output back in range"));
  }
}

/**
 * Prints one Serial Plotter line: comma-separated label:value pairs.
 * The constant Atmosphere and Vacuum traces act as reference lines
 * and keep the plot's vertical axis stable.
 */
static void printPlotterLine(float rawBar, float filteredBar) {
  Serial.print(F("Pressure_bar:"));
  Serial.print(filteredBar, 3);
  Serial.print(F(",Unfiltered_bar:"));
  Serial.print(rawBar, 3);
  Serial.print(F(",Atmosphere:"));
  Serial.print(Config::kAtmosphereBar, 1);
  Serial.print(F(",Vacuum:"));
  Serial.println(Config::kVacuumBar, 1);
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(Config::kBaudRate);
  analogReference(DEFAULT);                 // AVcc (5 V): ratiometric with the sensor
  analogRead(Config::kSensorPin);           // discard first conversion after reset
  delay(200);                               // let the sensor output settle

  Serial.println(F("# Vacuum jig monitor started"));
  if (Config::kAutoZeroOnStartup) {
    captureZero();
  }
}

void loop() {
  handleSerialCommands();

  const uint32_t now = millis();
  if (now - g_lastOutputMs < Config::kOutputPeriodMs) {
    return;
  }
  g_lastOutputMs = now;

  const float counts      = readAveragedCounts();
  const float rawBar      = countsToBar(counts);
  const float filteredBar = applyFilter(rawBar);

  updateSaturationStatus(counts);
  printPlotterLine(rawBar, filteredBar);
}
