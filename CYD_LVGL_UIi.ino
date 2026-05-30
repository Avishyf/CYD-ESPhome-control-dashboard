/**
 * Cheap Yellow Display (CYD / ESP32-2432S028) Standalone UI Dashboard
 * Framework: Arduino IDE (with ESP32 Board Manager)
 * Core Libraries: LVGL (v8.x or v9.x), TFT_eSPI, and XPT2046_Touchscreen
 * 
 * Description: 
 * A 1:1 pixel-perfect standalone clone of the premium smart home touchscreen dashboard.
 * Matches the exact card grids, intersections, symmetrical alignments, styling tokens, 
 * page routings, and dynamic controls defined in the ESPHome YAML configuration.
 * 
 * Fully compatible with both LVGL v8 and LVGL v9, and supports ESP32 Core v2.x and v3.x.
 * 
 * Hardware Config for ESP32-2432S028:
 * - TFT Display: ILI9341 on HSPI (CS=15, DC=2, CLK=14, MOSI=13, MISO=12)
 * - TFT Backlight: GPIO 21 (PWM controlled)
 * - Touch Controller: XPT2046 on VSPI (CS=33, CLK=25, MOSI=32, MISO=39)
 * - Onboard LDR: GPIO 34 (Analog ADC)
 * - Onboard RGB LED: Red=GPIO 4, Green=GPIO 16, Blue=GPIO 17
 */

// Suppress the GCC 14 linker warning regarding executable stack (missing .note.GNU-stack section)
#if defined(__GNUC__) && defined(__ELF__)
__asm__(".section .note.GNU-stack,\"\",@progbits");
#endif

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

// --- PIN DEFINITIONS ---
#define XPT2046_CS   33
#define LDR_PIN      34 // Onboard LDR Light Sensor
#define LED_RED      4
#define LED_GREEN    16
#define LED_BLUE     17
#define BACKLIGHT_PIN 21

// Resistive Touch Screen Dedicated SPI pins (SCLK=25, MISO=39, MOSI=32)
#define XPT2046_CLK  25
#define XPT2046_MOSI 32
#define XPT2046_MISO 39

// --- TFT & TOUCH OBJECTS ---
TFT_eSPI tft = TFT_eSPI(); 

// Instantiate Touch Screen without the optional IRQ pin to enable polling mode.
// This is significantly more robust and prevents interrupt conflicts on many ESP32 boards!
XPT2046_Touchscreen touch(XPT2046_CS);

// Dedicated SPI bus class instance for Touch Controller to solve touch issues!
SPIClass touchSPI(VSPI);

// --- LVGL SETUP ---
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;

// --- APP STATE & GLOBALS ---
int active_channel = 1;      // Current channel selected (1-4)
int config_channel = 1;      // Channel currently being configured (1-4)
bool channel_states[4] = {false, false, false, false};
int channel_brightness[4] = {100, 80, 50, 0};
int channel_temp[4] = {300, 300, 300, 300}; // Mireds (153 to 500)
int channel_types[4] = {1, 1, 1, 1};        // Mode: 1=Dimmer (Sliders), 2=Switch (Toggle)
int channel_logos[4] = {0, 1, 2, 3};        // Icon Index: 0 to 7
int screen_timeout_s = 60;   // Inactivity timeout
uint32_t last_activity_time = 0;
bool screen_sleeping = false;

// Custom text titles for channels
const char* channel_names[4] = {
  "Ceiling Light",
  "Spotlights",
  "Reading Lamp",
  "Bed Fan"
};

// LVGL standard symbols representing the 8 logos defined in the YAML MDI configuration
const char* selectable_logos[8] = {
  LV_SYMBOL_CHARGE,    // 1. Ceiling Light (BULB)
  LV_SYMBOL_IMAGE,     // 2. Recessed Light (SPOT)
  LV_SYMBOL_LOOP,      // 3. Ceiling Fan Light (FAN+L)
  LV_SYMBOL_HOME,      // 4. Outdoor Lamp (LAMP)
  LV_SYMBOL_POWER,     // 5. On/Off Toggle (POWER)
  LV_SYMBOL_REFRESH,   // 6. Ceiling Fan (FAN)
  LV_SYMBOL_SETTINGS,  // 7. Air Conditioner (A/C)
  LV_SYMBOL_KEYBOARD   // 8. Bed Icon (BED)
};

// --- LVGL SCREEN OBJECTS ---
lv_obj_t* scr_home;
lv_obj_t* scr_dimmer;
lv_obj_t* scr_settings;
lv_obj_t* scr_config;
lv_obj_t* scr_switch;
lv_obj_t* scr_wifi_hotspot;

// UI Elements that need dynamic updating
lv_obj_t* home_cards[4];
lv_obj_t* home_status_labels[4];
lv_obj_t* home_logo_labels[4];
lv_obj_t* dimmer_title_label;
lv_obj_t* dimmer_logo_label;
lv_obj_t* brightness_slider;
lv_obj_t* tone_slider;
lv_obj_t* settings_time_label;
lv_obj_t* settings_ssid_label;
lv_obj_t* settings_ip_label;
lv_obj_t* settings_wifi_signal_label;
lv_obj_t* settings_ldr_label;
lv_obj_t* wifi_bars[4];
lv_obj_t* timeout_label;
lv_obj_t* timeout_slider;
lv_obj_t* backlight_override_slider;
lv_obj_t* switch_title_label;
lv_obj_t* switch_page_logo_label;
lv_obj_t* switch_toggle_label;
lv_obj_t* btn_switch_toggle;
lv_obj_t* btn_settings;

// Config buttons references for highlighting
lv_obj_t* btn_mode_dimmer;
lv_obj_t* btn_mode_switch;
lv_obj_t* btn_logos[8];
lv_obj_t* config_title_label;

// Simple track lock to avoid multi-events collision
bool channel_long_pressed[4] = {false, false, false, false};

