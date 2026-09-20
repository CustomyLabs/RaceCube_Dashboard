


# RaceCube: Wireless Telemetry & TCU System 🏎️

**RaceCube** is a decentralized, ESP32-based wireless CAN-bus telemetry and automatic transmission control system. It is specifically engineered for **Dodge Caliber** vehicles swapped with Mitsubishi/Chrysler **F4A42/51** and **F5A42/51** automatic transmissions.

The system utilizes a 0-latency **ESP-NOW** wireless protocol to synchronize data between under-hood and cabin modules, completely eliminating the need to pull bulky wiring harnesses through the firewall.

---

<img width="640" height="480" alt="DiqJNhkPS0VUn1MjPreDkzDChizRIw4M2cwgITTdK58TJN1Hw1Yn6GHczMDFE508I_t-takj54m6CB2NZvlYg4xK" src="https://github.com/user-attachments/assets/43af99fc-03f8-466c-bd3f-2974fe24c6ee" />
<img width="640" height="480" alt="uIsQmHDQpzdxJ8-qTZhZGK9AnnsGVKyu-rEx1c08tP-jy3dcjbnyJzqfuJQYeH1Z8itbJzxiQPRgkQwPszeELccV" src="https://github.com/user-attachments/assets/7dc8688f-a370-434a-90da-615f9bdec74a" />
<img width="640" height="480" alt="cUFC8w9cv_jrAdwrMKmednJKwVpbVbr_rKTrtJZIaoOKswXi2BIqgVhxJfCLL9YBfnncMWuAM-MuiZxs0bH2uYFO" src="https://github.com/user-attachments/assets/ea84a879-5c94-4fcb-ad3d-ba5dc9c15af7" />
<img width="1280" height="720" alt="BTLX7K7x-Bg55LGbyOsiAXqrZWMg0TMVj3rbpjoTcWRanbx5vmcwaHZx1Qf7IEjSYIFb_FlCxCkODaAQ2EzOXMeX" src="https://github.com/user-attachments/assets/05b43b87-6348-4213-8f5c-82869fa0032f" />
<img width="1280" height="720" alt="454I2d743FAYN8uV84qGwtbLdiVf5vNXIWwV4CYeX7wsxxnsMqAZZMGokNxfFM9psKkLAELYZzL-XaGQiAIVrqY7" src="https://github.com/user-attachments/assets/e05bd298-5f48-4d06-b1b9-4c925a5cd654" />
<img width="828" height="1792" alt="zVFVv_V52W_jTT1OU-eNsTS5mhaH3xWXKm5g2ikrCFFYTKhsMCUB0o8m3j3727eWFN9hBMxhfFffWpviOlL-y9_x" src="https://github.com/user-attachments/assets/b0e2f314-1468-4966-bfd3-ca10f12b04cd" />
<img width="960" height="1280" alt="Untitled3333" src="https://github.com/user-attachments/assets/72d12b24-cafa-4e58-acf9-ff4557e3c0a6" />
<img width="960" height="1280" alt="Untitled4444" src="https://github.com/user-attachments/assets/d9ef5414-703c-482e-bc95-3576bfa5cb00" />
<img width="960" height="1280" alt="Untitled55" src="https://github.com/user-attachments/assets/a41c6b84-5635-46ca-a2a3-416c44b2ac08" />
<img width="720" height="1280" alt="436" src="https://github.com/user-attachments/assets/daa34d8f-41f6-4855-91dd-23ac1ad501cc" />
<img width="1280" height="720" alt="76876" src="https://github.com/user-attachments/assets/255c7dfe-4fd6-4c81-b91a-93461435c556" />
<img width="1133" height="1280" alt="75637" src="https://github.com/user-attachments/assets/e5a2e8de-012b-458f-958e-44221b15d488" />
<img width="768" height="1024" alt="round" src="https://github.com/user-attachments/assets/0d60a4ba-048f-421e-812b-6d83ffec07cc" />

## 🏗️ System Architecture

The RaceCube ecosystem consists of multiple ESP32 nodes working in unison:

