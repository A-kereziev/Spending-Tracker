#include "esp_camera.h"
#define LV_CONF_PATH /home/sashi/Arduino/libraries/lv_conf.h
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "bsp_cst816.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <time.h>

#define ENABLE_AUTOFOCUS 1
#if ENABLE_AUTOFOCUS
#include "ESP32_OV5640_AF.h"
static OV5640 autofocus;
#endif

// =============================================
// CAMERA CONFIGURATION
// =============================================

#define CAMERA_XCLK 10000000
#define CAMERA_JPEG_QUALITY 6
#define CAMERA_FRAME_SIZE FRAMESIZE_UXGA
#define CAMERA_FALLBACK_SIZE FRAMESIZE_SXGA
#define CAMERA_CAPTURE_ATTEMPTS 1

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

#define PHOTO_BUFFER_SIZE (2 * 1024 * 1024)

uint8_t *captured_photo = nullptr;
size_t captured_photo_len = 0;

volatile bool capture_requested = false;
volatile bool capture_complete = false;
volatile bool capture_in_progress = false;
volatile bool upload_requested = false;
volatile bool upload_in_progress = false;

bool camera_initialized = false;

framesize_t active_frame_size = CAMERA_FRAME_SIZE;

// =============================================
// MUTEXES
// =============================================

static SemaphoreHandle_t lvgl_api_mux = NULL;

// =============================================
// DISPLAY SETUP
// =============================================

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  EXAMPLE_PIN_NUM_LCD_DC,
  EXAMPLE_PIN_NUM_LCD_CS,
  EXAMPLE_PIN_NUM_LCD_SCLK,
  EXAMPLE_PIN_NUM_LCD_MOSI,
  EXAMPLE_PIN_NUM_LCD_MISO
);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus,
  EXAMPLE_PIN_NUM_LCD_RST,
  EXAMPLE_LCD_ROTATION,
  true,
  EXAMPLE_LCD_H_RES,
  EXAMPLE_LCD_V_RES
);

uint32_t screenWidth;
uint32_t screenHeight;
uint32_t bufSize;

lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf = nullptr;
lv_disp_drv_t disp_drv;

lv_obj_t *capture_button = nullptr;
lv_obj_t *status_label = nullptr;


// =============================================
// LVGL LOCKING
// =============================================

bool lvgl_lock(int timeout_ms) {
  if (!lvgl_api_mux) {
    return false;
  }

  const TickType_t timeout_ticks =
    (timeout_ms == -1)
      ? portMAX_DELAY
      : pdMS_TO_TICKS(timeout_ms);

  return xSemaphoreTakeRecursive(
    lvgl_api_mux,
    timeout_ticks
  ) == pdTRUE;
}

void lvgl_unlock() {
  xSemaphoreGiveRecursive(lvgl_api_mux);
}

void set_status(const char *message) {
  if (status_label && lvgl_lock(-1)) {
    lv_label_set_text(status_label, message);
    lvgl_unlock();
  }
}

void set_capture_button_enabled(bool enabled) {
  if (capture_button && lvgl_lock(-1)) {
    if (enabled) {
      lv_obj_clear_state(
        capture_button,
        LV_STATE_DISABLED
      );
    } else {
      lv_obj_add_state(
        capture_button,
        LV_STATE_DISABLED
      );
    }

    lvgl_unlock();
  }
}

#if LV_USE_LOG != 0
void my_print(const char *buf) {
  Serial.printf("%s", buf);
  Serial.flush();
}
#endif

// =============================================
// MEMORY DIAGNOSTICS
// =============================================

void printUploadMemory(const char *stage) {
  Serial.printf("\n[MEMORY] %s\n", stage);

  Serial.printf(
    "  Internal free: %u bytes\n",
    (unsigned)heap_caps_get_free_size(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    )
  );

  Serial.printf(
    "  Largest internal block: %u bytes\n",
    (unsigned)heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    )
  );

  Serial.printf(
    "  Free PSRAM: %u bytes\n",
    (unsigned)ESP.getFreePsram()
  );

  Serial.printf(
    "  Task stack margin: %u bytes\n",
    (unsigned)uxTaskGetStackHighWaterMark(NULL)
  );
}

