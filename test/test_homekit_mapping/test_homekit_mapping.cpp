// Unit tests: HomeKit <-> machine state mapping (Siri "turn on" writes Auto, mutually exclusive
// Brew/Steam/Hot Water switches). Host-side, no HomeSpan — pio test -e native -f test_homekit_mapping.

#include <unity.h>

#include <cmath>

#include "display/core/constants.h"
#include "display/plugins/homekit/HomekitMapping.cpp"

using namespace homekit;

void setUp() {}
void tearDown() {}

static void test_mode_constants_match_controller() {
    TEST_ASSERT_EQUAL(MODE_STANDBY, MACHINE_MODE_STANDBY);
    TEST_ASSERT_EQUAL(MODE_BREW, MACHINE_MODE_BREW);
    TEST_ASSERT_EQUAL(MODE_STEAM, MACHINE_MODE_STEAM);
    TEST_ASSERT_EQUAL(MODE_WATER, MACHINE_MODE_WATER);
    TEST_ASSERT_EQUAL(MODE_GRIND, MACHINE_MODE_GRIND);
}

static void test_every_non_off_heating_cooling_state_is_power_on() {
    TEST_ASSERT_FALSE(isPowerOnRequest(HEATING_COOLING_OFF));
    TEST_ASSERT_TRUE(isPowerOnRequest(HEATING_COOLING_HEAT));
    TEST_ASSERT_TRUE(isPowerOnRequest(HEATING_COOLING_COOL));
    TEST_ASSERT_TRUE(isPowerOnRequest(HEATING_COOLING_AUTO));
}

static void test_siri_turn_on_auto_wakes_to_brew() {
    HomekitCommand command;
    command.requestPower(isPowerOnRequest(HEATING_COOLING_AUTO));
    TEST_ASSERT_EQUAL(MACHINE_MODE_BREW, resolveRequestedMode(command, MACHINE_MODE_STANDBY));
}

static void test_power_on_keeps_current_awake_mode() {
    HomekitCommand command;
    command.requestPower(true);
    TEST_ASSERT_EQUAL(MACHINE_MODE_STEAM, resolveRequestedMode(command, MACHINE_MODE_STEAM));
}

static void test_power_off_enters_standby_and_wins_over_switches() {
    HomekitCommand command;
    command.requestModeSwitch(MACHINE_MODE_STEAM, true);
    command.requestPower(false);
    TEST_ASSERT_EQUAL(MACHINE_MODE_STANDBY, resolveRequestedMode(command, MACHINE_MODE_BREW));
}

static void test_mode_switch_on_selects_mode_from_standby() {
    HomekitCommand command;
    command.requestModeSwitch(MACHINE_MODE_WATER, true);
    TEST_ASSERT_EQUAL(MACHINE_MODE_WATER, resolveRequestedMode(command, MACHINE_MODE_STANDBY));
}

static void test_siri_turn_on_everything_prefers_brew() {
    // "Turn on <accessory>" writes the thermostat and all three switches in one request.
    HomekitCommand command;
    command.requestPower(true);
    command.requestModeSwitch(MACHINE_MODE_BREW, true);
    command.requestModeSwitch(MACHINE_MODE_STEAM, true);
    command.requestModeSwitch(MACHINE_MODE_WATER, true);
    TEST_ASSERT_EQUAL(MACHINE_MODE_BREW, resolveRequestedMode(command, MACHINE_MODE_STANDBY));
}

static void test_water_wins_over_steam_when_both_switched_on() {
    HomekitCommand command;
    command.requestModeSwitch(MACHINE_MODE_STEAM, true);
    command.requestModeSwitch(MACHINE_MODE_WATER, true);
    TEST_ASSERT_EQUAL(MACHINE_MODE_WATER, resolveRequestedMode(command, MACHINE_MODE_BREW));
}

static void test_switching_off_active_mode_enters_standby() {
    HomekitCommand command;
    command.requestModeSwitch(MACHINE_MODE_STEAM, false);
    TEST_ASSERT_EQUAL(MACHINE_MODE_STANDBY, resolveRequestedMode(command, MACHINE_MODE_STEAM));
}

