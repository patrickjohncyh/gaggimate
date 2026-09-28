#include "HomekitPlugin.h"
#include "../core/Controller.h"
#include "../core/constants.h"
#include <WiFi.h>
#include <esp_log.h>
#include <utility>
#include <version.h>

static constexpr char LOG_TAG[] = "HomekitPlugin";

void HomekitSharedState::setMode(int mode) {
    std::lock_guard<std::mutex> guard(mutex);
    machineState.mode = mode;
}

void HomekitSharedState::setCurrentTemperature(float temperature) {
    std::lock_guard<std::mutex> guard(mutex);
    machineState.currentTemperature = temperature;
}

void HomekitSharedState::setTargetTemperature(float temperature) {
    std::lock_guard<std::mutex> guard(mutex);
    machineState.targetTemperature = temperature;
}

void HomekitSharedState::setReady(bool isReady) {
    std::lock_guard<std::mutex> guard(mutex);
    machineState.isReady = isReady;
}

HomekitSharedState::MachineState HomekitSharedState::getMachineState() const {
    std::lock_guard<std::mutex> guard(mutex);
    return machineState;
}

void HomekitSharedState::recordCommand(const std::function<void(homekit::HomekitCommand &)> &record) {
    std::lock_guard<std::mutex> guard(mutex);
    record(stagedCommand);
}

void HomekitSharedState::commitStagedCommand() {
    std::lock_guard<std::mutex> guard(mutex);
    if (stagedCommand.isEmpty())
        return;
    pendingCommand.merge(stagedCommand);
    stagedCommand = homekit::HomekitCommand{};
}

bool HomekitSharedState::beginApplyingCommand(homekit::HomekitCommand &command) {
    std::lock_guard<std::mutex> guard(mutex);
    if (pendingCommand.isEmpty())
        return false;
    command = pendingCommand;
    pendingCommand = homekit::HomekitCommand{};
    isApplyingCommand = true;
    return true;
}

void HomekitSharedState::finishApplyingCommand() {
    std::lock_guard<std::mutex> guard(mutex);
    isApplyingCommand = false;
}

bool HomekitSharedState::hasUnappliedCommand() const {
    std::lock_guard<std::mutex> guard(mutex);
    return isApplyingCommand || !pendingCommand.isEmpty() || !stagedCommand.isEmpty();
}

HomekitThermostat::HomekitThermostat(HomekitSharedState &sharedState) : sharedState(sharedState) {
    currentHeatingCooling = new Characteristic::CurrentHeatingCoolingState();
    currentHeatingCooling->setValidValues(2, homekit::HEATING_COOLING_OFF, homekit::HEATING_COOLING_HEAT);
    // Only advertises Off/Heat to the Home app; Siri still writes Auto, see isPowerOnRequest().
    targetHeatingCooling = new Characteristic::TargetHeatingCoolingState();
    targetHeatingCooling->setValidValues(2, homekit::HEATING_COOLING_OFF, homekit::HEATING_COOLING_HEAT);
    currentTemperature = new Characteristic::CurrentTemperature();
    currentTemperature->setRange(homekit::TEMPERATURE_MIN, homekit::TEMPERATURE_MAX);
    targetTemperature = new Characteristic::TargetTemperature();
    targetTemperature->setRange(homekit::TEMPERATURE_MIN, homekit::TEMPERATURE_MAX);
    displayUnits = new Characteristic::TemperatureDisplayUnits();
    displayUnits->setVal(0);
}

boolean HomekitThermostat::update() {
    const bool isPowerUpdated = targetHeatingCooling->updated();
    const bool isTemperatureUpdated = targetTemperature->updated();
    const int requestedHeatingCooling = targetHeatingCooling->getNewVal();
    const float requestedTemperature = targetTemperature->getNewVal<float>();
    sharedState.recordCommand([&](homekit::HomekitCommand &command) {
        if (isPowerUpdated)
            command.requestPower(homekit::isPowerOnRequest(requestedHeatingCooling));
        if (isTemperatureUpdated)
            command.requestTargetTemperature(requestedTemperature);
    });
    return true;
}