// =============================================
// JPEG VALIDATION
// =============================================

bool valid_jpeg(const uint8_t *data, size_t length) {
  if (!data || length < 4) {
    return false;
  }

  // JPEG Start Of Image marker.
  if (data[0] != 0xFF || data[1] != 0xD8) {
    return false;
  }

  // JPEG End Of Image marker.
  if (data[length - 2] != 0xFF ||
      data[length - 1] != 0xD9) {
    return false;
  }

  return true;
}

// =============================================
// UPLOAD CLEANUP
// =============================================

void finish_upload(bool success) {
  if (success) {
    set_status("Receipt uploaded!");
  } else {
    set_status("Upload failed! Try again");
  }

  Serial.println("=== END RECEIPT DIAGNOSTICS ===");

  upload_in_progress = false;
  set_capture_button_enabled(true);

  vTaskDelete(NULL);
}

// =============================================
// RECEIPT UPLOAD TASK
// =============================================

void upload_receipt_task(void *param) {
  Serial.println(
    "\n=== RECEIPT UPLOAD / TLS DIAGNOSTICS ==="
  );

  set_status("Uploading receipt...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
      "Upload aborted: Wi-Fi disconnected."
    );

    finish_upload(false);
    return;
  }

  if (!valid_jpeg(
        captured_photo,
        captured_photo_len
      )) {

    Serial.println(
      "Upload aborted: missing or invalid JPEG."
    );

    finish_upload(false);
    return;
  }

  String baseURL = String(RECEIPT_API_URL);

  baseURL.trim();

  while (baseURL.endsWith("/")) {
    baseURL.remove(baseURL.length() - 1);
  }

  if (!baseURL.startsWith("https://")) {
    Serial.println(
      "Upload aborted: URL must begin with https://"
    );

    finish_upload(false);
    return;
  }

  String hostname = baseURL.substring(8);

  int slash = hostname.indexOf('/');

  if (slash >= 0) {
    hostname.remove(slash);
  }

  if (hostname.length() == 0 ||
      hostname.indexOf(':') >= 0) {

    Serial.println(
      "Upload aborted: invalid hostname or port."
    );

    finish_upload(false);
    return;
  }

  // ===========================================
  // NTP SYNCHRONIZATION
  // ===========================================

  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.google.com"
  );

  time_t now = time(nullptr);

  for (int i = 0;
       i < 30 && now < 1760000000;
       ++i) {

    vTaskDelay(pdMS_TO_TICKS(1000));
    now = time(nullptr);
  }

  if (now < 1760000000) {
    Serial.println(
      "Upload aborted: NTP clock sync failed."
    );

    finish_upload(false);
    return;
  }

  // ===========================================
  // TLS ROOT CERTIFICATE
  // ===========================================

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

  Serial.printf(
    "Host: %s\n",
    hostname.c_str()
  );

  Serial.printf(
    "JPEG: %u bytes\n",
    (unsigned)captured_photo_len
  );

  printUploadMemory("Before network tests");

  // ===========================================
  // DNS TEST
  // ===========================================

  IPAddress address;

  bool dnsOK = WiFi.hostByName(
    hostname.c_str(),
    address
  );

  Serial.printf(
    "DNS: %s",
    dnsOK ? "OK" : "FAILED"
  );

  if (dnsOK) {
    Serial.printf(
      " (%s)",
      address.toString().c_str()
    );
  }

  Serial.println();

  // ===========================================
  // TCP TEST
  // ===========================================

  bool tcpOK = false;

  if (dnsOK) {
    WiFiClient tcp;

    tcpOK = tcp.connect(
      hostname.c_str(),
      443,
      15000
    );

    Serial.printf(
      "TCP port 443: %s\n",
      tcpOK ? "OK" : "FAILED"
    );

    tcp.stop();
  }

  // ===========================================
  // TLS TEST
  // ===========================================

  bool tlsOK = false;

  if (tcpOK) {
    WiFiClientSecure tls;

    // Diagnostic connection only.
    // No receipt or authorization token is sent.
    tls.setInsecure();

    tls.setHandshakeTimeout(15);

    printUploadMemory(
      "Before TLS handshake"
    );

    uint32_t start = millis();

    tlsOK = tls.connect(
      hostname.c_str(),
      443,
      15000
    );

    Serial.printf(
      "TLS handshake: %s (%lu ms)\n",
      tlsOK ? "OK" : "FAILED",
      (unsigned long)(millis() - start)
    );

    if (!tlsOK) {
      char details[192] = {0};

      int err = tls.lastError(
        details,
        sizeof(details)
      );

      Serial.printf(
        "TLS lastError: %d (0x%08X) %s\n",
        err,
        (unsigned)err,
        details
      );
    }

    tls.stop();

    printUploadMemory(
      "After TLS handshake"
    );
  }

  // ===========================================
  // HTTPS POST
  // ===========================================

  bool upload_success = false;

  if (tlsOK) {
    WiFiClientSecure client;

    client.setCACert(root_ca);
    client.setHandshakeTimeout(15);

    HTTPClient http;

    http.setConnectTimeout(15000);
    http.setTimeout(120000);

    http.setFollowRedirects(
      HTTPC_DISABLE_FOLLOW_REDIRECTS
    );

    String endpoint =
      baseURL + "/api/receipt";

    if (!http.begin(client, endpoint)) {
      Serial.println(
        "HTTP initialization failed."
      );

    } else {
      http.addHeader(
        "Content-Type",
        "image/jpeg"
      );

      http.addHeader(
        "Authorization",
        String("Bearer ") + RECEIPT_API_TOKEN
      );

      printUploadMemory("Before POST");

      uint32_t start = millis();

      int status = http.POST(
        captured_photo,
        captured_photo_len
      );

      Serial.printf(
        "HTTP result: %d (%lu ms)\n",
        status,
        (unsigned long)(millis() - start)
      );

      if (status > 0) {
        Serial.println(http.getString());

        if (status == 200) {
          Serial.println(
            "Receipt processed successfully."
          );

          upload_success = true;

        } else {
          Serial.println(
            "Server returned a non-200 HTTP status."
          );
        }

      } else {
        Serial.printf(
          "HTTPClient: %s\n",
          http.errorToString(status).c_str()
        );

        char details[192] = {0};

        int err = client.lastError(
          details,
          sizeof(details)
        );

        Serial.printf(
          "TLS after POST: %d %s\n",
          err,
          details
        );
      }

      http.end();
    }

    client.stop();

    printUploadMemory("After POST");

  } else {
    Serial.println(
      "POST skipped because DNS/TCP/TLS preflight failed."
    );
  }

  finish_upload(upload_success);
}

