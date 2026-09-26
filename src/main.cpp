
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_timer.h>
#include <esp_littlefs.h>
#include "WaterTest.h"
#include "ntp.h"

#include <thorlog.h>
#include <thorlog_espidf.h>
#include "Brewpi.h"

#include "Ticks.h"
#include "Display.h"
#include "TempControl.h"
#include "PiLink.h"
#include "Menu.h"
#include "Pins.h"
#include "RotaryEncoder.h"
#include "Buzzer.h"
#include "TempSensor.h"
#include "TempSensorMock.h"
#include "TempSensorExternal.h"
#include "Ticks.h"
#include "Sensor.h"
#include "SettingsManager.h"
#include "ESP_BP_WiFi.h"
#include "CommandProcessor.h"
#include "PromServer.h"
#include "wireless/BTScanner.h"
#include "tplink/TPLinkScanner.h"
#include "http_server.h"
#include "GlycolLog.h"

#include "rest/rest_send.h"
#include "OneWireTempSensor.h"
#include <esp_system.h>
#include <esp_heap_caps.h>

#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_event.h>

#if BREWPI_SIMULATE
#include "Simulator.h"
#endif

/**
 * \file brewpi-esp8266.cpp
 *
 * \brief Main project entrypoint
 */

// global class objects static and defined in class cpp and h files
// instantiate and configure the sensors, actuators and controllers we want to use


/*
 * Create the correct type of PiLink connection for how we're configured.
 * The backend provides the low-level I/O (UART or TCP socket).
 */
#if defined(CONNECT_VIA_WIFI)
// TCP socket backend -- reads/writes via telnet_client_fd managed by wifi_connect_clients()
TcpBackend piLinkBackend(telnet_client_fd);
#else
// UART backend -- reads/writes via ESP-IDF UART driver (works for both HardwareSerial and USBCDC chips)
UartBackend piLinkBackend;
#endif
PiLink piLink(piLinkBackend);

/* Configure the counter and delay timer. The actual type of these will vary depending upon the environment.
* They are non-virtual to keep code size minimal, so typedefs and preprocessing are used to select the actual compile-time type used. */
TicksImpl ticks = TicksImpl(TICKS_IMPL_CONFIG);
DelayImpl wait = DelayImpl(DELAY_IMPL_CONFIG);

DisplayType realDisplay;
DisplayType DISPLAY_REF display = realDisplay;

ValueActuator alarm_actuator;

void printMem() {
    const uint32_t free = esp_get_free_heap_size();
    const uint32_t max = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint8_t frag = 100 - (max * 100) / free;
    printf("Free Heap: %lu, Largest contiguous block: %lu, Frag: %u%%\r\n",
           (unsigned long)free, (unsigned long)max, frag);
}

/**
 * \brief Restart the board
 */
void handleReset()
{
    // The asm volatile method doesn't work on ESP32. Instead, use esp_restart
    esp_restart();
}

// For ThorLog support
void printTimestamp(ThorPrint *_logOutput)
{
    char c[12];
    sprintf(c, "%10lu ", (unsigned long)(esp_timer_get_time() / 1000ULL));
    _logOutput->print(c);
}

void printPrefix(ThorPrint* _logOutput, int logLevel) {
    printTimestamp(_logOutput);
//    printLogLevel (_logOutput, logLevel);
}


/**
 * \brief Startup configuration
 *
 * - Start up the filesystem
 * - Initialize the display
 * - If in simulation mode, bootstrap the simulator
 * - Start the PiLink connection (either tcp socket or serial, depending on compile time configuration)
 */