### 1. Transmission Module (`RaceCube_CAN_TRANSIVER_AT`)
*   **Role:** The core control unit for the gearbox (Under-Hood / CAN-C Bus).
*   **Functions:** 
    *   Reads physical gear selector pins (P, R, N, D, 1, 2, 3, +, -) and ATF temperature.
    *   Connects to the high-speed engine CAN-C bus to read OBD data (RPM, Speed, Coolant, Boost, Fuel, MIL, Oil Pressure).
    *   **Torque Reduction (Shift-Cut):** Sends custom UDS commands to the Engine ECU to artificially drop RPMs and engine load during gear shifts, ensuring smooth and safe gear engagements for swapped transmissions.
    *   Packages all data into a `SuperTelemetry` struct and broadcasts it via ESP-NOW.

### 2. Comfort Module (`CAN_B_REALDASH_DODGE_CALIBER`)
*   **Role:** The interior state listener (Cabin / CAN-B Bus).
*   **Functions:**
    *   Listens to the low-speed cabin CAN-B bus.
    *   Extracts the status of Turn Signals, High/Low Beams, Park Lights, and the Handbrake.
    *   Broadcasts a `ComfortTelemetry` struct via ESP-NOW.

### 3. CYD Gateway & Web Monitor (`CYD_Realdash_Dodge_Caliber`)
*   **Role:** The diagnostic gateway and RealDash forwarder (Dashboard).
*   **Functions:**
    *   Combines data from Transmission and Comfort modules.
    *   Formats data into custom CAN packets and sends them via USB Serial to the RealDash app running on an Android head unit.
    *   Hosts a Wi-Fi Access Point (`RACECUBE_CAN`) with a dark neon Web UI to monitor raw pin states and CAN data in real-time.

### 4. Smart Round Display (`Racecube_Round_AT`)
*   **Role:** A standalone secondary instrument cluster.
*   **Functions:**
    *   Runs on an ESP32-based circular touchscreen (e.g., ST77916).
    *   Receives `SuperTelemetry` directly via ESP-NOW.
    *   Renders an aggressive, F1-style sweep tachometer, gear indicator, and custom gauges (Boost, AFR, ATF Temp, etc.).
    *   Features critical safety overlays (e.g., massive flashing "LOW OIL PRESS" alert).

---

## ⚡ Wireless Communication (ESP-NOW + Wi-Fi)

RaceCube solves the problem of network latency by using **ESP-NOW** for critical vehicle data. This allows the Transmission and Comfort modules to send data to the Displays in just a few milliseconds. 

Simultaneously, the modules support standard **Wi-Fi** for Over-The-Air (OTA) updates and Web Diagnostics, without interrupting the ESP-NOW telemetry stream.

---

## 🛠️ Connection & Diagnostic Guide

### 1. Accessing the Web Monitor (Live Diagnostics)
If you need to check which gear pin is active, verify CAN-bus connection, or read ATF temperature without a laptop:
1. Turn on the vehicle ignition.
2. On your smartphone or laptop, connect to the Wi-Fi network:
   * **SSID:** `RACECUBE_CAN`
   * **Password:** `012345678`
3. Open a web browser and go to `http://192.168.4.1`.
4. The **RaceCube CAN Monitor** will load, showing live JSON data parsing, engine vitals, and raw hardware pin states.

### 2. Over-The-Air (OTA) Flashing
You can update the firmware of any module wirelessly directly from the Arduino IDE, without digging under the hood or dismantling the dashboard:
1. Ensure your laptop is connected to the `RACECUBE_CAN` Wi-Fi network.
2. Open your sketch in the **Arduino IDE**.
3. Go to `Tools -> Port`. You will see the network ports discovered via mDNS (e.g., `RaceCube-Trans`, `RaceCube-Comfort`).
4. Select the target module and click **Upload**.

---

## 📊 RealDash Setup

To display RaceCube data on your Android head unit or tablet:
1. Copy the `RealDash/RaceCube.xml` file to your device.
2. Connect the CYD Gateway to your device via USB.
3. In RealDash, navigate to `Garage -> Connections -> Add -> CAN/LIN -> Custom CAN -> Import`.
4. Select `RaceCube.xml`. 
5. The app will instantly map RPM, Speed, Boost, Fuel, Current Gear, Handbrake, Oil Pressure warning, and lighting statuses.

---

## 📄 License
This project is licensed under the **GNU Affero General Public License v3.0 (AGPLv3)**. 
Any modifications, including those integrated into network-accessible devices, must remain open-source.