// =============================================
// DISPLAY FLUSH
// =============================================

void my_disp_flush(
  lv_disp_drv_t *disp_drv,
  const lv_area_t *area,
  lv_color_t *color_p
) {
#if (LV_COLOR_16_SWAP != 0)

  gfx->draw16bitBeRGBBitmap(
    0, 0,
    (uint16_t *)color_p,
    screenWidth,
    screenHeight
  );

#else

  gfx->draw16bitRGBBitmap(
    0, 0,
    (uint16_t *)color_p,
    screenWidth,
    screenHeight
  );

#endif

  lv_disp_flush_ready(disp_drv);
}

// =============================================
// TOUCHPAD INPUT
// =============================================

void my_touchpad_read(
  lv_indev_drv_t *indev_drv,
  lv_indev_data_t *data
) {

  uint16_t touchpad_x;
  uint16_t touchpad_y;

  bsp_touch_read();

  if (bsp_touch_get_coordinates(
        &touchpad_x,
        &touchpad_y
      )) {

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
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }

  if (capture_in_progress ||
      capture_requested ||
      upload_requested ||
      upload_in_progress) {

    Serial.println(
      "Capture ignored: device busy."
    );

    return;
  }

  capture_requested = true;

  Serial.println(
    "Capture button pressed!"
  );

  lv_label_set_text(
    status_label,
    "Capturing..."
  );

  lv_obj_add_state(
    capture_button,
    LV_STATE_DISABLED
  );
}

