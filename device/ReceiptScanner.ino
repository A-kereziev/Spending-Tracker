/* Using LVGL with Arduino requires some extra steps:
 * https://docs.lvgl.io/master/get-started/platforms/arduino.html
 */

#include "esp_camera.h"
#define LV_CONF_PATH /home/sashi/Arduino/libraries/lv_conf.h
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "bsp_cst816.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// =============================================
// SERIAL MUTEX
// =============================================

SemaphoreHandle_t serialMutex = NULL;

void safePrint(const char *message) {
  if (serialMutex != NULL &&
      xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
    Serial.print(message);
    Serial.flush();
    xSemaphoreGive(serialMutex);
  }
}

// =============================================
// WIFI SETTINGS / UPLOAD FUNCTION
// =============================================

extern uint8_t *captured_photo;
extern size_t captured_photo_len;
extern volatile bool upload_in_progress;

void upload_receipt_task(void *param) {
  Serial.println("\nStarting receipt upload...");
  Serial.println("DIAGNOSTIC BUILD 002");
  Serial.flush();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload failed: Wi-Fi disconnected.");
    Serial.flush();
    upload_in_progress = false;
    vTaskDelete(NULL);
    return;
  }

  // Wait for valid system time for TLS certificate validation.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  time_t now = time(nullptr);
  int time_attempts = 0;

  while (now < 1760000000 && time_attempts < 30) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    now = time(nullptr);
    time_attempts++;
  }

  if (now < 1760000000) {
    Serial.println("Upload failed: clock synchronization timed out.");
    Serial.flush();
    upload_in_progress = false;
    vTaskDelete(NULL);
    return;
  }

  Serial.println("Clock synchronized; validating HTTPS certificate.");
  Serial.flush();

  if (!captured_photo || captured_photo_len == 0) {
    Serial.println("Upload failed: no captured JPEG.");
    Serial.flush();
    upload_in_progress = false;
    vTaskDelete(NULL);
    return;
  }

  WiFiClientSecure client;
  HTTPClient http;

  // Certificate verification remains enabled.
  static const char root_ca[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";

  client.setCACert(root_ca);

  http.setConnectTimeout(15000);
  http.setTimeout(120000);

  Serial.println("\n=== HTTPS DIAGNOSTICS ===");

  Serial.printf(
    "Internal free heap: %u bytes\n",
    (unsigned int)heap_caps_get_free_size(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  Serial.printf(
    "Largest internal free block: %u bytes\n",
    (unsigned int)heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  Serial.printf(
    "Free PSRAM: %u bytes\n",
    (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  Serial.printf(
    "Minimum free heap since boot: %u bytes\n",
    (unsigned int)ESP.getMinFreeHeap());

  Serial.printf("Wi-Fi status: %d\n", WiFi.status());

  Serial.printf(
    "Gateway: %s\n",
    WiFi.gatewayIP().toString().c_str());

  Serial.flush();

  IPAddress cloudflareIP;

  if (WiFi.hostByName(
        "rising-disclosure-insulin-tuning.trycloudflare.com",
        cloudflareIP)) {
    Serial.printf(
      "Cloudflare DNS: %s\n",
      cloudflareIP.toString().c_str());
  } else {
    Serial.println("Cloudflare DNS lookup FAILED!");
  }

  IPAddress resolvedIP;

  Serial.println("\n--- DNS TEST ---");
  Serial.print("DNS server: ");
  Serial.println(WiFi.dnsIP(0));

  int dnsResult = WiFi.hostByName(
    "rising-disclosure-insulin-tuning.trycloudflare.com",
    resolvedIP);

  Serial.print("DNS result: ");
  Serial.println(dnsResult);

  Serial.print("Resolved IP: ");
  Serial.println(resolvedIP);

  Serial.flush();

  // =============================================
  // TLS HANDSHAKE DIAGNOSTIC
  // =============================================

  Serial.println("\n--- Starting TLS handshake ---");
  Serial.flush();

  if (client.connect(
        "rising-disclosure-insulin-tuning.trycloudflare.com", 443)) {
    Serial.println("Raw TLS connection SUCCEEDED!");
    client.stop();
  } else {
    Serial.println("Raw TLS connection FAILED!");
    Serial.flush();

    char tls_error[256] = { 0 };
    int error_code = client.lastError(
      tls_error,
      sizeof(tls_error));

    Serial.println("--- TLS FAILURE DETAILS ---");
    Serial.printf("TLS error code: %d\n", error_code);
    Serial.printf("TLS error details: %s\n", tls_error);
    Serial.println("--- END TLS FAILURE DETAILS ---");
    Serial.flush();
  }

  // =============================================
  // RAW TCP DIAGNOSTIC
  // =============================================

  Serial.println("\n--- Testing raw TCP connection ---");
  Serial.flush();

  WiFiClient tcpClient;

  if (tcpClient.connect(
        "rising-disclosure-insulin-tuning.trycloudflare.com", 443)) {
    Serial.println("Raw TCP connection SUCCEEDED!");
    tcpClient.stop();
  } else {
    Serial.println("Raw TCP connection FAILED!");
  }

  Serial.flush();

  // =============================================
  // HTTPS POST
  // =============================================

  Serial.println("\n--- Starting HTTPS POST ---");
  Serial.flush();

  if (!http.begin(client, RECEIPT_API_URL)) {
    Serial.println("Upload failed: HTTPS initialisation failed.");
    Serial.flush();
    upload_in_progress = false;
    vTaskDelete(NULL);
    return;
  }

  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader(
    "Authorization",
    String("Bearer ") + RECEIPT_API_TOKEN);

  Serial.printf(
    "Uploading %u JPEG bytes...\n",
    (unsigned int)captured_photo_len);

  Serial.flush();

  Serial.printf(
    "JPEG signature: %02X %02X %02X %02X\n",
    captured_photo[0],
    captured_photo[1],
    captured_photo[2],
    captured_photo[3]);

  int response_code = http.POST(
    captured_photo,
    captured_photo_len);

  if (response_code > 0) {
    Serial.printf("Server HTTP status: %d\n", response_code);

    String response = http.getString();
    Serial.println(response);

    if (response_code == 200) {
      Serial.println(
        "Receipt uploaded and processed successfully!");
    } else {
      Serial.println(
        "Server rejected or failed to process the receipt.");
    }
  } else {
    Serial.printf(
      "HTTPS request failed: %s\n",
      http.errorToString(response_code).c_str());

    Serial.printf(
      "HTTPClient error code: %d\n",
      response_code);

    Serial.printf("Wi-Fi status: %d\n", WiFi.status());
  }

  http.end();

  Serial.println("=== END HTTPS DIAGNOSTICS ===");
  Serial.flush();

  upload_in_progress = false;
  vTaskDelete(NULL);
}

// =============================================
// CAMERA PIN DEFINITIONS
// WAVESHARE ESP32-S3-TOUCH-LCD-2
// =============================================

#define PWDN_GPIO_NUM 17
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 8
#define SIOD_GPIO_NUM 21
#define SIOC_GPIO_NUM 16

#define Y9_GPIO_NUM 2
#define Y8_GPIO_NUM 7
#define Y7_GPIO_NUM 10
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 11
#define Y4_GPIO_NUM 15
#define Y3_GPIO_NUM 13
#define Y2_GPIO_NUM 12
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 4
#define PCLK_GPIO_NUM 9

// =============================================
// LCD DISPLAY PIN DEFINITIONS
// =============================================

#define EXAMPLE_PIN_NUM_LCD_SCLK 39
#define EXAMPLE_PIN_NUM_LCD_MOSI 38
#define EXAMPLE_PIN_NUM_LCD_MISO 40
#define EXAMPLE_PIN_NUM_LCD_DC 42
#define EXAMPLE_PIN_NUM_LCD_RST -1
#define EXAMPLE_PIN_NUM_LCD_CS 45
#define EXAMPLE_PIN_NUM_LCD_BL 1
#define EXAMPLE_PIN_NUM_TP_SDA 48
#define EXAMPLE_PIN_NUM_TP_SCL 47

#define LEDC_FREQ 5000
#define LEDC_TIMER_10_BIT 10

#define EXAMPLE_LCD_ROTATION 0
#define EXAMPLE_LCD_H_RES 240
#define EXAMPLE_LCD_V_RES 320

// =============================================
// CAPTURED PHOTO STORAGE
// =============================================

#define PHOTO_BUFFER_SIZE (1024 * 1024)

uint8_t *captured_photo = nullptr;
size_t captured_photo_len = 0;

volatile bool capture_requested = false;
volatile bool capture_complete = false;
volatile bool capture_in_progress = false;
volatile bool upload_requested = false;
volatile bool upload_in_progress = false;

// =============================================
// DISPLAY SETUP
// =============================================

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  EXAMPLE_PIN_NUM_LCD_DC,
  EXAMPLE_PIN_NUM_LCD_CS,
  EXAMPLE_PIN_NUM_LCD_SCLK,
  EXAMPLE_PIN_NUM_LCD_MOSI,
  EXAMPLE_PIN_NUM_LCD_MISO);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus,
  EXAMPLE_PIN_NUM_LCD_RST,
  EXAMPLE_LCD_ROTATION,
  true,
  EXAMPLE_LCD_H_RES,
  EXAMPLE_LCD_V_RES);

uint32_t screenWidth;
uint32_t screenHeight;
uint32_t bufSize;

lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf;
lv_disp_drv_t disp_drv;

static SemaphoreHandle_t lvgl_api_mux = NULL;

lv_obj_t *img_camera;
lv_obj_t *capture_button;
lv_obj_t *status_label;

// =============================================
// LVGL LOCKING
// =============================================

bool lvgl_lock(int timeout_ms) {
  const TickType_t timeout_ticks =
    (timeout_ms == -1)
      ? portMAX_DELAY
      : pdMS_TO_TICKS(timeout_ms);

  return xSemaphoreTakeRecursive(
           lvgl_api_mux,
           timeout_ticks)
         == pdTRUE;
}

void lvgl_unlock(void) {
  xSemaphoreGiveRecursive(lvgl_api_mux);
}

#if LV_USE_LOG != 0
void my_print(const char *buf) {
  Serial.printf("%s", buf);
  Serial.flush();
}
#endif

// =============================================
// DISPLAY FLUSH
// =============================================

void my_disp_flush(
  lv_disp_drv_t *disp_drv,
  const lv_area_t *area,
  lv_color_t *color_p) {
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(
    0, 0,
    (uint16_t *)disp_draw_buf,
    screenWidth,
    screenHeight);
#else
  gfx->draw16bitRGBBitmap(
    0, 0,
    (uint16_t *)disp_draw_buf,
    screenWidth,
    screenHeight);
#endif

  lv_disp_flush_ready(disp_drv);
}

// =============================================
// TOUCHPAD INPUT
// =============================================

void my_touchpad_read(
  lv_indev_drv_t *indev_drv,
  lv_indev_data_t *data) {
  uint16_t touchpad_x;
  uint16_t touchpad_y;

  bsp_touch_read();

  if (bsp_touch_get_coordinates(
        &touchpad_x,
        &touchpad_y)) {
    data->point.x = touchpad_x;
    data->point.y = touchpad_y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// =============================================
// CAPTURE BUTTON EVENT
// =============================================

void capture_button_event(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    if (!capture_in_progress && !capture_requested) {
      capture_requested = true;

      Serial.println("Capture button pressed!");

      if (status_label) {
        lv_label_set_text(status_label, "Capturing...");
      }
    }
  }
}

// =============================================
// LVGL CAMERA UI
// =============================================

void lvgl_camera_ui_init(lv_obj_t *parent) {
  // Camera image
  img_camera = lv_img_create(parent);

  lv_obj_align(img_camera, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_pos(img_camera, -1, 0);
  lv_obj_set_scroll_dir(parent, LV_DIR_NONE);

  lv_obj_set_style_pad_top(img_camera, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(img_camera, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(img_camera, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_right(img_camera, 0, LV_PART_MAIN);

  // Status label
  status_label = lv_label_create(parent);
  lv_label_set_text(
    status_label,
    "Tap CAPTURE to scan receipt");

  lv_obj_set_style_text_color(
    status_label,
    lv_color_hex(0xFFFFFF),
    0);

  lv_obj_align(
    status_label,
    LV_ALIGN_TOP_MID,
    0,
    10);

  // Capture button
  capture_button = lv_btn_create(parent);

  lv_obj_set_size(capture_button, 140, 55);

  lv_obj_align(
    capture_button,
    LV_ALIGN_BOTTOM_MID,
    0,
    -15);

  lv_obj_add_event_cb(
    capture_button,
    capture_button_event,
    LV_EVENT_CLICKED,
    NULL);

  // Button styling
  lv_obj_set_style_bg_color(
    capture_button,
    lv_color_hex(0x4CAF50),
    0);

  lv_obj_set_style_border_width(
    capture_button,
    2,
    0);

  lv_obj_set_style_border_color(
    capture_button,
    lv_color_hex(0xFFFFFF),
    0);

  lv_obj_set_style_radius(
    capture_button,
    10,
    0);

  lv_obj_t *label = lv_label_create(capture_button);

  lv_label_set_text(label, "CAPTURE");

  lv_obj_set_style_text_color(
    label,
    lv_color_hex(0xFFFFFF),
    0);

  lv_obj_center(label);
}

// =============================================
// CAMERA INITIALIZATION
// =============================================

void init_camera() {
  if (!psramFound()) {
    Serial.println(
      "PSRAM NOT FOUND! Camera may not work.");
  } else {
    Serial.printf(
      "PSRAM found: Total PSRAM: %d bytes, Free PSRAM: %d bytes\n",
      ESP.getPsramSize(),
      ESP.getFreePsram());
  }

  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 10000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = 6;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  Serial.printf(
    "Heap BEFORE camera init: %u\n",
    ESP.getFreeHeap());

  esp_err_t err = esp_camera_init(&config);

  Serial.printf(
    "Heap AFTER camera init: %u\n",
    ESP.getFreeHeap());

  if (err != ESP_OK) {
    Serial.printf(
      "Camera init failed with error 0x%x\n",
      err);

    Serial.printf(
      "Free heap: %d bytes\n",
      ESP.getFreeHeap());

    Serial.printf(
      "Free PSRAM: %d bytes\n",
      ESP.getFreePsram());

    return;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (!s) {
    Serial.println("Failed to get sensor handle!");
    return;
  }

  Serial.printf("Camera PID: 0x%04X\n", s->id.PID);
  Serial.printf("Camera VER: 0x%02X\n", s->id.VER);
  Serial.printf("Camera MIDH: 0x%02X\n", s->id.MIDH);
  Serial.printf("Camera MIDL: 0x%02X\n", s->id.MIDL);

  Serial.printf(
    "Detected camera sensor PID: 0x%04X\n",
    s->id.PID);

  // Match the OV3660 settings from 09_lvgl_camera.ino.
// OV3660 image optimization
if (s->id.PID == OV3660_PID) {
    Serial.println("OV3660 detected! Applying optimized settings.");

    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);

    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, -2);
    s->set_sharpness(s, 2);

    s->set_whitebal(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
} else {
    Serial.println(
      "WARNING: OV3660 not detected; reference settings not applied.");

    Serial.println(
      "Check the camera PID before proceeding.");
  }

  Serial.println("Camera settings applied!");
}

// =============================================
// CAMERA TASK
// Only this task calls esp_camera_fb_get().
// =============================================

static void camera_task(void *param) {
  init_camera();

  vTaskDelay(pdMS_TO_TICKS(500));

  camera_fb_t *pic;
  camera_fb_t *capture_frame;

  // Preview buffer: test pattern for display.
  uint8_t *preview_buffer =
    (uint8_t *)ps_malloc(240 * 240 * 2);

  Serial.printf(
    "Heap after preview buffer: %u\n",
    ESP.getFreeHeap());

  Serial.printf(
    "Largest heap block: %u\n",
    ESP.getMaxAllocHeap());

  Serial.printf(
    "Free PSRAM: %u\n",
    ESP.getFreePsram());

  if (!preview_buffer) {
    Serial.println("Failed to allocate preview buffer!");

    while (1) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  // Fill preview buffer with test pattern.
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      int idx = (y * 240 + x) * 2;

      uint8_t r = (x * 255) / 240;
      uint8_t g = (y * 255) / 240;
      uint8_t b = 128;

      preview_buffer[idx] =
        ((g & 0x1C) << 3) | (b >> 3);

      preview_buffer[idx + 1] =
        (r & 0xF8) | (g >> 5);
    }
  }

  lv_img_dsc_t img_dsc;

  img_dsc.header.always_zero = 0;
  img_dsc.header.w = 240;
  img_dsc.header.h = 240;
  img_dsc.data_size = 240 * 240 * 2;
  img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  img_dsc.data = preview_buffer;

  int consecutive_failures = 0;
  unsigned long last_preview_time = 0;
  const unsigned long PREVIEW_INTERVAL = 50;

  while (1) {
    // ===========================================
    // HANDLE CAPTURE REQUEST
    // ===========================================

    if (capture_requested) {
      capture_requested = false;
      capture_in_progress = true;

      Serial.println("CAPTURE REQUESTED!");

      if (status_label) {
        if (lvgl_lock(-1)) {
          lv_label_set_text(
            status_label,
            "Capturing photo...");

          lvgl_unlock();
        }
      }

      Serial.println(
        "CAPTURE: calling esp_camera_fb_get()");

      capture_frame = esp_camera_fb_get();

      Serial.printf(
        "CAPTURE: returned, frame=%p\n",
        capture_frame);

      // =========================================
      // SENSOR DIAGNOSTIC
      // Print the complete block under one mutex.
      // =========================================

      if (capture_frame != NULL) {
        char diagnostic[256];
        int offset = 0;

        offset += snprintf(
          diagnostic + offset,
          sizeof(diagnostic) - offset,
          "\n=== SENSOR DIAGNOSTIC START ===\n");

        offset += snprintf(
          diagnostic + offset,
          sizeof(diagnostic) - offset,
          "Resolution: %dx%d\n",
          capture_frame->width,
          capture_frame->height);

        offset += snprintf(
          diagnostic + offset,
          sizeof(diagnostic) - offset,
          "Format: %d\n",
          capture_frame->format);

        offset += snprintf(
          diagnostic + offset,
          sizeof(diagnostic) - offset,
          "Frame length: %u\n",
          (unsigned int)capture_frame->len);

        offset += snprintf(
          diagnostic + offset,
          sizeof(diagnostic) - offset,
          "First 16 bytes: ");

        for (int i = 0;
             i < 16 && i < capture_frame->len;
             i++) {
          offset += snprintf(
            diagnostic + offset,
            sizeof(diagnostic) - offset,
            "%02X ",
            capture_frame->buf[i]);
        }

        offset += snprintf(
          diagnostic + offset,
          sizeof(diagnostic) - offset,
          "\n=== SENSOR DIAGNOSTIC END ===\n");

        if (serialMutex != NULL &&
            xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
          Serial.print(diagnostic);
          Serial.flush();
          xSemaphoreGive(serialMutex);
        } else {
          Serial.print(diagnostic);
          Serial.flush();
        }
      }

      if (capture_frame && captured_photo) {
        if (capture_frame->len <= PHOTO_BUFFER_SIZE) {
          memcpy(
            captured_photo,
            capture_frame->buf,
            capture_frame->len);

          captured_photo_len = capture_frame->len;

          Serial.printf(
            "Photo stored: %u bytes\n",
            (unsigned int)captured_photo_len);
        } else {
          Serial.printf(
            "Photo too large! %u > %u\n",
            (unsigned int)capture_frame->len,
            (unsigned int)PHOTO_BUFFER_SIZE);

          captured_photo_len = 0;
        }

        esp_camera_fb_return(capture_frame);

        capture_complete = true;
        capture_in_progress = false;
        consecutive_failures = 0;
        upload_requested = true;

        Serial.println("Capture complete!");

        if (status_label) {
          if (lvgl_lock(-1)) {
            lv_label_set_text(
              status_label,
              "Photo captured! Tap again to scan");

            lvgl_unlock();
          }
        }
      } else {
        Serial.println("Photo capture FAILED!");

        if (capture_frame) {
          esp_camera_fb_return(capture_frame);
        }

        capture_complete = true;
        capture_in_progress = false;
        consecutive_failures++;

        if (status_label) {
          if (lvgl_lock(-1)) {
            lv_label_set_text(
              status_label,
              "Capture failed! Try again");

            lvgl_unlock();
          }
        }
      }
    }

    // ===========================================
    // NORMAL CAMERA PREVIEW
    // ===========================================

    if (capture_in_progress) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    unsigned long now = millis();

    if (now - last_preview_time < PREVIEW_INTERVAL) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    last_preview_time = now;

    pic = esp_camera_fb_get();

    if (pic == NULL) {
      Serial.printf(
        "Frame NULL | Heap: %u | Largest block: %u | PSRAM: %u\n",
        ESP.getFreeHeap(),
        ESP.getMaxAllocHeap(),
        ESP.getFreePsram());
    }

    if (pic != NULL) {
      consecutive_failures = 0;

      // Log frame information periodically.
      static int frame_count = 0;

      if (++frame_count % 100 == 0) {
        Serial.printf(
          "Frame OK: %dx%d, format=%d, %u bytes\n",
          pic->width,
          pic->height,
          pic->format,
          (unsigned int)pic->len);
      }

      // If RGB565 is available, copy it to the preview.
      // JPEG preview is not decoded here.
      if (pic->format == PIXFORMAT_RGB565) {
        int src_width = pic->width;
        int src_height = pic->height;

        int crop_x = (src_width - 240) / 2;
        int crop_y = (src_height - 240) / 2;

        for (int y = 0; y < 240; y++) {
          int src_y = crop_y + y;

          if (src_y >= 0 && src_y < src_height) {
            int src_offset =
              (src_y * src_width + crop_x) * 2;

            int dst_offset = y * 240 * 2;

            if (src_offset + 240 * 2 <= pic->len) {
              memcpy(
                preview_buffer + dst_offset,
                pic->buf + src_offset,
                240 * 2);
            }
          }
        }
      }

      if (lvgl_lock(-1)) {
        lv_img_set_src(img_camera, &img_dsc);
        lvgl_unlock();
      }

      esp_camera_fb_return(pic);
    } else {
      consecutive_failures++;

      if (consecutive_failures % 10 == 1) {
        Serial.printf(
          "Camera FB get FAILED | Free heap: %u | Free PSRAM: %u\n",
          ESP.getFreeHeap(),
          ESP.getFreePsram());
      }

      // Attempt recovery after repeated camera failures.
      if (consecutive_failures > 50) {
        Serial.println(
          "Too many failures! Reinitializing camera...");

        esp_camera_deinit();

        vTaskDelay(pdMS_TO_TICKS(1000));

        init_camera();

        vTaskDelay(pdMS_TO_TICKS(500));

        consecutive_failures = 0;

        Serial.println("Camera recovery attempted!");
      }

      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  vTaskDelete(NULL);
}

// =============================================
// SETUP
// =============================================

void setup() {
  Serial.begin(19200);
  delay(1000);

  serialMutex = xSemaphoreCreateMutex();

  Serial.println(
    "\n\n=== ESP32-S3 Touch LCD Camera Scanner ===");

  // Create mutex for LVGL.
  lvgl_api_mux = xSemaphoreCreateRecursiveMutex();

  Serial.println("Arduino_GFX LVGL_Arduino_v8 example");

  String LVGL_Arduino =
    String('V') + lv_version_major() + "." +
    lv_version_minor() + "." + lv_version_patch();

  Serial.println(LVGL_Arduino);

#ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
#endif

  // Initialize display.
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }

  gfx->fillScreen(0x0000);

#ifdef EXAMPLE_PIN_NUM_LCD_BL
  ledcAttach(
    EXAMPLE_PIN_NUM_LCD_BL,
    LEDC_FREQ,
    LEDC_TIMER_10_BIT);

  ledcWrite(
    EXAMPLE_PIN_NUM_LCD_BL,
    (1 << LEDC_TIMER_10_BIT) / 100 * 80);
#endif

  // Initialize touch device.
  Wire.begin(
    EXAMPLE_PIN_NUM_TP_SDA,
    EXAMPLE_PIN_NUM_TP_SCL);

  bsp_touch_init(
    &Wire,
    gfx->getRotation(),
    gfx->width(),
    gfx->height());

  // Initialize LVGL.
  lv_init();

#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  screenWidth = gfx->width();
  screenHeight = gfx->height();
  bufSize = screenWidth * screenHeight;

  // Allocate the large LVGL draw buffer in PSRAM.
  disp_draw_buf = (lv_color_t *)ps_malloc(
    bufSize * sizeof(lv_color_t));

  // Fall back to internal RAM only if PSRAM allocation fails.
  if (!disp_draw_buf) {
    Serial.println(
      "PSRAM LVGL buffer allocation failed; trying internal RAM.");

    disp_draw_buf = (lv_color_t *)heap_caps_malloc(
      bufSize * sizeof(lv_color_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  if (!disp_draw_buf) {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  } else {
    lv_disp_draw_buf_init(
      &draw_buf,
      disp_draw_buf,
      NULL,
      bufSize);

    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.direct_mode = true;

    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;

    lv_indev_drv_init(&indev_drv);

    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;

    lv_indev_drv_register(&indev_drv);

    lvgl_camera_ui_init(lv_scr_act());
  }

  // Allocate JPEG photo buffer in PSRAM.
  captured_photo = (uint8_t *)ps_malloc(
    PHOTO_BUFFER_SIZE);

  if (!captured_photo) {
    Serial.println("Failed to allocate photo buffer!");
  } else {
    Serial.printf(
      "Photo buffer allocated: %u bytes\n",
      (unsigned int)PHOTO_BUFFER_SIZE);
  }

  // Connect to Wi-Fi.
  Serial.println("Connecting to WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int wifi_attempts = 0;

  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");

    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed!");
  }

  // Create camera task on core 0.
  BaseType_t task_result = xTaskCreatePinnedToCore(
    camera_task,
    "camera_task",
    1024 * 12,
    NULL,
    1,
    NULL,
    0);

  if (task_result != pdPASS) {
    Serial.println("Failed to create camera task!");
  }
}

// =============================================
// MAIN LOOP
// =============================================

void loop() {
  if (upload_requested && !upload_in_progress) {
    upload_requested = false;
    upload_in_progress = true;

    BaseType_t result = xTaskCreate(
      upload_receipt_task,
      "receipt_upload",
      6144,
      NULL,
      1,
      NULL);

    if (result != pdPASS) {
      upload_in_progress = false;

      Serial.println("Failed to create upload task!");

      Serial.printf(
        "Free heap: %u bytes\n",
        ESP.getFreeHeap());

      Serial.printf(
        "Largest free block: %u bytes\n",
        ESP.getMaxAllocHeap());

      Serial.printf(
        "Free PSRAM: %u bytes\n",
        ESP.getFreePsram());

      Serial.printf(
        "Minimum free heap: %u bytes\n",
        ESP.getMinFreeHeap());
    }
  }

  if (lvgl_lock(-1)) {
    lv_timer_handler();
    lvgl_unlock();
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}