void HomekitThermostat::loop() {
    // First service loop after HomeSpan handled this cycle's requests (services loop in creation
    // order, the thermostat is created first); later loops find nothing left to commit.
    sharedState.commitStagedCommand();
    if (sharedState.hasUnappliedCommand())
        return;
    const HomekitSharedState::MachineState machineState = sharedState.getMachineState();
    const homekit::HomekitState state = homekit::stateForMode(machineState.mode);

    // Also snaps Auto (written by Siri) back to Heat once the power request has been applied.
    if (targetHeatingCooling->getVal() != state.targetHeatingCooling)
        targetHeatingCooling->setVal(state.targetHeatingCooling);
    if (currentHeatingCooling->getVal() != state.currentHeatingCooling)
        currentHeatingCooling->setVal(state.currentHeatingCooling);
    if (homekit::shouldPublishTemperature(currentTemperature->getVal<float>(), machineState.currentTemperature))
        currentTemperature->setVal(homekit::clampTemperature(machineState.currentTemperature));
    if (homekit::shouldPublishTemperature(targetTemperature->getVal<float>(), machineState.targetTemperature))
        targetTemperature->setVal(homekit::clampTemperature(machineState.targetTemperature));
}

HomekitModeSwitch::HomekitModeSwitch(HomekitSharedState &sharedState, int mode, const char *name)
    : sharedState(sharedState), mode(mode) {
    new Characteristic::Name(name);
    // iOS 16+ ignores Name on secondary services and shows the accessory name instead.
    new Characteristic::ConfiguredName(name);
    isOn = new Characteristic::On();
}

boolean HomekitModeSwitch::update() {
    const bool isRequestedOn = isOn->getNewVal();
    sharedState.recordCommand([&](homekit::HomekitCommand &command) { command.requestModeSwitch(mode, isRequestedOn); });
    return true;
}

void HomekitModeSwitch::loop() {
    sharedState.commitStagedCommand();
    if (sharedState.hasUnappliedCommand())
        return;
    const bool shouldBeOn = sharedState.getMachineState().mode == mode;
    if (isOn->getVal() != shouldBeOn)
        isOn->setVal(shouldBeOn);
}

HomekitReadySensor::HomekitReadySensor(HomekitSharedState &sharedState) : sharedState(sharedState) {
    new Characteristic::Name("Espresso Ready");
    new Characteristic::ConfiguredName("Espresso Ready");
    isDetected = new Characteristic::OccupancyDetected();
}

void HomekitReadySensor::loop() {
    const uint8_t shouldBeDetected = sharedState.getMachineState().isReady ? 1 : 0;
    if (isDetected->getVal() != shouldBeDetected)
        isDetected->setVal(shouldBeDetected);
}

HomekitPlugin::HomekitPlugin(String wifiSsid, String wifiPassword) : controller(nullptr) {
    this->wifiSsid = std::move(wifiSsid);
    this->wifiPassword = std::move(wifiPassword);
}

void HomekitPlugin::setup(Controller *controller, PluginManager *pluginManager) {
    this->controller = controller;

    pluginManager->on("controller:wifi:connect", [this](Event &event) {
        if (event.getInt("AP") || isStarted)
            return;
        startHomeSpan();
    });

    pluginManager->on("boiler:targetTemperature:change",
                      [this](Event const &event) { sharedState.setTargetTemperature(event.getFloat("value")); });

    pluginManager->on("boiler:currentTemperature:change",
                      [this](Event const &event) { sharedState.setCurrentTemperature(event.getFloat("value")); });

    pluginManager->on("controller:mode:change", [this](Event const &event) { sharedState.setMode(event.getInt("value")); });
}

