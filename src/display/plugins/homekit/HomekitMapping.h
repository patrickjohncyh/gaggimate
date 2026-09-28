#ifndef HOMEKITMAPPING_H
#define HOMEKITMAPPING_H

// Pure translation between GaggiMate machine state and HomeKit characteristic values.
// No HomeSpan/Arduino dependencies so it can be unit tested natively (test/test_homekit_mapping).

#include <cstdint>
#include <string>

namespace homekit {

// Mirrors MODE_* in display/core/constants.h; duplicated so this header stays dependency-free.
constexpr int MACHINE_MODE_STANDBY = 0;
constexpr int MACHINE_MODE_BREW = 1;
constexpr int MACHINE_MODE_STEAM = 2;
constexpr int MACHINE_MODE_WATER = 3;
constexpr int MACHINE_MODE_GRIND = 4;

// HAP TargetHeatingCoolingState / CurrentHeatingCoolingState values.
constexpr uint8_t HEATING_COOLING_OFF = 0;
constexpr uint8_t HEATING_COOLING_HEAT = 1;
constexpr uint8_t HEATING_COOLING_COOL = 2;
constexpr uint8_t HEATING_COOLING_AUTO = 3;

// Range advertised for CurrentTemperature/TargetTemperature. Values outside it make iOS mark the
// accessory "Not Responding", so everything published is clamped to it.
constexpr float TEMPERATURE_MIN = 0.0f;
constexpr float TEMPERATURE_MAX = 160.0f;
// HomeKit displays temperatures with 0.1 precision; smaller changes are not worth a notification.
constexpr float TEMPERATURE_NOTIFY_THRESHOLD = 0.1f;

enum class PowerRequest : uint8_t { None, On, Off };

/**
 * Accumulated writes from the Home app that the main task has not applied yet.
 *
 * Several writes can arrive in one HAP request (e.g. "Hey Siri, turn on <accessory>" writes the
 * thermostat and every switch at once), so requests are merged here and resolved together by
 * resolveRequestedMode().
 *
 * Example:
 *   HomekitCommand command;
 *   command.requestPower(isPowerOnRequest(HEATING_COOLING_AUTO)); // Siri "turn on"
 *   command.requestModeSwitch(MACHINE_MODE_STEAM, true);
 */
struct HomekitCommand {
    PowerRequest power = PowerRequest::None;
    bool hasTargetTemperature = false;
    float targetTemperature = 0.0f;
    uint8_t modesSwitchedOn = 0;  // bitmask of (1 << MACHINE_MODE_*)
    uint8_t modesSwitchedOff = 0; // bitmask of (1 << MACHINE_MODE_*)

    bool isEmpty() const {
        return power == PowerRequest::None && !hasTargetTemperature && modesSwitchedOn == 0 && modesSwitchedOff == 0;
    }

    // Folds a later batch of writes into this one; later writes to the same target win.
    void merge(const HomekitCommand &later) {
        if (later.power != PowerRequest::None)
            power = later.power;
        if (later.hasTargetTemperature)
            requestTargetTemperature(later.targetTemperature);
        for (int mode = MACHINE_MODE_STANDBY; mode <= MACHINE_MODE_GRIND; mode++) {
            const uint8_t bit = static_cast<uint8_t>(1u << mode);
            if (later.modesSwitchedOn & bit)
                requestModeSwitch(mode, true);
            if (later.modesSwitchedOff & bit)
                requestModeSwitch(mode, false);
        }
    }

    void requestPower(bool isOn) { power = isOn ? PowerRequest::On : PowerRequest::Off; }

    void requestTargetTemperature(float temperature) {
        hasTargetTemperature = true;
        targetTemperature = temperature;
    }