// --- PWM BACKLIGHT CONTROL ---
void set_backlight(uint8_t duty) {
  #if defined(ESP32)
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
      ledcWrite(BACKLIGHT_PIN, duty);
    #else
      ledcWrite(0, duty);
    #endif
  #else
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, duty > 0 ? HIGH : LOW);
  #endif
}

// --- DUAL COMPATIBILITY: LVGL v8 vs v9 DISPLAY/TOUCH FLUSH DRIVERS ---

#if LVGL_VERSION_MAJOR >= 9

static lv_display_t * disp;
static uint8_t draw_buf[screenWidth * 10 * sizeof(lv_color_t)];

void my_disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);
}

void my_touchpad_read(lv_indev_t * indev, lv_indev_data_t * data) {
  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    last_activity_time = millis();

    if (screen_sleeping) {
      screen_sleeping = false;
      set_backlight(200);
      data->state = LV_INDEV_STATE_RELEASED;
      delay(150);
      return;
    }

    // Symmetrical touch mapping matching YAML:
    // Raw Y maps to Screen X (left/right aligned); Raw X maps to Screen Y (up/down corrected)
    int16_t screenX = map(p.y, 3588, 588, 0, screenWidth);
    int16_t screenY = map(p.x, 430, 3650, 0, screenHeight);

    // Constrain to prevent LVGL out of bounds issues
    screenX = constrain(screenX, 0, screenWidth - 1);
    screenY = constrain(screenY, 0, screenHeight - 1);

    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = screenX;
    data->point.y = screenY;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

#else

// LVGL v8 Compatibility Drivers
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    last_activity_time = millis();

    if (screen_sleeping) {
      screen_sleeping = false;
      set_backlight(200);
      data->state = LV_INDEV_STATE_REL;
      delay(150);
      return;
    }

    // Symmetrical touch mapping matching YAML:
    int16_t screenX = map(p.y, 3588, 588, 0, screenWidth);
    int16_t screenY = map(p.x, 430, 3650, 0, screenHeight);

    screenX = constrain(screenX, 0, screenWidth - 1);
    screenY = constrain(screenY, 0, screenHeight - 1);

    data->state = LV_INDEV_STATE_PR;
    data->point.x = screenX;
    data->point.y = screenY;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

#endif

// --- CUSTOM DIALOGUE SCREEN SWAPPING ---
void load_screen(lv_obj_t* target_scr) {
  last_activity_time = millis();
  lv_scr_load(target_scr);
}

// --- DYNAMIC CARD STYLE REFRESH ---
void refresh_home_card(int idx) {
  if (channel_states[idx]) {
    // ON State Styling (Balanced Premium Green-ish Dark Accent 0x2C4022)
    lv_obj_set_style_bg_color(home_cards[idx], lv_color_hex(0x2C4022), LV_PART_MAIN);
    
    // Status text update
    char buf[32];
    sprintf(buf, "ON (%d%%)", channel_brightness[idx]);
    lv_label_set_text(home_status_labels[idx], buf);
    
    // Heat color mapping for brightness logo (gradient yellow)
    lv_obj_set_style_text_color(home_logo_labels[idx], lv_color_hex(0xFFDD6B), LV_PART_MAIN);
  } else {
    // OFF State Styling (Sleek Slate Grey Card 0x2E3238)
    lv_obj_set_style_bg_color(home_cards[idx], lv_color_hex(0x2E3238), LV_PART_MAIN);
    lv_label_set_text(home_status_labels[idx], "OFF");
    lv_obj_set_style_text_color(home_logo_labels[idx], lv_color_hex(0x64748B), LV_PART_MAIN); // Slate Gray
  }
  
  // Set selected symbol
  lv_label_set_text(home_logo_labels[idx], selectable_logos[channel_logos[idx]]);
}

// Helper to set config page highlights
void highlight_config_options() {
  auto set_btn_active = [](lv_obj_t* btn, bool active) {
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? 0x4A6984 : 0x2E3238), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(active ? 0x6080A0 : 0x3E444D), LV_PART_MAIN);
  };

  int ch = config_channel;
  int type = channel_types[ch - 1];
  int logo = channel_logos[ch - 1];

  set_btn_active(btn_mode_dimmer, type == 1);
  set_btn_active(btn_mode_switch, type == 2);

  for (int i = 0; i < 8; i++) {
    set_btn_active(btn_logos[i], logo == i);
  }

  // Dynamic configuration title label
  char title_buf[64];
  sprintf(title_buf, "Configure: %s", channel_names[ch - 1]);
  lv_label_set_text(config_title_label, title_buf);
}

// Helper to load settings page values
void refresh_settings_slider_values() {
  int to_val = screen_timeout_s;
  int idx = 3; // default 1m
  if (to_val == 5) idx = 0;
  else if (to_val == 15) idx = 1;
  else if (to_val == 30) idx = 2;
  else if (to_val == 60) idx = 3;
  else if (to_val == 180) idx = 4;
  else if (to_val == 300) idx = 5;
  else if (to_val == 600) idx = 6;
  
  lv_slider_set_value(timeout_slider, idx, LV_ANIM_OFF);
  
  const char* labels[] = {
    "Screen Timeout: 5s", "Screen Timeout: 15s", "Screen Timeout: 30s", 
    "Screen Timeout: 1m", "Screen Timeout: 3m", "Screen Timeout: 5m", "Screen Timeout: 10m"
  };
  lv_label_set_text(timeout_label, labels[idx]);
}

// --- EVENT HANDLERS ---

// Home Page Settings Intersect Button Click
static void open_settings_cb(lv_event_t* e) {
  refresh_settings_slider_values();
  load_screen(scr_settings);
}