void setup()
{
#ifdef CONNECT_VIA_WIFI
    // UART0 is initialised by the ESP-IDF console/logging subsystem.
    // When in WiFi mode, Serial is only used for debug logging (via printf / ESP_LOG).

#ifndef DISABLE_LOGGING
    Log.begin(THORLOG_LOG_LEVEL, &EspIdfOutput, true);
    Log.setPrefix(printPrefix);
    Log.notice("Serial logging started.\r\n");
#endif

#endif


    // Before anything else, let's get the filesystem working. We need to start it up, and then test if the file system
    // was formatted.
    // For ESP32 - mount LittleFS via ESP-IDF VFS layer using POSIX I/O
    {
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = "/littlefs";
        conf.partition_label = "spiffs";  // Partition table CSV uses "spiffs" as the label
        // A failed mount must not erase configuration or an unacknowledged test.
        conf.format_if_mount_failed = false;
        conf.dont_mount = false;
        esp_err_t ret = esp_vfs_littlefs_register(&conf);
        if (ret != ESP_OK) {
            printf("Failed to mount LittleFS: %s\n", esp_err_to_name(ret));
        }
    }

  deviceManager.preloadActuatorPins();  // Preload any pin-based actuators to set their pin modes

  extendedSettings.loadFromFilesystem();
  upstreamSettings.loadFromFilesystem();
  display.init();

  // Initialize tempControl before bringing up WiFi. The HTTP server starts
  // answering requests as soon as WiFi associates, and its JSON handlers
  // dereference tempControl.beerSensor / fridgeSensor. Those static pointers
  // are NULL until TempControl::init() allocates default sensors, so a
  // browser auto-refresh during boot would otherwise crash the device.
  tempControl.init();
  // Recover experiment ownership before saved modes or network commands load.
  WaterTest::init();

  // Order matters: wifi_cfg's Network Provisioning backend (esp_wifi_config
  // 0.1.0+) uses Espressif's wifi_prov_scheme_ble, which unconditionally calls
  // esp_bt_controller_init() and brings up its own NimBLE host. If NimBLE is
  // already up, that fails with ESP_ERR_INVALID_STATE and provisioning never
  // starts. So wifi_cfg has to be initialised first; bt_scanner.init() below
  // calls NimBLEDevice::init() afterwards and re-attaches to the controller,
  // which is kept resident by .prov_ble.memory_policy = KEEP_ALL.
  initialize_wifi();
  initNTP();

#ifdef HAS_BLUETOOTH
  bt_scanner.init();
#endif

#if BREWPI_BUZZER
	buzzer.init();
	buzzer.beep(2, 500);
#endif


	piLink.init();  // Initializes either the serial or telnet connection

#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
  // Initialize UDP and send the initial discovery message
  // TODO - Test how this reacts when WiFi is not available
  tp_link_scanner.init();
  tp_link_scanner.send_discover();
  vTaskDelay(pdMS_TO_TICKS(200)); // This should be very quick
  tp_link_scanner.process_udp_incoming();
#endif

#ifdef HAS_BLUETOOTH
    bt_scanner.scan();
    display.printBluetoothStartup();  // Alert the user about the startup delay
    vTaskDelay(pdMS_TO_TICKS(10000));
#endif

	logDebug("started");

	// Initialize OneWire buses
	if (!deviceManager.initOneWireBuses()) {
		logDebug("Failed to initialize OneWire buses");
	}

	settingsManager.loadSettings();  // Also fully loads devices
  WaterTest::tick();

#ifdef BREWPI_CHILLSIM_TEST
    // A reboot never silently resumes an unattended hardware experiment.
    tempControl.setMode(Modes::off, true);
    tempControl.updateOutputs();
#endif

#if BREWPI_SIMULATE
	simulator.step();
	// initialize the filters with the assigned initial temp value
	tempControl.beerSensor->init();
	tempControl.fridgeSensor->init();
#endif

	// Once the WiFi and piLink are initialized, we want to display a screen with connection information
  display_connect_info_and_create_callback();

  // NTP runs asynchronously once after the first WiFi association.
#ifdef ENABLE_GLYCOL_LOGGING
  glycolLog.logReboot();
#endif

	display.clear();
	display.printStationaryText();
	display.printState();


#ifdef ENABLE_PROMETHEUS_SERVER
  if(Config::Prometheus::enable())
    promServer.setup();
#endif

//	rotaryEncoder.init();

	logDebug("init complete");
  rest_handler.init();
}