    void requestModeSwitch(int mode, bool isOn) {
        const uint8_t bit = static_cast<uint8_t>(1u << mode);
        if (isOn) {
            modesSwitchedOn |= bit;
            modesSwitchedOff &= static_cast<uint8_t>(~bit);
        } else {
            modesSwitchedOff |= bit;
            modesSwitchedOn &= static_cast<uint8_t>(~bit);
        }
    }
};

/**
 * Characteristic values HomeKit should show for a given machine state.
 *
 * Example:
 *   HomekitState state = stateForMode(MACHINE_MODE_STEAM);
 *   // state.targetHeatingCooling == HEATING_COOLING_HEAT, state.isSteamOn == true
 */
struct HomekitState {
    uint8_t targetHeatingCooling;
    uint8_t currentHeatingCooling;
    bool isBrewOn;
    bool isSteamOn;
    bool isWaterOn;
};

/**
 * Whether a written TargetHeatingCoolingState means "turn the machine on".
 *
 * Siri's "turn on" writes Auto, and HomeSpan 1.9.1 does not enforce setValidValues() on writes,
 * so every non-Off value (Heat, Cool, Auto) must be treated as on.
 *
 * @param targetHeatingCooling value written by the Home app
 * @return true for any value other than Off
 *
 * Example: isPowerOnRequest(HEATING_COOLING_AUTO) == true
 */
bool isPowerOnRequest(int targetHeatingCooling);

/**
 * Resolves pending Home app writes into the machine mode to switch to.
 *
 * Precedence: power off > a mode switched on > power on > the active mode switched off.
 * When several mode switches are turned on at once, brew wins over water over steam (the
 * mode with the lowest boiler temperature is the safest guess).
 *
 * @param command pending writes
 * @param currentMode the machine's current MODE_* value
 * @return the MODE_* value to switch to, or currentMode when nothing should change
 *
 * Example:
 *   HomekitCommand command;
 *   command.requestPower(true);
 *   resolveRequestedMode(command, MACHINE_MODE_STANDBY) == MACHINE_MODE_BREW
 */
int resolveRequestedMode(const HomekitCommand &command, int currentMode);

/**
 * Controller calls needed to move from one machine mode to another, mirroring the display UI:
 * standby is entered with activateStandby(), waking goes through deactivateStandby() (which lands
 * in brew), and any other awake mode is then selected with setMode().
 *
 * Example:
 *   ModeTransition transition = planModeTransition(MACHINE_MODE_STEAM, MACHINE_MODE_BREW);
 *   // transition.shouldSetMode == true, transition.shouldWake == false
 */
struct ModeTransition {
    bool shouldEnterStandby = false;
    bool shouldWake = false;
    bool shouldSetMode = false;
};

/**
 * Plans the Controller calls for a mode change requested from HomeKit.
 *
 * @param currentMode the machine's current MODE_* value
 * @param requestedMode the MODE_* value to switch to
 * @return which of activateStandby(), deactivateStandby() and setMode(requestedMode) to call,
 *         in that order; all false when the modes are equal
 *
 * Example: planModeTransition(MACHINE_MODE_STANDBY, MACHINE_MODE_STEAM) wakes, then sets steam.
 */
ModeTransition planModeTransition(int currentMode, int requestedMode);

/**
 * Characteristic values that reflect the given machine mode. Grind mode shows the thermostat as
 * on with no mode switch active, since grinding is not controllable from HomeKit.
 *
 * @param mode MODE_* value
 * @return values to publish to the thermostat and the Brew/Steam/Hot Water switches
 *
 * Example: stateForMode(MACHINE_MODE_STANDBY).targetHeatingCooling == HEATING_COOLING_OFF
 */
HomekitState stateForMode(int mode);

// Room temperature assumed when the first boiler reading after boot is not a plausible room
// temperature (e.g. the display restarted while the machine was hot).
constexpr float FALLBACK_ROOM_TEMPERATURE = 28.0f;
constexpr float PLAUSIBLE_ROOM_TEMPERATURE_MIN = 10.0f;
constexpr float PLAUSIBLE_ROOM_TEMPERATURE_MAX = 35.0f;
// Soak level treated as "heat-soaked": 1 - e^-3, i.e. three time constants from cold.
constexpr float HEAT_SOAKED_LEVEL = 0.95f;
// The warm-up time setting covers three time constants (95 % soaked from cold).
constexpr float WARMUP_TIME_CONSTANTS = 3.0f;

/**
 * Estimates how heat-soaked the group and portafilter are, from the boiler temperature alone.
 *
 * The portafilter is a ~0.45 kg brass mass heated through the group lugs, so it follows the
 * boiler as a first-order lag: d(soak)/dt = (drive - soak) / tau, where drive is the boiler
 * temperature normalised between room temperature (0) and the reference temperature (1). A cold
 * start takes three time constants to reach HEAT_SOAKED_LEVEL; a short standby only loses as much
 * soak as the boiler actually cooled, so re-warming is correspondingly shorter. Heating and
 * cooling share one time constant (the real portafilter cools a little slower, so this errs
 * towards reporting ready late, never early). Starts unsoaked at boot.
 *
 * Example:
 *   HeatSoakEstimator estimator;
 *   estimator.update(93.0f, 93.0f, 300.0f, 900.0f); // 15 min at temperature, tau = 5 min
 *   estimator.getSoakLevel() >= HEAT_SOAKED_LEVEL
 */
class HeatSoakEstimator {
  public:
    /**
     * Advances the model by elapsedSeconds with the boiler held at boilerTemperature.
     *
     * @param boilerTemperature current boiler reading; readings <= 0 (no sensor data yet) are ignored
     * @param referenceTemperature temperature that counts as fully hot (the brew setpoint)
     * @param timeConstantSeconds tau of the portafilter lag
     * @param elapsedSeconds time since the previous update
     * @return the updated soak level, 0 (room temperature) .. 1 (fully soaked)
     */
    float update(float boilerTemperature, float referenceTemperature, float timeConstantSeconds, float elapsedSeconds);
    float getSoakLevel() const { return soakLevel; }
    float getRoomTemperature() const { return roomTemperature; }