// Main Cards Action CB (Click toggles, Long press configures)
static void card_event_cb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  active_channel = idx + 1;
  
  lv_event_code_t code = lv_event_get_code(e);
  
  if (code == LV_EVENT_PRESSED) {
    channel_long_pressed[idx] = false;
  }
  
  if (code == LV_EVENT_CLICKED) {
    if (!channel_long_pressed[idx]) {
      // Toggle card ON/OFF state
      channel_states[idx] = !channel_states[idx];
      if (channel_states[idx] && channel_brightness[idx] == 0) {
        channel_brightness[idx] = 100;
      }
      refresh_home_card(idx);
    }
  } 
  else if (code == LV_EVENT_LONG_PRESSED) {
    channel_long_pressed[idx] = true;
    
    // Check channel mode type
    if (channel_types[idx] == 1) {
      // Dimmer Slider page
      char title_buf[64];
      sprintf(title_buf, "Control: %s", channel_names[idx]);
      lv_label_set_text(dimmer_title_label, title_buf);
      lv_label_set_text(dimmer_logo_label, selectable_logos[channel_logos[idx]]);
      
      lv_slider_set_value(brightness_slider, channel_brightness[idx], LV_ANIM_OFF);
      lv_slider_set_value(tone_slider, map(channel_temp[idx], 153, 500, 0, 100), LV_ANIM_OFF);
      
      load_screen(scr_dimmer);
    } else {
      // Switch Toggle page
      lv_label_set_text(switch_title_label, channel_names[idx]);
      lv_label_set_text(switch_page_logo_label, selectable_logos[channel_logos[idx]]);
      
      if (channel_states[idx]) {
        lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2C4022), LV_PART_MAIN);
        lv_label_set_text(switch_toggle_label, "ACTIVE");
        lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0xFFDD6B), LV_PART_MAIN);
      } else {
        lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2E3238), LV_PART_MAIN);
        lv_label_set_text(switch_toggle_label, "INACTIVE");
        lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0x64748B), LV_PART_MAIN);
      }
      
      load_screen(scr_switch);
    }
  }
}

// Dimmer sliders callbacks
static void dimmer_slider_cb(lv_event_t* e) {
  lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
  int is_brightness = (int)(intptr_t)lv_event_get_user_data(e);
  int ch_idx = active_channel - 1;

  if (is_brightness) {
    channel_brightness[ch_idx] = lv_slider_get_value(slider);
    channel_states[ch_idx] = (channel_brightness[ch_idx] > 0);
  } else {
    int val = lv_slider_get_value(slider);
    channel_temp[ch_idx] = map(val, 0, 100, 153, 500);
  }
  
  refresh_home_card(ch_idx);
}

// Switch Page Big Button toggle callback
static void switch_toggle_cb(lv_event_t* e) {
  int ch_idx = active_channel - 1;
  channel_states[ch_idx] = !channel_states[ch_idx];
  
  if (channel_states[ch_idx]) {
    lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2C4022), LV_PART_MAIN);
    lv_label_set_text(switch_toggle_label, "ACTIVE");
    lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0xFFDD6B), LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2E3238), LV_PART_MAIN);
    lv_label_set_text(switch_toggle_label, "INACTIVE");
    lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0x64748B), LV_PART_MAIN);
  }
  
  refresh_home_card(ch_idx);
}

// Settings Screen sliders callbacks
static void settings_timeout_cb(lv_event_t* e) {
  lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
  int val = lv_slider_get_value(slider);
  
  int steps[] = {5, 15, 30, 60, 180, 300, 600};
  const char* labels[] = {
    "Screen Timeout: 5s", "Screen Timeout: 15s", "Screen Timeout: 30s", 
    "Screen Timeout: 1m", "Screen Timeout: 3m", "Screen Timeout: 5m", "Screen Timeout: 10m"
  };
  
  screen_timeout_s = steps[val];
  lv_label_set_text(timeout_label, labels[val]);
}

static void settings_backlight_cb(lv_event_t* e) {
  lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
  int val = lv_slider_get_value(slider);
  
  // Scale 10-100 down to PWM duty cycle (25 to 255)
  int duty = map(val, 10, 100, 25, 255);
  set_backlight(duty);
}

// Dynamic Config Screen Picker Event Routing
static void open_config_cb(lv_event_t* e) {
  config_channel = active_channel;
  highlight_config_options();
  load_screen(scr_config);
}

static void config_back_cb(lv_event_t* e) {
  // Route back dynamically based on configuration selected type mode
  int type = channel_types[config_channel - 1];
  if (type == 1) {
    load_screen(scr_dimmer);
  } else {
    // Sync Switch UI states
    int idx = config_channel - 1;
    lv_label_set_text(switch_title_label, channel_names[idx]);
    lv_label_set_text(switch_page_logo_label, selectable_logos[channel_logos[idx]]);
    if (channel_states[idx]) {
      lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2C4022), LV_PART_MAIN);
      lv_label_set_text(switch_toggle_label, "ACTIVE");
      lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0xFFDD6B), LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2E3238), LV_PART_MAIN);
      lv_label_set_text(switch_toggle_label, "INACTIVE");
      lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0x64748B), LV_PART_MAIN);
    }
    load_screen(scr_switch);
  }
}

// Config Screen selection edits
static void config_mode_dimmer_cb(lv_event_t* e) {
  channel_types[config_channel - 1] = 1; // Dimmer
  highlight_config_options();
}

static void config_mode_switch_cb(lv_event_t* e) {
  channel_types[config_channel - 1] = 2; // Switch
  highlight_config_options();
}

static void config_logo_select_cb(lv_event_t* e) {
  int logo_idx = (int)(intptr_t)lv_event_get_user_data(e);
  channel_logos[config_channel - 1] = logo_idx;
  highlight_config_options();
  
  // Sync immediately
  refresh_home_card(config_channel - 1);
}

// General BACK transitions
static void back_to_home_cb(lv_event_t* e) {
  load_screen(scr_home);
}

static void wifi_setup_btn_cb(lv_event_t* e) {
  load_screen(scr_wifi_hotspot);
}

static void wifi_hotspot_back_cb(lv_event_t* e) {
  load_screen(scr_settings);
}

// --- UI GENERATOR FUNCTIONS ---

