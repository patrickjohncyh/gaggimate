#ifndef HOMEKITPLUGIN_H
#define HOMEKITPLUGIN_H
#include "../core/Plugin.h"
#include "HomeSpan.h"
#include "homekit/HomekitMapping.h"

#include <mutex>

#define HOMESPAN_PORT 8080
#define DEVICE_NAME "GaggiMate"

/**
 * State shared between the main task (Controller, plugin events) and HomeSpan's poll task.
 *
 * HomeSpan 1.9.1 is not thread-safe: SpanCharacteristic::setVal() appends to an unguarded
 * notification vector that the poll task drains. So only the poll task touches characteristics
 * and only the main task touches the Controller; the two sides exchange plain values through here.
 */
class HomekitSharedState {
  public:
    struct MachineState {
        int mode = homekit::MACHINE_MODE_STANDBY;
        float currentTemperature = 0.0f;
        float targetTemperature = 0.0f;
    };

    // Main task -> poll task.
    void setMode(int mode);
    void setCurrentTemperature(float temperature);
    void setTargetTemperature(float temperature);
    MachineState getMachineState() const;

    // Poll task -> main task.
    void recordCommand(const std::function<void(homekit::HomekitCommand &)> &record);
    bool beginApplyingCommand(homekit::HomekitCommand &command);
    void finishApplyingCommand();
    // True while Home app writes are queued or being applied; characteristics must not be
    // overwritten from MachineState then, or the user's change would flicker back.
    bool hasUnappliedCommand() const;

  private:
    mutable std::mutex mutex;
    MachineState machineState;
    homekit::HomekitCommand pendingCommand;
    bool isApplyingCommand = false;
};

class HomekitThermostat : public Service::Thermostat {
  public:
    explicit HomekitThermostat(HomekitSharedState &sharedState);
    boolean update() override;
    void loop() override;

  private:
    HomekitSharedState &sharedState;
    SpanCharacteristic *currentHeatingCooling;
    SpanCharacteristic *targetHeatingCooling;
    SpanCharacteristic *currentTemperature;
    SpanCharacteristic *targetTemperature;
    SpanCharacteristic *displayUnits;
};

// One of the mutually exclusive Brew / Steam / Hot Water switches.
class HomekitModeSwitch : public Service::Switch {
  public:
    HomekitModeSwitch(HomekitSharedState &sharedState, int mode, const char *name);
    boolean update() override;
    void loop() override;

  private:
    HomekitSharedState &sharedState;
    int mode;
    SpanCharacteristic *isOn;
};

class HomekitPlugin : public Plugin {
  public:
    HomekitPlugin(String wifiSsid, String wifiPassword);
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    void startHomeSpan();
    void applyCommand(const homekit::HomekitCommand &command);

    String wifiSsid;
    String wifiPassword;
    HomekitSharedState sharedState;
    bool isStarted = false;
    Controller *controller;
};

#endif // HOMEKITPLUGIN_H
