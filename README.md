# Smart Alarm IoT — ESP32 Anti-Theft Security System

Full-stack IoT anti-theft alarm prototype built with **ESP32**, **ESP-IDF**, **MQTT**, **Firebase Realtime Database** and a browser-based dashboard.

The system allows a user to pair an ESP32 alarm device through a temporary WiFi access point, control it remotely from a web panel, arm/disarm it locally with a keypad, monitor battery voltage, and trigger an alarm when suspicious movement is detected by an accelerometer.

---

## Overview

This project is a complete IoT security prototype consisting of:

- ESP32 firmware written in C using ESP-IDF
- WiFi provisioning through a temporary ESP32 access point
- HTTP endpoint for sending WiFi, MQTT and PIN configuration to the device
- MQTT communication between ESP32 and the backend bridge
- Firebase Realtime Database for device/user state synchronization
- Web dashboard for user login, device pairing and alarm control
- ADXL345 accelerometer-based motion detection
- PIN keypad for local arm/disarm control
- LED and audio alarm feedback
- Battery voltage monitoring through ADC
- Factory reset / provisioning reset support

The goal of the project was to build a realistic end-to-end IoT system, not only a small embedded demo.

---

## Architecture

```text
+---------------------+        MQTT         +----------------------+       Firebase       +---------------------+
| ESP32 Alarm Device  | <-----------------> | Python MQTT Bridge   | <-----------------> | Web Dashboard       |
|                     |                     |                      |                      |                     |
| - ADXL345 sensor    |                     | - MQTT subscriber    |                      | - Firebase Auth     |
| - Keypad            |                     | - MQTT publisher     |                      | - Device cards      |
| - LEDs              |                     | - Firebase listener  |                      | - Arm/Disarm        |
| - Audio alarm       |                     | - Heartbeat          |                      | - Threshold control |
| - Battery monitor   |                     |                      |                      |                     |
+---------------------+                     +----------------------+                      +---------------------+
```

The ESP32 communicates through MQTT.

The Python bridge synchronizes MQTT messages with Firebase.

The web dashboard reads and updates device state through Firebase.

---

## Main Features

### WiFi Provisioning

When the ESP32 has no saved WiFi configuration, it starts a temporary setup access point:

```text
ESP32_SETUP_PROV
```

The dashboard sends configuration data to the ESP32 through:

```http
POST http://192.168.4.1/api/wifi
```

Example provisioning payload:

```json
{
  "ssid": "HomeWiFi",
  "password": "password123",
  "mqtt_uri": "mqtt://MyLaptop",
  "pinpad_code": "1234"
}
```

After receiving the configuration, the ESP32:

1. saves WiFi credentials in NVS,
2. saves MQTT URI and PIN code,
3. returns a MAC-based device identifier,
4. restarts,
5. connects to the configured WiFi network.

---

### MQTT Communication

Each device generates MQTT topics based on its MAC-derived identifier.

Example topics:

```text
iot/device/ESP32_MAC_XXXXXX/data
iot/device/ESP32_MAC_XXXXXX/config
iot/server/status
```

ESP32 publishes telemetry:

```json
{
  "status": "ARMED",
  "x": 0.01,
  "y": -0.03,
  "z": 1.02,
  "alarm": false,
  "battery_mv": 4100
}
```

ESP32 receives configuration commands:

```json
{ "cmd": "ARM" }
```

```json
{ "cmd": "DISARM" }
```

```json
{ "cmd": "ROBBERY" }
```

```json
{ "cmd": "FACTORY_RESET" }
```

```json
{ "threshold": 0.5 }
```

---

### Motion Detection

The firmware reads acceleration data from the **ADXL345** sensor.

When the alarm is armed, the ESP32 calculates movement intensity and compares it with a configurable threshold.

If movement exceeds the threshold:

- alarm state is latched,
- local alarm sound can be started,
- alarm LED animation is enabled,
- MQTT status changes to `ROBBERY`,
- dashboard displays the robbery/alarm state.

---

### Local Keypad Control

The device supports a physical keypad.

The user can:

- enter a PIN code,
- press `#` to confirm,
- press `*` to clear input,
- arm or disarm the alarm locally.

Default fallback PIN:

```text
1234
```

The PIN can be configured during provisioning.

---

### Web Dashboard

The dashboard provides:

- Firebase email/password authentication,
- user-specific device list,
- live alarm status cards,
- acceleration preview,
- battery voltage display,
- arm/disarm buttons,
- robbery test button,
- threshold slider,
- device deletion,
- WiFi provisioning form.

The frontend is implemented as a browser-based dashboard using Firebase SDK and Bootstrap.

---

## Repository Structure