// Home Page Builder
void build_home_screen() {
  scr_home = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_home, lv_color_hex(0x1A1D20), LV_PART_MAIN); // Symmetrical Dark Gray 0x1A1D20

  // 1. Create the 4 Cards positioned EXACTLY at their symmetrical coordinate bounds
  int card_w = 150;
  int card_h = 110;
  
  int coords_x[] = {5, 165, 5, 165};
  int coords_y[] = {5, 5, 125, 125};

  for (int i = 0; i < 4; i++) {
    home_cards[i] = lv_btn_create(scr_home);
    lv_obj_set_size(home_cards[i], card_w, card_h);
    lv_obj_set_pos(home_cards[i], coords_x[i], coords_y[i]);
    
    // Style Cards (Radius 12, borders 0x3E444D)
    lv_obj_set_style_radius(home_cards[i], 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(home_cards[i], lv_color_hex(0x2E3238), LV_PART_MAIN);
    lv_obj_set_style_border_width(home_cards[i], 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(home_cards[i], lv_color_hex(0x3E444D), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(home_cards[i], 0, LV_PART_MAIN);
    
    lv_obj_add_event_cb(home_cards[i], card_event_cb, LV_EVENT_ALL, (void*)(intptr_t)i);

    // Channel Name Label (Card Symmetrical alignment)
    lv_obj_t* name_lbl = lv_label_create(home_cards[i]);
    lv_label_set_text(name_lbl, channel_names[i]);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // style_title_text
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, LV_PART_MAIN);
    
    // Left column cards align text left; right column cards align text right
    if (i == 0 || i == 2) {
      lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, -2, -2);
    } else {
      lv_obj_align(name_lbl, LV_ALIGN_TOP_RIGHT, 2, -2);
    }

    // Large Center-Aligned Logo Label (offset symmetrically)
    home_logo_labels[i] = lv_label_create(home_cards[i]);
    lv_obj_set_style_text_font(home_logo_labels[i], &lv_font_montserrat_24, LV_PART_MAIN); // style_icon_large
    
    // Symmetrical center offsets matching the YAML: (Left cards offset x=43; right cards offset x=-43)
    int y_offset = (i >= 2) ? 5 : 0;
    if (i == 0 || i == 2) {
      lv_obj_align(home_logo_labels[i], LV_ALIGN_CENTER, 43, y_offset);
    } else {
      lv_obj_align(home_logo_labels[i], LV_ALIGN_CENTER, -43, y_offset);
    }

    // Status Label (Card Symmetrical bottom alignment)
    home_status_labels[i] = lv_label_create(home_cards[i]);
    lv_obj_set_style_text_color(home_status_labels[i], lv_color_hex(0xCBD5E1), LV_PART_MAIN); // style_normal_text
    lv_obj_set_style_text_font(home_status_labels[i], &lv_font_montserrat_12, LV_PART_MAIN);
    
    if (i == 0 || i == 2) {
      lv_obj_align(home_status_labels[i], LV_ALIGN_BOTTOM_LEFT, -2, 2);
    } else {
      lv_obj_align(home_status_labels[i], LV_ALIGN_BOTTOM_RIGHT, 2, 2);
    }

    // Apply states
    refresh_home_card(i);
  }

  // 2. Layered Floating Center Settings/Clock Button (Exactly x: 125, y: 85, width: 70, height: 70)
  btn_settings = lv_btn_create(scr_home);
  lv_obj_set_size(btn_settings, 70, 70);
  lv_obj_set_pos(btn_settings, 125, 85);
  
  // Style circular button (Radius 35, blue background 0x4A6984, border-width 5, border-color 0x1A1D20)
  lv_obj_set_style_radius(btn_settings, 35, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x4A6984), LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_settings, 5, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_settings, lv_color_hex(0x1A1D20), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_settings, 0, LV_PART_MAIN);
  
  lv_obj_add_event_cb(btn_settings, open_settings_cb, LV_EVENT_CLICKED, NULL);

  // local time clock display inside circle
  settings_time_label = lv_label_create(btn_settings);
  lv_label_set_text(settings_time_label, "12:00");
  lv_obj_set_style_text_color(settings_time_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN); // style_normal_text
  lv_obj_set_style_text_font(settings_time_label, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_center(settings_time_label);
}

