/*
  ===========================================================================
   V.E.R.T. - Camera + AI Module
   Seeed XIAO ESP32S3 Sense Firmware
  ===========================================================================

  WHAT THIS BOARD DOES (independently of the main ESP32):
    - Streams live MJPEG video at:      http://vert-cam.local:81/stream
    - Serves a single JPEG snapshot at: http://vert-cam.local/capture
    - Runs an on-device Edge Impulse image classifier every few seconds and
      publishes the result as JSON at:  http://vert-cam.local/status
        {"ok":true,"detected":true,"label":"healthy","confidence":0.93,"ageMs":1200}
      "detected" is false whenever the top label's confidence is below
      CONFIDENCE_THRESHOLD - i.e. nothing was confidently recognized in
      frame (empty view, blurry, plant out of shot, etc). The dashboard
      should skip charting a reading when detected is false rather than
      plotting a low-confidence guess.

  This board is a SEPARATE device from your Doit ESP32 DevKit V1. It joins
  the same WiFi network and gets its own IP / mDNS name. Your dashboard's
  browser talks to BOTH boards directly (vert.local for sensors/pump/door,
  vert-cam.local for video/AI) - the two firmwares never talk to each other
  directly, which keeps your sensor/motor timing on the main board safe from
  anything camera or AI related.

  ===========================================================================
  BEFORE YOU CAN COMPILE THIS FILE
  ===========================================================================
  1. Board package:
       File > Preferences > "Additional Board Manager URLs" ->
       https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
       Then Tools > Board > Boards Manager > install "esp32" by Espressif.
       Tools > Board > select "XIAO_ESP32S3" (under ESP32 Arduino).
       Tools > PSRAM > "OPI PSRAM"  <-- REQUIRED, the camera needs the PSRAM.

  2. Train your model on Edge Impulse (https://studio.edgeimpulse.com):
       a. Create a free account, create a new project (e.g. "vert-plant-health").
       b. Data acquisition: use "Upload data" and upload photos of your plants,
          labelled per class - for example "healthy", "sick", "seedling",
          "ready_to_harvest". Aim for 40-60+ photos per class from different
          angles/lighting; more is better. You can also collect photos live
          through the XIAO once this firmware is flashed once in "capture only"
          mode - simplest is just uploading photos from your phone though.
       c. Create Impulse: Image (96x96, or 160x160 if you have PSRAM to spare)
          -> Image processing block -> Transfer Learning (MobileNetV2 96x96
          0.1, the smallest one) as the learning block. This is the standard
          combo that fits on an ESP32S3.
       d. Train it, check the accuracy on the "Model testing" tab.
       e. Deployment tab -> "Arduino library" -> Build. This downloads a .zip.
       f. Arduino IDE: Sketch > Include Library > Add .ZIP Library... and
          select that downloaded file.
       g. This file already includes <Strawberry_Dataset_inferencing.h> -
          that's the project name Edge Impulse gave your export. If you
          retrain under a different project name later, update that one
          #include line below to match (check the include at the top of
          File > Examples > [project]_inferencing > esp32 > esp32_camera).

  3. Libraries required (Library Manager):
       - "ESP32" board package already provides esp_camera.h - nothing to
         install for that.
       - Your exported Edge Impulse Arduino library (step 2f above).

  ===========================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_camera.h"

#include <Strawberry_Dataset_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

// ===========================================================================
// --- WiFi credentials (same network as the main ESP32) ---
// ===========================================================================
const char* WIFI_SSID     = "Airbox-7113";
const char* WIFI_PASSWORD = "14594540";
const char* MDNS_NAME     = "vert-cam";   // reachable at http://vert-cam.local

// ===========================================================================
// --- XIAO ESP32S3 Sense camera pin map (fixed by the board, do not change) ---
// ===========================================================================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// Raw frame buffer size the camera captures at for inference - this matches
// Edge Impulse's official ESP32 camera example pattern (QVGA), which then
// gets cropped/interpolated down to whatever size your model actually wants
// (EI_CLASSIFIER_INPUT_WIDTH/HEIGHT, defined by your exported library).
// We stream at this same QVGA size too - it keeps everything on one camera
// config instead of switching modes, and is plenty for a monitoring feed.
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS  320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS  240
#define EI_CAMERA_FRAME_BYTE_SIZE        3

// ===========================================================================
// --- Servers: port 80 for JSON/snapshot, port 81 for the MJPEG stream ---
// ===========================================================================
WebServer controlServer(80);
WiFiServer streamServer(81);
WiFiClient streamClient; // persists across loop() iterations - see notes below

// Latest inference result, shared between the inference task and /status.
// Guarded by dataMutex since it's written from the inference task (core 0)
// and read from the HTTP handler running in loop() (core 1).
String  lastLabel      = "starting";
float   lastConfidence = 0.0;
bool    lastDetected    = false; // true only when confidence clears CONFIDENCE_THRESHOLD
unsigned long lastInferenceAt = 0;
const unsigned long INFERENCE_INTERVAL_MS = 4000; // run AI every 4s - plenty for plant growth

// Below this confidence, we treat the reading as "nothing confidently
// detected" rather than reporting whatever label happened to score highest -
// an empty frame or a leaf half out of shot will otherwise still produce a
// label with low confidence, which is misleading on a health graph.
const float CONFIDENCE_THRESHOLD = 0.60;

// esp_camera_fb_get()/fb_return() are not safe to call from two tasks at
// once. Streaming (loop() on core 1) and inference (its own task on core 0,
// see below) both grab frames, so every camera access is wrapped with this.
SemaphoreHandle_t camMutex;
// Guards lastLabel/lastConfidence/lastDetected/lastInferenceAt between the
// inference task (writer) and the HTTP handler (reader).
SemaphoreHandle_t dataMutex;

// ===========================================================================
// --- CORS helper (the dashboard page is loaded from vert.local, this
//     server is vert-cam.local - different origins, so /status and
//     /capture need this header or the browser's fetch() will block it) ---
// ===========================================================================
void sendCors() {
  controlServer.sendHeader("Access-Control-Allow-Origin", "*");
}

// ===========================================================================
// --- Camera init ---
// ===========================================================================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (!psramFound()) {
    Serial.println("No PSRAM found! Set Tools > PSRAM > OPI PSRAM and re-flash.");
  }
  config.frame_size = FRAMESIZE_QVGA;   // 320x240 - matches EI_CAMERA_RAW_FRAME_BUFFER_*
  config.jpeg_quality = 12;
  // REVERTED to 1 buffer / WHEN_EMPTY - the fb_count=2 + GRAB_LATEST change
  // turned out to cause a heap-corruption crash on this board (reproduced
  // even with AI inference fully disabled, so it's specific to this camera
  // config, not the classifier). Frame-buffer contention between streaming
  // and inference is handled by camMutex instead now.
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }
  return true;
}

// ===========================================================================
// --- MJPEG streaming (raw socket, not through WebServer - lower overhead) ---
// ===========================================================================
// IMPORTANT: this does NOT loop-and-block until the client disconnects.
// A blocking loop here would starve controlServer.handleClient() and the
// inference timer for as long as anyone had the video open (which, for a
// dashboard left on a farmer's screen, could be forever). Instead this
// sends headers once, then sendOneStreamFrame() is called once per pass of
// the main loop() - so streaming, inference, and /status all interleave.
void beginStreamClient(WiFiClient& client) {
  String header = client.readStringUntil('\r');
  while (client.available()) client.read(); // discard rest of request

  client.println("HTTP/1.1 200 OK");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println();
}

unsigned long lastStreamFrameAt = 0;
const unsigned long STREAM_FRAME_INTERVAL_MS = 80; // ~12fps, gentle on the radio

void sendOneStreamFrame() {
  if (!streamClient || !streamClient.connected()) return;
  if (millis() - lastStreamFrameAt < STREAM_FRAME_INTERVAL_MS) return;

  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(50)) != pdTRUE) return; // inference is mid-grab, skip this pass rather than block the stream

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(camMutex);
    return;
  }

  streamClient.println("--frame");
  streamClient.println("Content-Type: image/jpeg");
  streamClient.printf("Content-Length: %u\r\n\r\n", fb->len);
  streamClient.write(fb->buf, fb->len);
  streamClient.println();

  esp_camera_fb_return(fb);
  xSemaphoreGive(camMutex);
  lastStreamFrameAt = millis();
}

// ===========================================================================
// --- Snapshot + status endpoints (port 80) ---
// ===========================================================================
void handleCapture() {
  xSemaphoreTake(camMutex, portMAX_DELAY);
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(camMutex);
    controlServer.send(500, "text/plain", "capture failed");
    return;
  }
  sendCors();
  controlServer.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(camMutex);
}

void handleStatus() {
  sendCors();
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  String label = lastLabel;
  float confidence = lastConfidence;
  bool detected = lastDetected;
  unsigned long age = millis() - lastInferenceAt;
  xSemaphoreGive(dataMutex);

  String json = "{";
  json += "\"ok\":true,";
  json += "\"detected\":" + String(detected ? "true" : "false") + ",";
  json += "\"label\":\"" + label + "\",";
  json += "\"confidence\":" + String(confidence, 2) + ",";
  json += "\"ageMs\":" + String(age);
  json += "}";
  controlServer.send(200, "application/json", json);
}

// ===========================================================================
// --- AI inference ---
// This follows Edge Impulse's official ESP32 camera capture pipeline
// (from the esp32_camera example your exported library ships with):
// grab a JPEG frame -> fmt2rgb888 into a full-size raw buffer -> use their
// image library to crop+interpolate down to the model's expected input size
// (EI_CLASSIFIER_INPUT_WIDTH/HEIGHT). That crop_and_interpolate_rgb888 call
// is what actually did the resizing correctly in their sample - it matters
// because it's exactly what the model saw during training via their
// pipeline, so swapping in a different resize method can quietly hurt
// accuracy even if the code "works".
// ===========================================================================
static uint8_t* snapshot_buf = nullptr; // full QVGA RGB888 frame

static int ei_camera_get_data(size_t offset, size_t length, float* out_ptr) {
  size_t pixel_ix = offset * 3;
  size_t pixels_left = length;
  size_t out_ptr_ix = 0;
  while (pixels_left != 0) {
    // BGR->RGB swap, matching the official example
    // (esp32-camera returns BGR ordering from fmt2rgb888)
    out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16)
                        + (snapshot_buf[pixel_ix + 1] << 8)
                        +  snapshot_buf[pixel_ix];
    out_ptr_ix++;
    pixel_ix += 3;
    pixels_left--;
  }
  return 0;
}

bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t* out_buf) {
  xSemaphoreTake(camMutex, portMAX_DELAY);
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(camMutex);
    return false;
  }
  Serial.printf("[chk] got fb: len=%u width=%u height=%u format=%d\n", fb->len, fb->width, fb->height, fb->format);

  Serial.println("[chk] calling fmt2rgb888...");
  bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
  esp_camera_fb_return(fb);
  xSemaphoreGive(camMutex);
  if (!converted) return false;
  Serial.println("[chk] fmt2rgb888 returned OK");

  bool do_resize = (img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS)
                 || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS);

  if (do_resize) {
    Serial.printf("[chk] calling crop_and_interpolate_rgb888 (%ux%u -> %ux%u)...\n",
                  EI_CAMERA_RAW_FRAME_BUFFER_COLS, EI_CAMERA_RAW_FRAME_BUFFER_ROWS, img_width, img_height);
    ei::image::processing::crop_and_interpolate_rgb888(
      out_buf, EI_CAMERA_RAW_FRAME_BUFFER_COLS, EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
      out_buf, img_width, img_height);
    Serial.println("[chk] crop_and_interpolate_rgb888 returned OK");
  }
  return true;
}

void runInference() {
  // TEMPORARY checkpoint logging to localize a heap-corruption crash - remove
  // once the crash is found. Prints happen just BEFORE each step, so if the
  // board resets/corrupts mid-step, the last printed checkpoint tells us
  // which call is responsible.
  Serial.printf("[chk] free heap=%u free psram=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());

  if (!snapshot_buf) {
    Serial.println("[chk] allocating snapshot_buf...");
    snapshot_buf = (uint8_t*)ps_malloc(
      EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);
    if (!snapshot_buf) { Serial.println("ERR: snapshot_buf alloc failed"); return; }
    Serial.printf("[chk] snapshot_buf allocated at %p\n", (void*)snapshot_buf);
  }

  Serial.println("[chk] calling ei_camera_capture...");
  if (!ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf)) {
    Serial.println("Failed to capture image for inference");
    return;
  }
  Serial.println("[chk] ei_camera_capture returned OK");

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &ei_camera_get_data;

  Serial.println("[chk] calling run_classifier...");
  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    Serial.printf("ERR: classifier failed (%d)\n", err);
    return;
  }
  Serial.println("[chk] run_classifier returned OK");

  float best = 0;
  String bestLabel = "unknown";
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > best) {
      best = result.classification[i].value;
      bestLabel = result.classification[i].label;
    }
  }

  bool detected = best >= CONFIDENCE_THRESHOLD;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  lastLabel      = bestLabel;
  lastConfidence = best;
  lastDetected   = detected;
  lastInferenceAt = millis();
  xSemaphoreGive(dataMutex);

  Serial.printf("Prediction: %s (%.2f) %s [dsp %dms, classify %dms]\n",
                bestLabel.c_str(), best, detected ? "" : "(below threshold, not reported as a detection)",
                result.timing.dsp, result.timing.classification);
}

// ===========================================================================
// --- Inference task ---
// Runs on core 0, completely separate from loop() (which stays on core 1
// handling the HTTP servers and MJPEG stream). This is what actually fixes
// the stream freezing during inference: however long run_classifier() takes,
// it can never block sendOneStreamFrame() or controlServer.handleClient()
// again, since they're not sharing a call stack anymore. Camera hardware
// access is still serialized via camMutex since the sensor itself can only
// be grabbed by one task at a time.
// ===========================================================================
// TEMPORARY diagnostic switch: set to 0 to disable AI inference entirely
// (camera streaming + /capture + /status still run, /status just always
// reports "starting"/not detected). Used to bisect whether a heap-corruption
// crash is coming from the inference pipeline or from something else
// (camera/PSRAM/WiFi) - flip back to 1 once the crash is isolated.
#define ENABLE_INFERENCE 0

void inferenceTask(void* pvParameters) {
  for (;;) {
#if ENABLE_INFERENCE
    runInference();
#endif
    vTaskDelay(pdMS_TO_TICKS(INFERENCE_INTERVAL_MS));
  }
}

// ===========================================================================
// --- Setup / loop ---
// ===========================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("### V.E.R.T. CAM + AI MODULE ###");

  camMutex  = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();

  if (!initCamera()) {
    Serial.println("Camera init failed - halting.");
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(MDNS_NAME)) {
      Serial.printf("Reachable at http://%s.local\n", MDNS_NAME);
    }
  } else {
    Serial.println("\nWiFi failed - check credentials.");
  }

  controlServer.on("/capture", HTTP_GET, handleCapture);
  controlServer.on("/status", HTTP_GET, handleStatus);
  controlServer.onNotFound([]() {
    sendCors();
    controlServer.send(404, "text/plain", "Not found. Try /capture, /status, or :81/stream");
  });
  controlServer.begin();
  streamServer.begin();

  // Pinned to core 0 - loop() (streaming + HTTP) runs on core 1 by default
  // on the ESP32S3, so this genuinely runs in parallel rather than just
  // interleaving on the same core.
  //
  // Stack size: Edge Impulse's image DSP + MobileNetV2 classification path
  // needs a lot more stack than a typical task - 8192 bytes overflows during
  // the very first classification (crop/interpolate + DSP buffers eat into
  // it), which corrupts whatever heap memory sits right after the task's
  // stack. That's what "CORRUPT HEAP: Bad head ... got 0x00000000" right
  // after boot actually was. 32768 gives it real headroom; internal RAM can
  // afford it since the frame buffers themselves live in PSRAM, not here.
  xTaskCreatePinnedToCore(inferenceTask, "inference", 32768, NULL, 1, NULL, 0);

  Serial.println("Ready: /capture, /status on :80, MJPEG stream on :81");
}

void loop() {
  controlServer.handleClient();

  // Accept a new stream viewer if none is currently connected. If someone
  // is already watching, new connections are naturally queued by the
  // WiFiServer backlog until the current one disconnects.
  if (!streamClient || !streamClient.connected()) {
    WiFiClient incoming = streamServer.available();
    if (incoming) {
      streamClient = incoming;
      beginStreamClient(streamClient);
    }
  }
  sendOneStreamFrame(); // no-op if nobody's connected, does nothing blocking

  // Inference itself runs on its own FreeRTOS task on core 0 (see setup()) -
  // nothing to call here. That's what keeps it from ever blocking the
  // stream/HTTP handling above, no matter how long a classification takes.
}