```text
.
├── main/
│   ├── app_main.c
│   └── CMakeLists.txt
│
├── components/
│   ├── wifi_manager/
│   │   ├── wifi_manager.c
│   │   └── wifi_manager.h
│   │
│   ├── mqtt_app/
│   │   ├── mqtt_app.c
│   │   └── mqtt_app.h
│   │
│   └── ...
│
├── MQTT_serwer/
│   └── server.py
│
├── Strona/
│   └── public/
│       └── index.html
│
├── CMakeLists.txt
├── .gitignore
└── README.md
```

---

## Technologies Used

### Embedded

- ESP32
- ESP-IDF
- FreeRTOS tasks
- GPIO
- ADC
- I2C
- mDNS
- NVS flash storage
- MQTT client

### Backend / Bridge

- Python
- paho-mqtt
- firebase-admin
- threading
- Firebase Realtime Database listener

### Frontend

- HTML
- CSS
- JavaScript
- Bootstrap
- Firebase Auth
- Firebase Realtime Database

### Communication

- WiFi
- HTTP provisioning endpoint
- MQTT publish/subscribe
- Firebase synchronization

---

## Hardware

Main hardware modules:

- ESP32 development board
- ADXL345 accelerometer
- Matrix keypad / PIN pad
- Connection status LED
- Alarm / robbery LED
- Optional speaker / audio module
- Battery voltage divider connected to ADC

Pin mapping is configured through ESP-IDF project configuration.

---

## Firmware Flow

1. Initialize NVS, GPIO, ADC, WiFi manager and background tasks.
2. Start the button watchdog task for provisioning reset.
3. Start WiFi manager.
4. If WiFi credentials are missing:
   - start ESP32 SoftAP,
   - expose HTTP endpoint `/api/wifi`,
   - wait for provisioning data.
5. If WiFi credentials are saved:
   - connect to WiFi,
   - start mDNS,
   - read MQTT URI from NVS,
   - initialize MQTT client,
   - generate device-specific topics,
   - subscribe to config and heartbeat topics,
   - start publisher task,
   - start keypad task.
6. During normal operation:
   - read accelerometer data,
   - read battery voltage,
   - monitor heartbeat from server,
   - publish telemetry,
   - process ARM/DISARM/ROBBERY commands,
   - trigger local alarm when movement exceeds threshold.

---

## Backend Bridge Flow

The Python server acts as a bridge between MQTT and Firebase.

It:

1. connects to a local MQTT broker,
2. subscribes to device telemetry topics,
3. receives ESP32 status and sensor data,
4. finds the Firebase user assigned to the device,
5. writes measurements and state to Firebase,
6. listens for Firebase config changes,
7. translates dashboard actions into MQTT commands,
8. publishes heartbeat messages for ESP32 watchdog logic.

---

## Setup

### 1. Clone Repository

```bash
git clone https://github.com/Yeetoo45/IOT-security-project.git
cd IOT-security-project
```

---

### 2. Install ESP-IDF

Install ESP-IDF according to the official Espressif documentation.

After ESP-IDF is available, check that this works:

```bash
idf.py --version
```

---

### 3. Configure Firmware

Set target:

```bash
idf.py set-target esp32
```

Open configuration menu:

```bash
idf.py menuconfig
```

Configure the required GPIO/I2C/ADC options according to your hardware.

---

### 4. Build and Flash ESP32 Firmware

```bash
idf.py build
idf.py -p COM_PORT flash monitor
```

Example on Windows:

```bash
idf.py -p COM5 flash monitor
```

To exit serial monitor:

```text
Ctrl + ]
```

---

## Backend Setup

### 1. Install Python Dependencies

From the project root or from `MQTT_serwer/`:

```bash
pip install paho-mqtt firebase-admin
```

---

### 2. Run MQTT Broker

The backend expects a local MQTT broker.

Example with Mosquitto:

```bash
mosquitto -v
```

Default configuration used by the project:

```text
MQTT_BROKER = localhost
MQTT_PORT   = 1883
```

---

### 3. Configure Firebase Credentials

This repository does **not** include private Firebase service-account credentials.

Create your Firebase service account JSON file locally, for example:

```text
MQTT_serwer/admin_sdk.json
```

Then set environment variables.

Windows PowerShell:

```powershell
$env:FIREBASE_CRED_PATH="MQTT_serwer/admin_sdk.json"
$env:FIREBASE_DB_URL="https://YOUR_PROJECT-default-rtdb.region.firebasedatabase.app/"
python MQTT_serwer/server.py
```

Linux/macOS:

```bash
export FIREBASE_CRED_PATH="MQTT_serwer/admin_sdk.json"
export FIREBASE_DB_URL="https://YOUR_PROJECT-default-rtdb.region.firebasedatabase.app/"
python MQTT_serwer/server.py
```

---

## Frontend Setup

The dashboard is located in:

```text
Strona/public/index.html
```

Before running it, configure Firebase frontend settings inside `index.html`:

```js
const firebaseConfig = {
  apiKey: "YOUR_FIREBASE_API_KEY",
  authDomain: "YOUR_PROJECT.firebaseapp.com",
  databaseURL: "https://YOUR_PROJECT-default-rtdb.region.firebasedatabase.app",
  projectId: "YOUR_PROJECT_ID"
};
```

You can serve the frontend locally:

```bash
cd Strona/public
python -m http.server 5173
```

Then open:

```text
http://localhost:5173
```

---

## Device Pairing Flow

1. Flash ESP32 firmware.
2. Start the device.
3. If no WiFi credentials are saved, ESP32 creates WiFi network:

```text
ESP32_SETUP_PROV
```

4. Connect your computer/phone to this network.
5. Open the dashboard.
6. Enter:
   - WiFi SSID,
   - WiFi password,
   - MQTT server name/URI,
   - PIN code.
7. Click pairing button.
8. ESP32 saves configuration and restarts.
9. Device appears in the dashboard.

---

## MQTT Protocol

### Device Telemetry

ESP32 publishes to:

```text
iot/device/<device_id>/data
```

Example payload:

```json
{
  "status": "ARMED",
  "x": 0.12,
  "y": -0.04,
  "z": 1.01,
  "alarm": false,
  "battery_mv": 4120
}
```

Status values:

```text
DISARMED
ARMED
ROBBERY
DELETE_DB
-
```

---

### Device Configuration

ESP32 subscribes to:

```text
iot/device/<device_id>/config
```

Example payloads:

```json
{ "cmd": "ARM" }
```

```json
{ "cmd": "DISARM" }
```

```json
{ "cmd": "ROBBERY" }
```

```json
{ "cmd": "FACTORY_RESET" }
```

```json
{ "threshold": 0.7 }
```

---

### Server Heartbeat

The backend publishes heartbeat messages to:

```text
iot/server/status
```

The ESP32 uses this heartbeat to detect whether the backend is alive.

If heartbeat is missing for too long:

- device marks the server as offline,
- telemetry publishing is paused,
- LED status indicates connection/server issue.

---

## Firebase Data Model

Example database structure:

```text
devices_registry/
  ESP32_MAC_XXXXXX: user_uid

users/
  user_uid/
    devices/
      ESP32_MAC_XXXXXX/
        config/
          state: ARMED
          threshold: 0.5
          battery_mv: 4100
        measurements/
          measurement_id/
            accel_x: 0.01
            accel_y: -0.03
            accel_z: 1.02
            battery_mv: 4100
            timestamp: ...
```

---

## Security Notes

This is an educational/portfolio IoT security prototype.

Current security-conscious design choices:

- private Firebase Admin SDK credentials are not stored in the repository,
- backend uses environment variables for Firebase configuration,
- `.gitignore` excludes local credential files,
- frontend uses placeholder Firebase config in the public repository.

Limitations that should be improved before real-world deployment:

- provisioning access point should use a temporary password,
- provisioning mode should have a timeout,
- sensitive data should never be logged,
- MQTT should use authentication,
- MQTT should use TLS when deployed outside a trusted local network,
- Firebase Realtime Database rules should be strict,
- MAC-based device IDs should be replaced or strengthened with real device authentication,
- retained MQTT messages should be carefully separated between persistent desired state and one-time commands.

---

## What I Learned

This project helped me practice and connect multiple areas of software engineering:

- embedded C programming on ESP32,
- FreeRTOS task-based architecture,
- WiFi provisioning,
- persistent configuration with NVS,
- MQTT publish/subscribe communication,
- sensor integration with ADXL345,
- keypad-based local authentication,
- Firebase Realtime Database integration,
- frontend dashboard development,
- backend bridge design,
- IoT security considerations,
- debugging a full system across hardware, firmware, backend and frontend.

---

## Why This Project Matters

This is not only a simple ESP32 example.

It is a complete prototype that connects:

- physical sensors,
- embedded firmware,
- local networking,
- message broker communication,
- cloud database synchronization,
- authentication,
- user interface,
- alarm-state logic.

That makes it a strong portfolio project for roles connected with:

- embedded systems,
- IoT,
- backend development,
- full-stack development,
- cybersecurity,
- software engineering,
- AI/ML infrastructure and MLOps-adjacent engineering.

---

## Current Status

Implemented:

- ESP32 firmware
- WiFi provisioning
- MQTT client
- Firebase bridge
- web dashboard
- accelerometer-based alarm logic
- PIN keypad control
- battery voltage reporting
- server heartbeat
- factory reset mechanism

Planned improvements:

- add screenshots and demo video,
- add hardware wiring diagram,
- add Firebase rules example,
- add Docker setup for MQTT/backend,
- add TLS/authentication for MQTT,
- improve production security hardening,
- clean up comments and split modules further.

---

## Author

**Dawid Węcirz**

Computer Science student interested in:

- IoT systems
- cybersecurity
- backend engineering
- AI/ML
- MLOps
- full-stack project development

GitHub: [@Yeetoo45](https://github.com/Yeetoo45)