// =============================================
// LVGL CAMERA UI
// =============================================

void lvgl_camera_ui_init(lv_obj_t *parent) {
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(parent, lv_color_hex(0x101820), 0);

  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "RECEIPT SCANNER");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

  lv_obj_t *panel = lv_obj_create(parent);
  lv_obj_set_size(panel, 216, 146);
  lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 51);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x1C2934), 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x364957), 0);
  lv_obj_set_style_radius(panel, 12, 0);

  lv_obj_t *guide = lv_label_create(panel);
  lv_label_set_text(guide, LV_SYMBOL_FILE "\n\nPosition receipt\nunder the camera");
  lv_obj_set_style_text_color(guide, lv_color_hex(0xDAE7ED), 0);
  lv_obj_set_style_text_align(guide, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(guide);

  status_label = lv_label_create(parent);
  lv_obj_set_width(status_label, 216);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xDAE7ED), 0);
  lv_label_set_text(status_label, "Camera standby");
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 207);

  capture_button = lv_btn_create(parent);
  lv_obj_set_size(capture_button, 216, 48);
  lv_obj_align(capture_button, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_add_event_cb(capture_button, capture_button_event, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_bg_color(capture_button, lv_color_hex(0x237D56), 0);
  lv_obj_set_style_bg_color(capture_button, lv_color_hex(0x354550), LV_STATE_DISABLED);
  lv_obj_set_style_shadow_width(capture_button, 0, 0);
  lv_obj_set_style_border_width(capture_button, 0, 0);
  lv_obj_set_style_radius(capture_button, 10, 0);
  lv_obj_t *label = lv_label_create(capture_button);
  lv_label_set_text(label, "CAPTURE");
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(label);
  lv_obj_add_state(capture_button, LV_STATE_DISABLED);
}

// =============================================
// CAMERA SENSOR SETTINGS
// =============================================

void configure_sensor(sensor_t *s) {
  if (!s) return;
  Serial.printf("Camera PID: 0x%04X\n", s->id.PID);
  if (s->id.PID != OV5640_PID) {
    Serial.println("WARNING: OV5640 not detected.");
    return;
  }
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
  s->set_brightness(s, 0);
  s->set_contrast(s, 1);
  s->set_saturation(s, -1);
  if (s->set_sharpness) s->set_sharpness(s, 2);
  s->set_whitebal(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);
}

void init_autofocus(sensor_t *s) {
#if ENABLE_AUTOFOCUS
  if (!s || s->id.PID != OV5640_PID) return;
  autofocus.start(s);
  if (autofocus.focusInit() != 0) {
    Serial.println("[AF] Firmware initialization failed; continuing capture.");
    return;
  }
  if (autofocus.autoFocusMode() != 0) {
    Serial.println("[AF] Mode initialization failed; continuing capture.");
    return;
  }
  Serial.println("[AF] Continuous autofocus enabled.");
#endif
}

// =============================================
// CAMERA INITIALIZATION
// =============================================

// OV5640 PWDN is normally active HIGH; verify module wiring.
void camera_standby() {
  if (camera_initialized) {
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
      Serial.printf("Camera deinit error: 0x%X\n", (unsigned)err);
    }
    camera_initialized = false;
  }
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  Serial.println("[CAM] PWDN asserted (standby requested)");
}

