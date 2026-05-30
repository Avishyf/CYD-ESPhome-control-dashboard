# Cheap Yellow Display (CYD) ESPHome Lights Controller

[![Device](https://img.shields.io/badge/Hardware-ESP32--2432S028-yellow.svg)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
[![Framework](https://img.shields.io/badge/Framework-ESPHome-green.svg)](https://esphome.io/)
[![Platform](https://img.shields.io/badge/Integration-Home%20Assistant-blue.svg)](https://www.home-assistant.io/)

A state-of-the-art, premium touch-based control panel replica for your Home Assistant lights and switches. This project runs natively on the **Cheap Yellow Display (CYD / `esp32-2432s028`)**, leveraging the advanced hardware features of the ESP32 and ESPHome's declarative LVGL graphics component.

---

## 📸 Dashboard Previews

*Here you can drop the pixel-perfect digital screenshots you captured from the VS Code C++ Simulator to showcase your gorgeous dashboard:*

| Home Screen Dashboard | Dimmer Sub-Page | settings menu |
| ![](https://github.com/Avishyf/CYD-ESPhome-control-dashboard/blob/main/light_control.png) | :---: | :---: |
| *(Drop Home Screen screenshot here)* | *(Drop Dimmer page screenshot here)* | *(Drop Settings page screenshot here)* |

---

## 🚀 Core Features

### 1. Symmetrical 180° Screen & Touch Rotation
To support mounting the CYD upside down (or changing your micro-USB cable exit path), the entire visual layout is rotated exactly 180 degrees (`rotation: 180`). Touch inputs are mathematically calibrated and remapped (`swap_xy: true`, `mirror_x: false`, `mirror_y: false`) to ensure pixel-perfect and responsive landscape presses!

### 2. Premium Inactivity Screen Saver & "Zero-Click" Wake
* **Automated Fade-to-Black**: Uses LVGL's native C++ inactivity tracking API to smoothly fade the display backlight to `0%` over **0.5 seconds** when unused.
* **Accidental Click Interceptor**: Pauses LVGL's touch engine the instant the screen turns off. When you touch a dark screen, the controller turns the backlight on instantly (`0s` transition) and resumes LVGL, **completely discarding the first touch**. This ensures you can wake the screen without accidentally toggling lamps or sliders!

### 3. Symmetrical 2-Column Settings & 7-Step Timeout Slider
* **Balanced Grid Layout**: Reorganizes settings information (SSID, IP, LDR ambient light, and green WiFi bars) into a clean, two-column grid.
* **Non-Linear Timeout Control**: Provides an Outfit-styled screen timeout slider with **7 non-linear steps**: `5s`, `15s`, `30s`, `1m`, `3m`, `5m`, and `10m`, pre-populating with your saved choice on menu entry.

### 4. Advanced WiFi Battery Optimization (LIGHT Power Save)
Configures `power_save_mode: LIGHT` on the ESP32's WiFi radio. By pulsing the receiver in line with your router's beacon intervals, the idle screen-saver current draw drops from **35mA down to just ~12mA at 5V**—more than doubling battery life for portable remote builds, while retaining **instant (0.01s) click responsiveness** and lag-free bidirectional HA communication!

### 5. Dynamic Runtime Customization & Commits
Change button modes (Sliders vs Toggle Switch) and choose from **8 selectable MDI logos** (Bulb, Recessed Spot, Fan, Bed, Air Conditioner, etc.) directly from the CYD config page. Selection is permanently written to flash within **10 seconds** of any change using a throttled commit interval (`flash_write_interval: 10s`), protecting flash memory from wear while surviving power reboots.

### 6. Bidirectional Switch & Light Blueprints (Zero-Bounce)
Includes custom Home Assistant automation blueprints featuring strict **reconnection state guards**. When the CYD boots or recovers from a WiFi disconnect, these guards block the transition from `unavailable` to `off` from triggering a turn-off event on your physical lamps. If a bedroom lamp is already ON, the connecting CYD will smoothly synchronize and update its virtual state instead of shutting it down!

---

## 🛠️ Setup & Installation Guide

### Step 1: Flashing your CYD Controller
1. Open your **ESPHome Dashboard** in Home Assistant or locally on your PC.
2. Create a new device or edit your existing one, pasting the contents of [`esphome_cyd_lights.yaml`]
3. Connect your Cheap Yellow Display board to your computer using a USB cable.
4. install the bin file using [`https://web.esphome.io/`]
5. *Important Boot Mode*: **hold down the BOOT button on the back of your CYD board and rst, then leave boot the leave RST** to make sure it flashable.

### Step 2: Setting up Bidirectional Automations in HA
1. Copy [`zero_bounce_light_sync_blueprint.yaml`] and [`zero_bounce_switch_sync_blueprint.yaml`] into your Home Assistant directory under `/config/blueprints/automation/`.
2. Go to **Settings > Automations & Scenes > Blueprints** and reload or re-import the blueprints.
3. Create a new automation from the blueprints:
   * Select your CYD virtual light/switch entity (e.g. `light.cyd_l1`).
   * Select the target physical lamp or smart switch in your room.
4. Save and enjoy a robust, bounce-free bidirectional synchronizer!

---

## 💡 Key Design Considerations for Forking

* **Gamma Correction**: ESPHome applies a default `2.8` gamma correction. Because these CYD lights are virtual and target physical lights that already perform their own gamma corrections, `gamma_correct: 0` is set to ensure a true **1:1 linear scaling** of brightness.
* **Flash Writes Protection**: Setting `flash_write_interval: 10s` is a perfect compromise between retaining your latest configurations across reboots and saving your ESP32's flash memory cells from quick degradation. Do not reduce this under `5s`.
* **MDI Glyphs Bounds**: When adding new custom icons to the logo picker, ensure their UTF-8 byte sequences are explicitly included in the `font:` configuration block of the YAML, or they will render as blank boxes.