/**
 * \brief Main execution loop
 */
void brewpiLoop()
{
  WaterTest::tick();
	static unsigned long lastUpdate = 0;
	uint8_t oldState;
#ifdef BREWPI_IIC  // We only want to do this for the IIC displays
    static unsigned long lastLcdUpdate = 0;
    if(ticks.millis() - lastLcdUpdate >= (180000)) { //reset lcd every 180 seconds as a workaround for screen scramble
        lastLcdUpdate = ticks.millis();

        DisplayType::init();
        DisplayType::printStationaryText();
        DisplayType::printState();

        rotaryEncoder.init();
    }
#endif

  if (ticks.millis() - lastUpdate >= (1000)) { //update settings every second
    // printMem();

		lastUpdate = ticks.millis();

#if BREWPI_BUZZER
		buzzer.setActive(alarm_actuator.isActive() && !buzzer.isActive());
#endif

		tempControl.updateTemperatures();

		tempControl.detectPeaks();
		tempControl.updatePID();
		oldState = tempControl.getState();
		tempControl.updateState();

		if (oldState != tempControl.getState()) {
          piLink.sendStateNotification();
		}
		tempControl.updateOutputs();

#if BREWPI_MENU
		if (!WaterTest::controlOwned() && rotaryEncoder.pushed()) {
			rotaryEncoder.resetPushed();
			menu.pickSettingToChange();
		}
#endif

		// update the lcd for the chamber being displayed
    display.printAll();
	}

	//listen for incoming connections while waiting to update
  wifi_connect_clients();
  CommandProcessor::receiveCommand();

#ifdef HAS_BLUETOOTH
if(bt_scanner.scanning_failed()) {
#ifdef ENABLE_HTTP_INTERFACE
  // rest_handler.send_bluetooth_crash_report();
  // TODO - Figure out if we want to keep this here
#endif
  esp_restart();
}
  bt_scanner.scan();        // Check/restart scan 
#endif

#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
  if (!WaterTest::controlOwned()) tp_link_scanner.scan_and_refresh();
#endif

#ifdef ENABLE_HTTP_INTERFACE
  // The webserver is now handled asynchronously, so we don't need to call handleClient() here
  http_server.processQueuedDeviceDefinition();  // Do this in the main loop to avoid issues with blocking to read DS18b20s
  // The upstream client performs blocking HTTP. Experiment timing and immutable
  // configuration must not depend on those requests or upstream commands.
  if (!WaterTest::controlOwned()) rest_handler.process();
  http_server.processQueuedActions();
#endif

}


/**
 * \brief Main execution loop
 *
 * This dispatches to brewpiLoop(), or if we're in simulation mode simulateLoop()
 */
void loop() {
#if BREWPI_SIMULATE
	simulateLoop();
#else
	brewpiLoop();
#endif
}

extern "C" void app_main(void) {
#ifdef BREWPI_CHILLSIM_TEST
    // Establish OFF before NVS recovery, filesystem or network initialization.
    // The known shield is active-low even without persisted device settings.
    gpio_set_level(GPIO_NUM_25, 1);
    gpio_set_level(GPIO_NUM_26, 1);
    gpio_set_direction(GPIO_NUM_25, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT);
#endif
    // Initialize NVS (required for WiFi credential storage)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    // Initialize TCP/IP stack and default event loop
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    // Run setup on the main task (stack size set via CONFIG_ESP_MAIN_TASK_STACK_SIZE)
    setup();

    // Create loop task on the app core
    xTaskCreatePinnedToCore(
        [](void*) { for (;;) { loop(); vTaskDelay(pdMS_TO_TICKS(10)); } },
        "loopTask",
        8192,
        nullptr,
        1,
        nullptr,
        1  // Core 1 = app core
    );
}