bool init_camera(framesize_t frame_size) {
  Serial.println(
    "\n=== CAMERA INITIALIZATION ==="
  );

  if (!psramFound()) {
    Serial.println(
      "ERROR: PSRAM NOT FOUND!"
    );

    return false;
  }

  Serial.printf(
    "Total PSRAM: %u bytes\n",
    (unsigned)ESP.getPsramSize()
  );

  Serial.printf(
    "Free PSRAM: %u bytes\n",
    (unsigned)ESP.getFreePsram()
  );

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

  // ===========================================
  // CAMERA CAPTURE CONFIGURATION
  // ===========================================

  config.xclk_freq_hz = CAMERA_XCLK;

  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = frame_size;

  config.jpeg_quality = CAMERA_JPEG_QUALITY;

  config.fb_count = 1;

  config.fb_location = CAMERA_FB_IN_PSRAM;

  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  // ===========================================
  // CONFIGURATION DIAGNOSTICS
  // ===========================================

  Serial.printf(
    "XCLK: %u Hz\n",
    (unsigned)config.xclk_freq_hz
  );

  Serial.printf(
    "JPEG quality: %d\n",
    config.jpeg_quality
  );

  Serial.printf(
    "Frame buffers: %d\n",
    config.fb_count
  );

  if (frame_size == FRAMESIZE_UXGA) {
    Serial.println(
      "Resolution: UXGA (1600x1200)"
    );

  } else if (frame_size == FRAMESIZE_SXGA) {
    Serial.println(
      "Resolution: SXGA (1280x1024)"
    );
  }

  Serial.printf(
    "Heap BEFORE camera init: %u\n",
    ESP.getFreeHeap()
  );

  esp_err_t err = esp_camera_init(&config);

  Serial.printf(
    "Heap AFTER camera init: %u\n",
    ESP.getFreeHeap()
  );

  if (err != ESP_OK) {
    Serial.printf(
      "Camera initialization failed: 0x%X\n",
      (unsigned)err
    );

    camera_standby();

    return false;
  }

  camera_initialized = true;
  active_frame_size = frame_size;

  sensor_t *s = esp_camera_sensor_get();

  if (!s) {
    Serial.println(
      "Failed to get sensor handle!"
    );

    camera_standby();

    return false;
  }

  configure_sensor(s);
  init_autofocus(s);

  // Allow autofocus and automatic exposure to settle.
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Discard the first queued frame, which may predate focus.
  camera_fb_t *warmup = esp_camera_fb_get();
  if (!warmup) {
    Serial.println("[CAM] Warmup frame failed; aborting capture.");
    camera_standby();
    return false;
  }
  esp_camera_fb_return(warmup);
  Serial.println("[CAM] Warmup frame discarded.");

  Serial.println(
    "Camera initialization complete!"
  );

  Serial.println(
    "=== END CAMERA INITIALIZATION ===\n"
  );

  return true;
}

// =============================================
// CAMERA REINITIALIZATION
// =============================================

// =============================================
// CAMERA FRAME DIAGNOSTICS
// =============================================

void print_frame_diagnostics(camera_fb_t *frame) {
  if (!frame) return;
  Serial.printf("Frame: %ux%u, %u bytes, format %d\n",
                (unsigned)frame->width, (unsigned)frame->height,
                (unsigned)frame->len, frame->format);
}

// =============================================
// SINGLE FRAME ACQUISITION
// =============================================

bool acquire_photo() {
  if (!camera_initialized ||
      !captured_photo) {

    Serial.println(
      "Capture aborted: camera or buffer unavailable."
    );

    return false;
  }

  captured_photo_len = 0;

  for (int attempt = 1;
       attempt <= CAMERA_CAPTURE_ATTEMPTS;
       attempt++) {

    Serial.printf(
      "\nCapture attempt %d/%d\n",
      attempt,
      CAMERA_CAPTURE_ATTEMPTS
    );

    uint32_t start = millis();

    camera_fb_t *frame = esp_camera_fb_get();

    Serial.printf(
      "esp_camera_fb_get(): %p (%lu ms)\n",
      (void *)frame,
      (unsigned long)(millis() - start)
    );

    if (!frame) {
      Serial.println(
        "ERROR: Camera returned NULL frame."
      );

      vTaskDelay(pdMS_TO_TICKS(200));

      continue;
    }

    print_frame_diagnostics(frame);

    // Validate frame format.

    if (frame->format != PIXFORMAT_JPEG) {
      Serial.println(
        "ERROR: Frame is not JPEG."
      );

      esp_camera_fb_return(frame);

      vTaskDelay(pdMS_TO_TICKS(200));

      continue;
    }

    // Validate buffer size.

    if (frame->len > PHOTO_BUFFER_SIZE) {
      Serial.printf(
        "ERROR: Photo too large: %u > %u\n",
        (unsigned)frame->len,
        (unsigned)PHOTO_BUFFER_SIZE
      );

      esp_camera_fb_return(frame);

      continue;
    }

    // Validate JPEG markers.

    if (!valid_jpeg(frame->buf, frame->len)) {
      Serial.println(
        "ERROR: Incomplete or invalid JPEG."
      );

      esp_camera_fb_return(frame);

      vTaskDelay(pdMS_TO_TICKS(200));

      continue;
    }

    // Copy JPEG into dedicated PSRAM storage.

    memcpy(
      captured_photo,
      frame->buf,
      frame->len
    );

    captured_photo_len = frame->len;

    // Return the camera driver's frame buffer.

    esp_camera_fb_return(frame);

    Serial.printf(
      "Photo stored: %u bytes\n",
      (unsigned)captured_photo_len
    );

    Serial.println(
      "JPEG validation: PASSED"
    );

    return true;
  }

  return false;
}