void HomekitPlugin::startHomeSpan() {
    homeSpan.setHostNameSuffix("");
    homeSpan.setPortNum(HOMESPAN_PORT);
    homeSpan.begin(Category::Thermostats, DEVICE_NAME, controller->getSettings().getMdnsName().c_str());
    homeSpan.setWifiCredentials(wifiSsid.c_str(), wifiPassword.c_str());

    // HomeSpan keeps pointers to these for the lifetime of the program.
    new SpanAccessory();
    new Service::AccessoryInformation();
    new Characteristic::Identify();
    new Characteristic::Name(DEVICE_NAME);
    new Characteristic::Manufacturer("GaggiMate");
    new Characteristic::Model("GaggiMate");
    new Characteristic::SerialNumber(WiFi.macAddress().c_str());
    new Characteristic::FirmwareRevision(homekit::firmwareRevisionFromVersion(BUILD_GIT_VERSION).c_str());

    // Seed from the controller so the first loop() doesn't publish defaults.
    sharedState.setMode(controller->getMode());
    sharedState.setTargetTemperature(controller->getTargetTemp());

    (new HomekitThermostat(sharedState))->setPrimary();
    new HomekitModeSwitch(sharedState, MODE_BREW, "Brew");
    new HomekitModeSwitch(sharedState, MODE_STEAM, "Steam");
    new HomekitModeSwitch(sharedState, MODE_WATER, "Hot Water");
    new HomekitReadySensor(sharedState);

    homeSpan.autoPoll();
    isStarted = true;
}

void HomekitPlugin::loop() {
    if (controller == nullptr)
        return;
    publishStatus();

    homekit::HomekitCommand command;
    if (!sharedState.beginApplyingCommand(command))
        return;
    applyCommand(command);
    // Publish what the controller actually did (it may have refused or altered the request)
    // before the poll task is allowed to reconcile characteristics again.
    sharedState.setMode(controller->getMode());
    sharedState.setTargetTemperature(controller->getTargetTemp());
    sharedState.finishApplyingCommand();
}

void HomekitPlugin::publishStatus() {
    const int mode = controller->getMode();
    const float targetTemperature = controller->getTargetTemp();
    if (mode == MODE_BREW && targetTemperature > 0.0f)
        brewReferenceTemperature = targetTemperature;

    const unsigned long now = millis();
    const unsigned long elapsedMillis = now - lastSoakUpdateMillis;
    if (lastSoakUpdateMillis == 0 || elapsedMillis >= SOAK_UPDATE_INTERVAL_MS) {
        const float timeConstantSeconds =
            static_cast<float>(controller->getSettings().getWarmupMinutes()) * 60.0f / homekit::WARMUP_TIME_CONSTANTS;
        const float elapsedSeconds = lastSoakUpdateMillis == 0 ? 0.0f : static_cast<float>(elapsedMillis) / 1000.0f;
        heatSoakEstimator.update(controller->getCurrentTemp(), brewReferenceTemperature, timeConstantSeconds, elapsedSeconds);
        lastSoakUpdateMillis = now;
        ESP_LOGD(LOG_TAG, "Heat soak %.3f (room %.1f, reference %.1f)", heatSoakEstimator.getSoakLevel(),
                 heatSoakEstimator.getRoomTemperature(), brewReferenceTemperature);
    }

    const bool isHeatSoaked = controller->getSettings().getWarmupMinutes() == 0 ||
                              heatSoakEstimator.getSoakLevel() >= homekit::HEAT_SOAKED_LEVEL;
    sharedState.setReady(readinessTracker.update(mode, targetTemperature, controller->getWarnings().isTemperatureStable(),
                                                 isHeatSoaked));
}

void HomekitPlugin::applyCommand(const homekit::HomekitCommand &command) {
    // Never interrupt or retarget a running brew/steam/water process from a phone.
    if (controller->isActive()) {
        ESP_LOGW(LOG_TAG, "Ignoring HomeKit command while a process is running (mode=%d)", controller->getMode());
        return;
    }

    const int currentMode = controller->getMode();
    const int requestedMode = homekit::resolveRequestedMode(command, currentMode);

    if (requestedMode != currentMode) {
        ESP_LOGI(LOG_TAG, "Changing mode from %d to %d", currentMode, requestedMode);
        const homekit::ModeTransition transition = homekit::planModeTransition(currentMode, requestedMode);
        if (transition.shouldEnterStandby)
            controller->activateStandby();
        if (transition.shouldWake)
            controller->deactivateStandby();
        if (transition.shouldSetMode)
            controller->setMode(requestedMode);
    }

    if (command.hasTargetTemperature)
        controller->setTargetTemp(command.targetTemperature);
}