// Dimmer Slider Controls Page Builder
void build_dimmer_screen() {
  scr_dimmer = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_dimmer, lv_color_hex(0x1A1D20), LV_PART_MAIN);

  // BACK button (x:10, y:10, width:60, height:35)
  lv_obj_t* btn_back = lv_btn_create(scr_dimmer);
  lv_obj_set_size(btn_back, 60, 35);
  lv_obj_set_pos(btn_back, 10, 10);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
  
  lv_obj_t* label_back = lv_label_create(btn_back);
  lv_label_set_text(label_back, "BACK");
  lv_obj_set_style_text_font(label_back, &lv_font_montserrat_12, LV_PART_MAIN); // style_small_text
  lv_obj_set_style_text_color(label_back, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_back);
  lv_obj_add_event_cb(btn_back, back_to_home_cb, LV_EVENT_CLICKED, NULL);

  // CONFIG button (x:250, y:10, width:60, height:35)
  lv_obj_t* btn_config = lv_btn_create(scr_dimmer);
  lv_obj_set_size(btn_config, 60, 35);
  lv_obj_set_pos(btn_config, 250, 10);
  lv_obj_set_style_bg_color(btn_config, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_config, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_config, 0, LV_PART_MAIN);
  
  lv_obj_t* label_config = lv_label_create(btn_config);
  lv_label_set_text(label_config, "CONFIG");
  lv_obj_set_style_text_font(label_config, &lv_font_montserrat_12, LV_PART_MAIN); // style_small_text
  lv_obj_set_style_text_color(label_config, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_config);
  lv_obj_add_event_cb(btn_config, open_config_cb, LV_EVENT_CLICKED, NULL);

  // Screen Title (x:85, y:15)
  dimmer_title_label = lv_label_create(scr_dimmer);
  lv_label_set_text(dimmer_title_label, "Control");
  lv_obj_set_pos(dimmer_title_label, 85, 15);
  lv_obj_set_style_text_color(dimmer_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // style_title_text
  lv_obj_set_style_text_font(dimmer_title_label, &lv_font_montserrat_16, LV_PART_MAIN);

  // Central Large logo display
  dimmer_logo_label = lv_label_create(scr_dimmer);
  lv_obj_set_style_text_font(dimmer_logo_label, &lv_font_montserrat_24, LV_PART_MAIN); // style_icon_large
  lv_obj_set_style_text_color(dimmer_logo_label, lv_color_hex(0xFFDD6B), LV_PART_MAIN);
  lv_obj_align(dimmer_logo_label, LV_ALIGN_TOP_MID, 0, 48);

  // --- Symmetrical Touch Sliders ---
  // 1. Brightness Section (Label y:60; Slider y:85, width:220, height:35)
  lv_obj_t* br_text = lv_label_create(scr_dimmer);
  lv_label_set_text(br_text, "Brightness");
  lv_obj_set_style_text_font(br_text, &lv_font_montserrat_12, LV_PART_MAIN); // style_normal_text
  lv_obj_set_style_text_color(br_text, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_pos(br_text, 50, 60);

  brightness_slider = lv_slider_create(scr_dimmer);
  lv_obj_set_size(brightness_slider, 220, 35);
  lv_obj_set_pos(brightness_slider, 50, 85);
  lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(brightness_slider, lv_color_hex(0xD6E8FF), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(brightness_slider, LV_GRAD_DIR_HOR, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_100, LV_PART_MAIN);
  lv_obj_set_style_radius(brightness_slider, 8, LV_PART_MAIN);
  
  // Circular white knob (radius 12, transparent indicators)
  lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_radius(brightness_slider, 12, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(brightness_slider, LV_OPA_0, LV_PART_INDICATOR);
  
  lv_obj_add_event_cb(brightness_slider, dimmer_slider_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)1);

  // 2. Color Tone Section (Label y:140; Slider y:165, width:220, height:35)
  lv_obj_t* tn_text = lv_label_create(scr_dimmer);
  lv_label_set_text(tn_text, "Color Temperature (Tone)");
  lv_obj_set_style_text_font(tn_text, &lv_font_montserrat_12, LV_PART_MAIN); // style_normal_text
  lv_obj_set_style_text_color(tn_text, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_pos(tn_text, 50, 140);

  tone_slider = lv_slider_create(scr_dimmer);
  lv_obj_set_size(tone_slider, 220, 35);
  lv_obj_set_pos(tone_slider, 50, 165);
  
  // Custom yellow-to-blue color gradient background
  lv_obj_set_style_bg_color(tone_slider, lv_color_hex(0xFFFF00), LV_PART_MAIN); // Yellow
  lv_obj_set_style_bg_grad_color(tone_slider, lv_color_hex(0x00A5FF), LV_PART_MAIN); // Blue
  lv_obj_set_style_bg_grad_dir(tone_slider, LV_GRAD_DIR_HOR, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tone_slider, LV_OPA_100, LV_PART_MAIN);
  lv_obj_set_style_radius(tone_slider, 8, LV_PART_MAIN);
  
  // Circular white knob
  lv_obj_set_style_bg_color(tone_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_radius(tone_slider, 12, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(tone_slider, LV_OPA_0, LV_PART_INDICATOR);
  
  lv_obj_add_event_cb(tone_slider, dimmer_slider_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)0);
}

// Switch Page UI Builder
void build_switch_screen() {
  scr_switch = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_switch, lv_color_hex(0x1A1D20), LV_PART_MAIN);

  // Title Label (x:85, y:15)
  switch_title_label = lv_label_create(scr_switch);
  lv_label_set_text(switch_title_label, "Switch");
  lv_obj_set_pos(switch_title_label, 85, 15);
  lv_obj_set_style_text_color(switch_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // style_title_text
  lv_obj_set_style_text_font(switch_title_label, &lv_font_montserrat_16, LV_PART_MAIN);

  // BACK button (x:10, y:10, width:60, height:35)
  lv_obj_t* btn_back = lv_btn_create(scr_switch);
  lv_obj_set_size(btn_back, 60, 35);
  lv_obj_set_pos(btn_back, 10, 10);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
  
  lv_obj_t* label_back = lv_label_create(btn_back);
  lv_label_set_text(label_back, "BACK");
  lv_obj_set_style_text_font(label_back, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(label_back, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_back);
  lv_obj_add_event_cb(btn_back, back_to_home_cb, LV_EVENT_CLICKED, NULL);

  // CONFIG button (x:250, y:10, width:60, height:35)
  lv_obj_t* btn_config = lv_btn_create(scr_switch);
  lv_obj_set_size(btn_config, 60, 35);
  lv_obj_set_pos(btn_config, 250, 10);
  lv_obj_set_style_bg_color(btn_config, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_config, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_config, 0, LV_PART_MAIN);
  
  lv_obj_t* label_config = lv_label_create(btn_config);
  lv_label_set_text(label_config, "CONFIG");
  lv_obj_set_style_text_font(label_config, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(label_config, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_config);
  lv_obj_add_event_cb(btn_config, open_config_cb, LV_EVENT_CLICKED, NULL);

  // Large Symmetrical Toggle Switch Button (x:80, y:70, width:160, height:120, radius 20)
  btn_switch_toggle = lv_btn_create(scr_switch);
  lv_obj_set_size(btn_switch_toggle, 160, 120);
  lv_obj_set_pos(btn_switch_toggle, 80, 70);
  lv_obj_set_style_radius(btn_switch_toggle, 20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn_switch_toggle, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_switch_toggle, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_switch_toggle, lv_color_hex(0x3E444D), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_switch_toggle, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn_switch_toggle, switch_toggle_cb, LV_EVENT_CLICKED, NULL);

  // Centered Large Icon inside toggle
  switch_page_logo_label = lv_label_create(btn_switch_toggle);
  lv_obj_set_style_text_font(switch_page_logo_label, &lv_font_montserrat_24, LV_PART_MAIN); // Large font
  lv_obj_set_style_text_color(switch_page_logo_label, lv_color_hex(0x64748B), LV_PART_MAIN);
  lv_obj_align(switch_page_logo_label, LV_ALIGN_CENTER, 0, -20);

  // Underneath active state description text label
  switch_toggle_label = lv_label_create(btn_switch_toggle);
  lv_label_set_text(switch_toggle_label, "INACTIVE");
  lv_obj_set_style_text_font(switch_toggle_label, &lv_font_montserrat_12, LV_PART_MAIN); // style_normal_text
  lv_obj_set_style_text_color(switch_toggle_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_align(switch_toggle_label, LV_ALIGN_CENTER, 0, 25);
}

// 2-Column Symmetrical Settings Screen Builder
void build_settings_screen() {
  scr_settings = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_settings, lv_color_hex(0x1A1D20), LV_PART_MAIN);

  // Header Title (x:85, y:15)
  lv_obj_t* title = lv_label_create(scr_settings);
  lv_label_set_text(title, "Settings");
  lv_obj_set_pos(title, 85, 15);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);

  // BACK button (x:10, y:10, width:60, height:35)
  lv_obj_t* btn_back = lv_btn_create(scr_settings);
  lv_obj_set_size(btn_back, 60, 35);
  lv_obj_set_pos(btn_back, 10, 10);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
  
  lv_obj_t* label_back = lv_label_create(btn_back);
  lv_label_set_text(label_back, "BACK");
  lv_obj_set_style_text_font(label_back, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(label_back, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_back);
  lv_obj_add_event_cb(btn_back, back_to_home_cb, LV_EVENT_CLICKED, NULL);

  // WIFI SETUP Button on Header Right (x:200, y:10, width:110, height:35)
  lv_obj_t* btn_wifi = lv_btn_create(scr_settings);
  lv_obj_set_size(btn_wifi, 110, 35);
  lv_obj_set_pos(btn_wifi, 200, 10);
  lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_wifi, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_wifi, 0, LV_PART_MAIN);
  
  lv_obj_t* label_wifi = lv_label_create(btn_wifi);
  lv_label_set_text(label_wifi, "WIFI SETUP");
  lv_obj_set_style_text_font(label_wifi, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(label_wifi, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_wifi);
  lv_obj_add_event_cb(btn_wifi, wifi_setup_btn_cb, LV_EVENT_CLICKED, NULL);

  // Left Column Info Labels (y:50 and y:75)
  settings_ssid_label = lv_label_create(scr_settings);
  lv_label_set_text(settings_ssid_label, "SSID: CYD-Lights-AP");
  lv_obj_set_pos(settings_ssid_label, 20, 50);
  lv_obj_set_style_text_color(settings_ssid_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(settings_ssid_label, &lv_font_montserrat_12, LV_PART_MAIN);

  settings_ip_label = lv_label_create(scr_settings);
  lv_label_set_text(settings_ip_label, "IP: 192.168.4.1");
  lv_obj_set_pos(settings_ip_label, 20, 75);
  lv_obj_set_style_text_color(settings_ip_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(settings_ip_label, &lv_font_montserrat_12, LV_PART_MAIN);

  // Right Column Info Labels (y:50 and y:75)
  settings_wifi_signal_label = lv_label_create(scr_settings);
  lv_label_set_text(settings_wifi_signal_label, "Signal:");
  lv_obj_set_pos(settings_wifi_signal_label, 170, 50);
  lv_obj_set_style_text_color(settings_wifi_signal_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(settings_wifi_signal_label, &lv_font_montserrat_12, LV_PART_MAIN);

  settings_ldr_label = lv_label_create(scr_settings);
  lv_label_set_text(settings_ldr_label, "Ambient: 1.25V");
  lv_obj_set_pos(settings_ldr_label, 170, 75);
  lv_obj_set_style_text_color(settings_ldr_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(settings_ldr_label, &lv_font_montserrat_12, LV_PART_MAIN);

  // Symmetrical Multi-Height Wifi signal bars
  int bar_x[] = {235, 241, 247, 253};
  int bar_y[] = {59, 54, 49, 44};
  int bar_h[] = {6, 11, 16, 21};
  for (int i = 0; i < 4; i++) {
    wifi_bars[i] = lv_obj_create(scr_settings);
    lv_obj_set_size(wifi_bars[i], 4, bar_h[i]);
    lv_obj_set_pos(wifi_bars[i], bar_x[i], bar_y[i]);
    lv_obj_set_style_bg_color(wifi_bars[i], lv_color_hex(0x4A6984), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_bars[i], LV_OPA_100, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_bars[i], 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_bars[i], 1, LV_PART_MAIN);
  }

  // --- Settings Slider Controls ---
  
  // 1. Screen Timeout (Label x:50, y:105; Slider x:50, y:128, width:220, height:25)
  timeout_label = lv_label_create(scr_settings);
  lv_label_set_text(timeout_label, "Screen Timeout: 1m");
  lv_obj_set_pos(timeout_label, 50, 105);
  lv_obj_set_style_text_color(timeout_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(timeout_label, &lv_font_montserrat_12, LV_PART_MAIN);

  timeout_slider = lv_slider_create(scr_settings);
  lv_obj_set_size(timeout_slider, 220, 25);
  lv_obj_set_pos(timeout_slider, 50, 128);
  lv_obj_set_style_bg_color(timeout_slider, lv_color_hex(0x141619), LV_PART_MAIN);
  lv_obj_set_style_radius(timeout_slider, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(timeout_slider, lv_color_hex(0x6080A0), LV_PART_INDICATOR);
  lv_slider_set_range(timeout_slider, 0, 6);
  lv_slider_set_value(timeout_slider, 3, LV_ANIM_OFF); // Default 1m
  lv_obj_add_event_cb(timeout_slider, settings_timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // 2. Backlight Override (Label x:50, y:160; Slider x:50, y:183, width:220, height:25)
  lv_obj_t* bl_label = lv_label_create(scr_settings);
  lv_label_set_text(bl_label, "Display Backlight");
  lv_obj_set_pos(bl_label, 50, 160);
  lv_obj_set_style_text_color(bl_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(bl_label, &lv_font_montserrat_12, LV_PART_MAIN);

  backlight_override_slider = lv_slider_create(scr_settings);
  lv_obj_set_size(backlight_override_slider, 220, 25);
  lv_obj_set_pos(backlight_override_slider, 50, 183);
  lv_obj_set_style_bg_color(backlight_override_slider, lv_color_hex(0x141619), LV_PART_MAIN);
  lv_obj_set_style_radius(backlight_override_slider, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(backlight_override_slider, lv_color_hex(0x6080A0), LV_PART_INDICATOR);
  lv_slider_set_range(backlight_override_slider, 10, 100);
  lv_slider_set_value(backlight_override_slider, 80, LV_ANIM_OFF); // Default 80%
  lv_obj_add_event_cb(backlight_override_slider, settings_backlight_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// Config Screen Builder (Icon & Mode Select Grid Page)
void build_config_screen() {
  scr_config = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_config, lv_color_hex(0x1A1D20), LV_PART_MAIN);

  // Title (x:85, y:15)
  config_title_label = lv_label_create(scr_config);
  lv_label_set_text(config_title_label, "Configure Channel");
  lv_obj_set_pos(config_title_label, 85, 15);
  lv_obj_set_style_text_color(config_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(config_title_label, &lv_font_montserrat_16, LV_PART_MAIN);

  // BACK button (x:10, y:10, width:60, height:35)
  lv_obj_t* btn_back = lv_btn_create(scr_config);
  lv_obj_set_size(btn_back, 60, 35);
  lv_obj_set_pos(btn_back, 10, 10);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
  
  lv_obj_t* label_back = lv_label_create(btn_back);
  lv_label_set_text(label_back, "BACK");
  lv_obj_set_style_text_font(label_back, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(label_back, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_back);
  lv_obj_add_event_cb(btn_back, config_back_cb, LV_EVENT_CLICKED, NULL);

  // 1. Channel Mode section (Label x:20, y:55; Buttons y:75, width:135, height:35)
  lv_obj_t* mode_lbl = lv_label_create(scr_config);
  lv_label_set_text(mode_lbl, "Channel Mode");
  lv_obj_set_pos(mode_lbl, 20, 55);
  lv_obj_set_style_text_color(mode_lbl, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_12, LV_PART_MAIN);

  btn_mode_dimmer = lv_btn_create(scr_config);
  lv_obj_set_size(btn_mode_dimmer, 135, 35);
  lv_obj_set_pos(btn_mode_dimmer, 20, 75);
  lv_obj_set_style_radius(btn_mode_dimmer, 8, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_mode_dimmer, 1, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_mode_dimmer, 0, LV_PART_MAIN);
  
  lv_obj_t* lbl_dimmer = lv_label_create(btn_mode_dimmer);
  lv_label_set_text(lbl_dimmer, "Dimmer (Sliders)");
  lv_obj_set_style_text_font(lbl_dimmer, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_center(lbl_dimmer);
  lv_obj_add_event_cb(btn_mode_dimmer, config_mode_dimmer_cb, LV_EVENT_CLICKED, NULL);

  btn_mode_switch = lv_btn_create(scr_config);
  lv_obj_set_size(btn_mode_switch, 135, 35);
  lv_obj_set_pos(btn_mode_switch, 165, 75);
  lv_obj_set_style_radius(btn_mode_switch, 8, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_mode_switch, 1, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_mode_switch, 0, LV_PART_MAIN);
  
  lv_obj_t* lbl_switch = lv_label_create(btn_mode_switch);
  lv_label_set_text(lbl_switch, "Switch (Toggle)");
  lv_obj_set_style_text_font(lbl_switch, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_center(lbl_switch);
  lv_obj_add_event_cb(btn_mode_switch, config_mode_switch_cb, LV_EVENT_CLICKED, NULL);

  // 2. Logo selector section (Label x:20, y:125; Buttons width 60, height 35)
  lv_obj_t* logo_sec_lbl = lv_label_create(scr_config);
  lv_label_set_text(logo_sec_lbl, "Logo");
  lv_obj_set_pos(logo_sec_lbl, 20, 125);
  lv_obj_set_style_text_color(logo_sec_lbl, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(logo_sec_lbl, &lv_font_montserrat_12, LV_PART_MAIN);

  int start_x = 22;
  int gap_x = 12;
  int start_y = 150;
  int gap_y = 8;
  int btn_w = 60;
  int btn_h = 35;

  for (int i = 0; i < 8; i++) {
    int col = i % 4;
    int row = i / 4;
    int x_pos = start_x + col * (btn_w + gap_x);
    int y_pos = start_y + row * (btn_h + gap_y);

    btn_logos[i] = lv_btn_create(scr_config);
    lv_obj_set_size(btn_logos[i], btn_w, btn_h);
    lv_obj_set_pos(btn_logos[i], x_pos, y_pos);
    lv_obj_set_style_radius(btn_logos[i], 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_logos[i], 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_logos[i], 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_logos[i], config_logo_select_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* lbl_icon = lv_label_create(btn_logos[i]);
    lv_label_set_text(lbl_icon, selectable_logos[i]);
    lv_obj_set_style_text_font(lbl_icon, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_icon, lv_color_hex(0xFFDD6B), LV_PART_MAIN);
    lv_obj_center(lbl_icon);
  }
}

// WIFI Hotspot Instructions Page Builder
void build_wifi_hotspot_screen() {
  scr_wifi_hotspot = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_wifi_hotspot, lv_color_hex(0x1A1D20), LV_PART_MAIN);

  // Title (x:95, y:15)
  lv_obj_t* title = lv_label_create(scr_wifi_hotspot);
  lv_label_set_text(title, "WiFi Setup Hotspot");
  lv_obj_set_pos(title, 95, 15);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);

  // BACK button (x:10, y:10, width:60, height:35)
  lv_obj_t* btn_back = lv_btn_create(scr_wifi_hotspot);
  lv_obj_set_size(btn_back, 60, 35);
  lv_obj_set_pos(btn_back, 10, 10);
  lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2E3238), LV_PART_MAIN);
  lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
  
  lv_obj_t* label_back = lv_label_create(btn_back);
  lv_label_set_text(label_back, "BACK");
  lv_obj_set_style_text_font(label_back, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(label_back, lv_color_hex(0x94A3B8), LV_PART_MAIN);
  lv_obj_center(label_back);
  lv_obj_add_event_cb(btn_back, wifi_hotspot_back_cb, LV_EVENT_CLICKED, NULL);

  // Symmetrical Instructions Labels (x:20, y:65)
  lv_obj_t* instr = lv_label_create(scr_wifi_hotspot);
  lv_label_set_text(instr, "1. Connect your phone/PC to WiFi:\n   SSID: CYD-Lights-AP\n   Password: password123\n\n2. Open your web browser to:\n   http://192.168.4.1\n\n3. Select your network & connect!");
  lv_obj_set_pos(instr, 20, 65);
  lv_obj_set_style_text_color(instr, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_text_font(instr, &lv_font_montserrat_12, LV_PART_MAIN);
}

// --- SETUP & HARDWARE INITIALIZATION ---
void setup() {
  Serial.begin(115200);
  Serial.println("Starting Cheap Yellow Display Standalone 1:1 Dashboard...");

  // Setup Backlight PWM: Compatible with ESP32 core v2.x and v3.x
  #if defined(ESP32)
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
      ledcAttach(BACKLIGHT_PIN, 5000, 8);
      ledcWrite(BACKLIGHT_PIN, 200); // 80% default brightness
    #else
      ledcSetup(0, 5000, 8);
      ledcAttachPin(BACKLIGHT_PIN, 0);
      ledcWrite(0, 200);
    #endif
  #else
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, HIGH);
  #endif

  // Setup Onboard LDR Pin
  pinMode(LDR_PIN, INPUT);

  // Initialize display SPI
  tft.begin();
  tft.setRotation(2); // 180° orientation rotation (corresponds to ESPHome rotation 180)
  tft.fillScreen(TFT_BLACK);

  // Initialize Touch Screen SPI on secondary bus (SCLK=25, MISO=39, MOSI=32, CS=33)
  // Re-route multiplexed pins for resistive touch hardware
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSPI);
  touch.setRotation(2); // remap touch rotation to match display

  // --- DUAL VERSION DISPLAY/TOUCH INITIALIZATION ---
  
  #if LVGL_VERSION_MAJOR >= 9
    // LVGL v9 Initialization
    lv_init();
    disp = lv_display_create(screenWidth, screenHeight);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);
  #else
    // LVGL v8 Initialization
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
  #endif

  // --- BUILD SCREENS ---
  build_home_screen();
  build_dimmer_screen();
  build_switch_screen();
  build_settings_screen();
  build_config_screen();
  build_wifi_hotspot_screen();

  // Load the initial home screen
  lv_scr_load(scr_home);

  last_activity_time = millis();
}

// --- MAIN STANDALONE LOOP ---
void loop() {
  // Feed the elapsed time to the LVGL internal layout engine (CRITICAL for button click processing!)
  static uint32_t last_tick = 0;
  uint32_t elapsed = millis() - last_tick;
  last_tick = millis();
  lv_tick_inc(elapsed);

  // Always trigger LVGL timer loop tick
  lv_timer_handler();
  delay(5);

  // Touch Diagnostics Serial Monitor Output (Polls every 100ms when pressed)
  static uint32_t last_touch_print = 0;
  if (millis() - last_touch_print > 100) {
    if (touch.touched()) {
      last_touch_print = millis();
      TS_Point p = touch.getPoint();
      Serial.printf("[TOUCH DEBUG] Raw X: %d | Raw Y: %d | Pressure Z: %d\n", p.x, p.y, p.z);
    }
  }

  // Onboard LDR Sensor Reading (update settings page label every 2s)
  static uint32_t last_ldr_check = 0;
  if (millis() - last_ldr_check > 2000) {
    last_ldr_check = millis();
    int ldr_raw = analogRead(LDR_PIN);
    float volt = (ldr_raw / 4095.0f) * 3.3f;
    
    char ldr_buf[32];
    sprintf(ldr_buf, "Ambient: %.2fV", volt);
    if (settings_ldr_label != NULL) {
      lv_label_set_text(settings_ldr_label, ldr_buf);
    }
  }

  // Update time label in center settings button (Mock local running clock)
  static uint32_t last_time_update = 0;
  static int mock_hour = 12;
  static int mock_minute = 0;
  static int mock_second = 0;
  if (millis() - last_time_update > 1000) {
    last_time_update = millis();
    mock_second++;
    if (mock_second >= 60) {
      mock_second = 0;
      mock_minute++;
      if (mock_minute >= 60) {
        mock_minute = 0;
        mock_hour = (mock_hour + 1) % 24;
      }
    }
    char time_str[16];
    sprintf(time_str, "%02d:%02d", mock_hour, mock_minute);
    if (settings_time_label != NULL) {
      lv_label_set_text(settings_time_label, time_str);
    }
  }

  // Symmetrical Inactivity Screen Saver Timeout Check
  uint32_t elapsed_idle = (millis() - last_activity_time) / 1000;
  if (elapsed_idle >= screen_timeout_s && !screen_sleeping) {
    screen_sleeping = true;
    
    // Slow smooth fade out of backlight over 0.5s
    for (int i = 200; i >= 0; i -= 5) {
      set_backlight(i);
      delay(12);
    }
    
    Serial.println("Screen saver active (backlight OFF). Touch screen to wake!");
  }
}