// =============================================
// CAMERA CAPTURE WITH FALLBACK
// =============================================

bool capture_photo() {
  captured_photo_len = 0;
  bool success = false;
  uint32_t start = millis();
  if (captured_photo && init_camera(CAMERA_FRAME_SIZE)) {
    success = acquire_photo();
  }
  // Always release the driver and request standby, even on failure.
  camera_standby();
  Serial.printf("[CAM] Capture cycle: %lu ms\n", (unsigned long)(millis() - start));
  return success;
}

// =============================================
// CAMERA TASK
// Only this task accesses the camera driver.
// =============================================

static void camera_task(void *param) {
  set_status(captured_photo ? "Tap CAPTURE to scan receipt" : "Photo buffer failed!");
  set_capture_button_enabled(captured_photo != nullptr);

  while (true) {
    if (capture_requested &&
        !capture_in_progress &&
        !upload_requested &&
        !upload_in_progress) {

      capture_in_progress = true;
      capture_requested = false;
      capture_complete = false;

      captured_photo_len = 0;

      Serial.println(
        "\n=== CAPTURE REQUESTED ==="
      );

      set_status("Capturing photo...");

      printUploadMemory(
        "Before camera capture"
      );

      bool success = capture_photo();

      if (success) {
        Serial.println(
          "Capture complete!"
        );

        set_status("Photo captured!");

        upload_requested = true;

      } else {
        Serial.println(
          "Photo capture FAILED!"
        );

        captured_photo_len = 0;

        set_status(
          "Capture failed! Try again"
        );

        set_capture_button_enabled(true);
      }

      capture_complete = true;
      capture_in_progress = false;

      printUploadMemory(
        "After camera capture"
      );

      Serial.println(
        "=== END CAPTURE ===\n"
      );
    }

    // Do not continuously request JPEG frames.

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =============================================
// SETUP
// =============================================

void setup() {
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  Serial.begin(115200);

  delay(1000);

  // Keep camera errors visible during debugging.
  // FB-OVF is useful diagnostic information.

  esp_log_level_set(
    "cam_hal",
    ESP_LOG_WARN
  );

  esp_log_level_set(
    "camera",
    ESP_LOG_WARN
  );

  lvgl_api_mux =
    xSemaphoreCreateRecursiveMutex();

  if (!lvgl_api_mux) {
    Serial.println(
      "Failed to create LVGL mutex!"
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println(
    "\n=== ESP32-S3 Touch LCD Camera Scanner ==="
  );

  Serial.printf(
    "LVGL version: %d.%d.%d\n",
    lv_version_major(),
    lv_version_minor(),
    lv_version_patch()
  );

#ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
#endif

  // ===========================================
  // DISPLAY INITIALIZATION
  // ===========================================

  if (!gfx->begin(20000000)) {
    Serial.println(
      "gfx->begin() failed!"
    );
  }

  gfx->fillScreen(0x0000);

  gfx->fillScreen(0xFFFF);
  delay(3000);

  gfx->fillScreen(0xF800);
  delay(3000);

  gfx->fillScreen(0x07E0);
  delay(3000);

  gfx->fillScreen(0x001F);
  delay(3000);

  gfx->fillScreen(0x0000);
#ifdef EXAMPLE_PIN_NUM_LCD_BL

  pinMode(EXAMPLE_PIN_NUM_LCD_BL, OUTPUT);
  digitalWrite(EXAMPLE_PIN_NUM_LCD_BL, HIGH);

#endif

  // ===========================================
  // TOUCH INITIALIZATION
  // ===========================================

  Wire.begin(
    EXAMPLE_PIN_NUM_TP_SDA,
    EXAMPLE_PIN_NUM_TP_SCL
  );

  bsp_touch_init(
    &Wire,
    gfx->getRotation(),
    gfx->width(),
    gfx->height()
  );

  // ===========================================
  // LVGL INITIALIZATION
  // ===========================================

  lv_init();

#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  screenWidth = gfx->width();
  screenHeight = gfx->height();

  bufSize = screenWidth * screenHeight;

  disp_draw_buf = (lv_color_t *)ps_malloc(
    bufSize * sizeof(lv_color_t)
  );

  if (!disp_draw_buf) {
    Serial.println(
      "PSRAM LVGL allocation failed."
    );

    disp_draw_buf =
      (lv_color_t *)heap_caps_malloc(
        bufSize * sizeof(lv_color_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
      );
  }

  if (!disp_draw_buf) {
    Serial.println(
      "LVGL draw buffer allocation failed!"
    );

    while (true) {
      delay(1000);
    }
  }

  lv_disp_draw_buf_init(
    &draw_buf,
    disp_draw_buf,
    NULL,
    bufSize
  );

  lv_disp_drv_init(&disp_drv);

  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;

  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;

  disp_drv.direct_mode = false;
  disp_drv.full_refresh = true;

  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;

  lv_indev_drv_init(&indev_drv);

  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;

  lv_indev_drv_register(&indev_drv);

  lvgl_camera_ui_init(lv_scr_act());

  // ===========================================
  // JPEG PHOTO BUFFER
  // ===========================================

  captured_photo = (uint8_t *)ps_malloc(
    PHOTO_BUFFER_SIZE
  );

  if (!captured_photo) {
    Serial.println(
      "Failed to allocate photo buffer!"
    );

    set_status("Photo buffer failed!");

  } else {
    Serial.printf(
      "Photo buffer allocated: %u bytes\n",
      (unsigned)PHOTO_BUFFER_SIZE
    );
  }

  // ===========================================
  // WIFI INITIALIZATION
  // ===========================================

  Serial.println(
    "Connecting to WiFi..."
  );

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  int wifi_attempts = 0;

  while (WiFi.status() != WL_CONNECTED &&
         wifi_attempts < 30) {

    delay(500);

    Serial.print(".");

    wifi_attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(
      "WiFi connected!"
    );

    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());

  } else {
    Serial.println(
      "WiFi connection failed!"
    );
  }

  // ===========================================
  // CAMERA TASK INITIALIZATION
  // ===========================================

  BaseType_t task_result =
    xTaskCreatePinnedToCore(
      camera_task,
      "camera_task",
      12288,
      NULL,
      1,
      NULL,
      0
    );

  if (task_result != pdPASS) {
    Serial.println(
      "Failed to create camera task!"
    );

    set_status("Camera task failed!");
  }
}

// =============================================
// MAIN LOOP
// =============================================

void loop() {
  if (upload_requested &&
      !upload_in_progress &&
      !capture_in_progress) {

    upload_in_progress = true;
    upload_requested = false;

    BaseType_t result = xTaskCreate(
      upload_receipt_task,
      "receipt_upload",
      12288,
      NULL,
      1,
      NULL
    );

    if (result != pdPASS) {
      Serial.println(
        "Failed to create upload task!"
      );

      printUploadMemory(
        "Upload task creation failed"
      );

      set_status(
        "Upload task failed!"
      );

      set_capture_button_enabled(true);

      upload_in_progress = false;
    }
  }

  if (lvgl_lock(-1)) {
    lv_timer_handler();
    lvgl_unlock();
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}