#include "HomekitMapping.h"

#include <cctype>
#include <cmath>

namespace homekit {

namespace {

// Brew first: the mode with the lowest boiler temperature is the safest pick when several are requested.
constexpr int MODE_SWITCH_PRIORITY[] = {MACHINE_MODE_BREW, MACHINE_MODE_WATER, MACHINE_MODE_STEAM};

bool isModeRequested(uint8_t modes, int mode) { return (modes & (1u << mode)) != 0; }

} // namespace

bool isPowerOnRequest(int targetHeatingCooling) { return targetHeatingCooling != HEATING_COOLING_OFF; }

int resolveRequestedMode(const HomekitCommand &command, int currentMode) {
    if (command.power == PowerRequest::Off)
        return MACHINE_MODE_STANDBY;

    for (int mode : MODE_SWITCH_PRIORITY) {
        if (isModeRequested(command.modesSwitchedOn, mode))
            return mode;
    }

    if (command.power == PowerRequest::On)
        return currentMode == MACHINE_MODE_STANDBY ? MACHINE_MODE_BREW : currentMode;

    // Turning off the active mode's switch means "turn the machine off"; turning off a switch that
    // is not active is a no-op (HomeKit may still send it when it turns off every switch at once).
    if (isModeRequested(command.modesSwitchedOff, currentMode))
        return MACHINE_MODE_STANDBY;

    return currentMode;
}

HomekitState stateForMode(int mode) {
    const bool isAwake = mode != MACHINE_MODE_STANDBY;
    const uint8_t heatingCooling = isAwake ? HEATING_COOLING_HEAT : HEATING_COOLING_OFF;
    return HomekitState{
        heatingCooling,
        heatingCooling,
        mode == MACHINE_MODE_BREW,
        mode == MACHINE_MODE_STEAM,
        mode == MACHINE_MODE_WATER,
    };
}

float clampTemperature(float temperature) {
    if (std::isnan(temperature) || temperature < TEMPERATURE_MIN)
        return TEMPERATURE_MIN;
    if (temperature > TEMPERATURE_MAX)
        return TEMPERATURE_MAX;
    return temperature;
}

bool shouldPublishTemperature(float publishedTemperature, float temperature) {
    return std::fabs(clampTemperature(temperature) - publishedTemperature) >= TEMPERATURE_NOTIFY_THRESHOLD;
}

std::string firmwareRevisionFromVersion(const char *version) {
    constexpr int MAX_PARTS = 3;
    std::string revision;
    if (version == nullptr)
        return "0.0.0";

    const char *cursor = version;
    if (*cursor == 'v' || *cursor == 'V')
        cursor++;

    int parts = 0;
    while (parts < MAX_PARTS && std::isdigit(static_cast<unsigned char>(*cursor))) {
        if (parts > 0)
            revision += '.';
        while (std::isdigit(static_cast<unsigned char>(*cursor)))
            revision += *cursor++;
        parts++;
        if (*cursor != '.')
            break;
        cursor++;
    }
    return parts == 0 ? "0.0.0" : revision;
}

} // namespace homekit
