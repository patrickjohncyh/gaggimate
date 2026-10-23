#ifndef HOMEKITPLUGIN_H
#define HOMEKITPLUGIN_H
#include "../core/Plugin.h"
#include "HomeSpan.h"
#include "homekit/HomekitMapping.h"

#include <mutex>

#define HOMESPAN_PORT 8080
#define DEVICE_NAME "GaggiMate"
// Used as the soak model's reference until the machine has been in brew mode once.
#define DEFAULT_BREW_REFERENCE_TEMPERATURE 93.0f
#define SOAK_UPDATE_INTERVAL_MS 1000

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
        bool isReady = false;
    };

    // Main task -> poll task.
    void setMode(int mode);
    void setCurrentTemperature(float temperature);
    void setTargetTemperature(float temperature);
    void setReady(bool isReady);
    MachineState getMachineState() const;

    // Poll task -> main task. Writes are staged during a HAP request (Service::update()) and
    // committed once HomeSpan moves on to the service loops, so all writes of one request (e.g.
    // Siri turning on every switch at once) reach the main task as a single command.
    void recordCommand(const std::function<void(homekit::HomekitCommand &)> &record);
    void commitStagedCommand();
    bool beginApplyingCommand(homekit::HomekitCommand &command);
    void finishApplyingCommand();
    // True while Home app writes are queued or being applied; characteristics must not be
    // overwritten from MachineState then, or the user's change would flicker back.
    bool hasUnappliedCommand() const;

  private:
    mutable std::mutex mutex;
    MachineState machineState;
    homekit::HomekitCommand stagedCommand;
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

// "Espresso Ready" occupancy sensor, so iOS can notify when the machine reaches temperature.
class HomekitReadySensor : public Service::OccupancySensor {
  public:
    explicit HomekitReadySensor(HomekitSharedState &sharedState);
    void loop() override;

  private:
    HomekitSharedState &sharedState;
    SpanCharacteristic *isDetected;
};

class HomekitPlugin : public Plugin {
  public:
    HomekitPlugin(String wifiSsid, String wifiPassword);
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    void startHomeSpan();
    void applyCommand(const homekit::HomekitCommand &command);
    void publishStatus();

    String wifiSsid;
    String wifiPassword;
    HomekitSharedState sharedState;
    homekit::ReadinessTracker readinessTracker;
    homekit::HeatSoakEstimator heatSoakEstimator;
    // Brew setpoint that counts as "fully hot" for the soak model, also while in steam/standby.
    float brewReferenceTemperature = DEFAULT_BREW_REFERENCE_TEMPERATURE;
    unsigned long lastSoakUpdateMillis = 0;
    bool isStarted = false;
    Controller *controller;
};

#endif // HOMEKITPLUGIN_H