  private:
    bool hasRoomTemperature = false;
    float roomTemperature = FALLBACK_ROOM_TEMPERATURE;
    float soakLevel = 0.0f;
};

/**
 * Latched "machine is ready" state for the Espresso Ready sensor.
 *
 * Becomes ready once the machine is awake, the display reports the boiler temperature as stable,
 * and the group/portafilter are heat-soaked. Stays ready through temperature dips (e.g. during a
 * shot) so iOS doesn't notify repeatedly; resets when the mode or the target temperature changes,
 * so switching to steam notifies again once steam is at temperature.
 *
 * Example:
 *   ReadinessTracker tracker;
 *   tracker.update(MACHINE_MODE_BREW, 93.0f, true, true) == true
 */
class ReadinessTracker {
  public:
    /**
     * @param mode current MODE_* value
     * @param targetTemperature current boiler setpoint
     * @param isTemperatureStable WarningManager::isTemperatureStable()
     * @param isHeatSoaked whether HeatSoakEstimator reports the group/portafilter as soaked
     * @return whether the machine is ready
     */
    bool update(int mode, float targetTemperature, bool isTemperatureStable, bool isHeatSoaked);

  private:
    int lastMode = MACHINE_MODE_STANDBY;
    float lastTargetTemperature = 0.0f;
    bool isReady = false;
};

/**
 * Clamps a temperature into the range advertised to HomeKit.
 *
 * Example: clampTemperature(-3.0f) == TEMPERATURE_MIN
 */
float clampTemperature(float temperature);

/**
 * Whether a temperature moved enough since it was last published to be worth notifying.
 *
 * Example: shouldPublishTemperature(93.0f, 93.04f) == false
 */
bool shouldPublishTemperature(float publishedTemperature, float temperature);

/**
 * Converts a `git describe` firmware version into a HAP FirmwareRevision.
 *
 * HAP requires FirmwareRevision to be "x[.y[.z]]" with numeric parts only; suffixes like
 * "-210-g6dff0448-dirty" are rejected by iOS.
 *
 * @param version firmware version string, e.g. BUILD_GIT_VERSION
 * @return the leading numeric "x.y.z" part, or "0.0.0" when none is found
 *
 * Example: firmwareRevisionFromVersion("v1.8.1-210-g6dff0448") == "1.8.1"
 */
std::string firmwareRevisionFromVersion(const char *version);

} // namespace homekit

#endif // HOMEKITMAPPING_H