static void test_switching_off_inactive_mode_is_ignored() {
    HomekitCommand command;
    command.requestModeSwitch(MACHINE_MODE_STEAM, false);
    TEST_ASSERT_EQUAL(MACHINE_MODE_BREW, resolveRequestedMode(command, MACHINE_MODE_BREW));
}

static void test_latest_write_for_a_switch_wins() {
    HomekitCommand command;
    command.requestModeSwitch(MACHINE_MODE_STEAM, true);
    command.requestModeSwitch(MACHINE_MODE_STEAM, false);
    TEST_ASSERT_EQUAL(0, command.modesSwitchedOn);
    TEST_ASSERT_EQUAL(MACHINE_MODE_STANDBY, resolveRequestedMode(command, MACHINE_MODE_STEAM));
}

static void test_temperature_only_command_keeps_mode() {
    HomekitCommand command;
    command.requestTargetTemperature(94.0f);
    TEST_ASSERT_FALSE(command.isEmpty());
    TEST_ASSERT_EQUAL(MACHINE_MODE_BREW, resolveRequestedMode(command, MACHINE_MODE_BREW));
}

static void assert_transition(int currentMode, int requestedMode, bool shouldEnterStandby, bool shouldWake,
                              bool shouldSetMode) {
    const ModeTransition transition = planModeTransition(currentMode, requestedMode);
    TEST_ASSERT_EQUAL(shouldEnterStandby, transition.shouldEnterStandby);
    TEST_ASSERT_EQUAL(shouldWake, transition.shouldWake);
    TEST_ASSERT_EQUAL(shouldSetMode, transition.shouldSetMode);
}

static void test_transition_between_awake_modes_sets_mode() {
    // Regression: Steam/Hot Water -> Brew used to do nothing because brew was only reached by waking.
    assert_transition(MACHINE_MODE_STEAM, MACHINE_MODE_BREW, false, false, true);
    assert_transition(MACHINE_MODE_WATER, MACHINE_MODE_BREW, false, false, true);
    assert_transition(MACHINE_MODE_BREW, MACHINE_MODE_STEAM, false, false, true);
    assert_transition(MACHINE_MODE_STEAM, MACHINE_MODE_WATER, false, false, true);
    assert_transition(MACHINE_MODE_GRIND, MACHINE_MODE_BREW, false, false, true);
}

static void test_transition_from_standby_wakes() {
    assert_transition(MACHINE_MODE_STANDBY, MACHINE_MODE_BREW, false, true, false);
    assert_transition(MACHINE_MODE_STANDBY, MACHINE_MODE_STEAM, false, true, true);
    assert_transition(MACHINE_MODE_STANDBY, MACHINE_MODE_WATER, false, true, true);
}

static void test_transition_to_standby_and_no_change() {
    assert_transition(MACHINE_MODE_STEAM, MACHINE_MODE_STANDBY, true, false, false);
    assert_transition(MACHINE_MODE_BREW, MACHINE_MODE_BREW, false, false, false);
    assert_transition(MACHINE_MODE_STANDBY, MACHINE_MODE_STANDBY, false, false, false);
}

static void test_state_for_each_mode() {
    const HomekitState standby = stateForMode(MACHINE_MODE_STANDBY);
    TEST_ASSERT_EQUAL(HEATING_COOLING_OFF, standby.targetHeatingCooling);
    TEST_ASSERT_EQUAL(HEATING_COOLING_OFF, standby.currentHeatingCooling);
    TEST_ASSERT_FALSE(standby.isBrewOn || standby.isSteamOn || standby.isWaterOn);

    const HomekitState steam = stateForMode(MACHINE_MODE_STEAM);
    TEST_ASSERT_EQUAL(HEATING_COOLING_HEAT, steam.targetHeatingCooling);
    TEST_ASSERT_EQUAL(HEATING_COOLING_HEAT, steam.currentHeatingCooling);
    TEST_ASSERT_TRUE(steam.isSteamOn);
    TEST_ASSERT_FALSE(steam.isBrewOn || steam.isWaterOn);

    const HomekitState grind = stateForMode(MACHINE_MODE_GRIND);
    TEST_ASSERT_EQUAL(HEATING_COOLING_HEAT, grind.targetHeatingCooling);
    TEST_ASSERT_FALSE(grind.isBrewOn || grind.isSteamOn || grind.isWaterOn);
}

static void test_published_heating_cooling_state_never_auto() {
    // CurrentHeatingCoolingState only allows 0..2; publishing Auto (3) caused "Not Responding".
    for (int mode = MACHINE_MODE_STANDBY; mode <= MACHINE_MODE_GRIND; mode++) {
        const HomekitState state = stateForMode(mode);
        TEST_ASSERT_TRUE(state.currentHeatingCooling <= HEATING_COOLING_HEAT);
        TEST_ASSERT_TRUE(state.targetHeatingCooling <= HEATING_COOLING_HEAT);
    }
}

static void test_clamp_temperature() {
    TEST_ASSERT_EQUAL_FLOAT(TEMPERATURE_MIN, clampTemperature(-3.0f));
    TEST_ASSERT_EQUAL_FLOAT(TEMPERATURE_MAX, clampTemperature(400.0f));
    TEST_ASSERT_EQUAL_FLOAT(TEMPERATURE_MIN, clampTemperature(NAN));
    TEST_ASSERT_EQUAL_FLOAT(93.5f, clampTemperature(93.5f));
}

static void test_should_publish_temperature_threshold() {
    TEST_ASSERT_FALSE(shouldPublishTemperature(93.0f, 93.04f));
    TEST_ASSERT_TRUE(shouldPublishTemperature(93.0f, 93.2f));
    TEST_ASSERT_TRUE(shouldPublishTemperature(93.0f, 92.8f));
    TEST_ASSERT_FALSE(shouldPublishTemperature(TEMPERATURE_MAX, 400.0f));
}

static void test_firmware_revision_from_version() {
    TEST_ASSERT_EQUAL_STRING("1.8.1", firmwareRevisionFromVersion("v1.8.1").c_str());
    TEST_ASSERT_EQUAL_STRING("1.8.1", firmwareRevisionFromVersion("v1.8.1-210-g6dff0448-dirty").c_str());
    TEST_ASSERT_EQUAL_STRING("2.0", firmwareRevisionFromVersion("2.0-rc1").c_str());
    TEST_ASSERT_EQUAL_STRING("1.2.3", firmwareRevisionFromVersion("v1.2.3.4").c_str());
    TEST_ASSERT_EQUAL_STRING("0.0.0", firmwareRevisionFromVersion("g6dff0448").c_str());
    TEST_ASSERT_EQUAL_STRING("0.0.0", firmwareRevisionFromVersion("").c_str());
    TEST_ASSERT_EQUAL_STRING("0.0.0", firmwareRevisionFromVersion(nullptr).c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_mode_constants_match_controller);
    RUN_TEST(test_every_non_off_heating_cooling_state_is_power_on);
    RUN_TEST(test_siri_turn_on_auto_wakes_to_brew);
    RUN_TEST(test_power_on_keeps_current_awake_mode);
    RUN_TEST(test_power_off_enters_standby_and_wins_over_switches);
    RUN_TEST(test_mode_switch_on_selects_mode_from_standby);
    RUN_TEST(test_siri_turn_on_everything_prefers_brew);
    RUN_TEST(test_water_wins_over_steam_when_both_switched_on);
    RUN_TEST(test_switching_off_active_mode_enters_standby);
    RUN_TEST(test_switching_off_inactive_mode_is_ignored);
    RUN_TEST(test_latest_write_for_a_switch_wins);
    RUN_TEST(test_temperature_only_command_keeps_mode);
    RUN_TEST(test_transition_between_awake_modes_sets_mode);
    RUN_TEST(test_transition_from_standby_wakes);
    RUN_TEST(test_transition_to_standby_and_no_change);
    RUN_TEST(test_state_for_each_mode);
    RUN_TEST(test_published_heating_cooling_state_never_auto);
    RUN_TEST(test_clamp_temperature);
    RUN_TEST(test_should_publish_temperature_threshold);
    RUN_TEST(test_firmware_revision_from_version);
    return UNITY_END();
}
