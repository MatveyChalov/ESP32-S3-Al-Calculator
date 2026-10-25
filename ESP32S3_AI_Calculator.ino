/*
  ESP32-S3 AI Calculator
  Libraries (Arduino Library Manager):
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
    - Keypad
    - ArduinoJson (7.x)
  Board package: Espressif ESP32 (current stable)

  IMPORTANT:
  - Change pin definitions and TFT dimensions for your board.
  - Battery ADC requires a correctly designed voltage divider; never exceed ADC limits.
  - For production, replace the CA certificate below with the current root CA bundle
    appropriate for each API host. Certificate verification is enabled; no insecure mode.
  - NVS Preferences is not automatically encrypted. Use flash/NVS encryption and secure
    boot for products that store credentials. Do not commit real keys to source control.
  - API calls are synchronous HTTP transactions with bounded timeouts. They are isolated
    from the UI's regular loop, but a request can temporarily occupy the main task.
    For fully responsive production UX, move HTTP transactions to a dedicated FreeRTOS task.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Keypad.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>
#include <math.h>
#include <esp_crt_bundle.h>

// --------------------------- Board configuration ---------------------------
static constexpr int TFT_CS = 10;
static constexpr int TFT_DC = 9;
static constexpr int TFT_RST = 8;
static constexpr int TFT_SCLK = 12;
static constexpr int TFT_MOSI = 11;
static constexpr int TFT_WIDTH = 240;
static constexpr int TFT_HEIGHT = 320;
static constexpr int BATTERY_ADC_PIN = 4;
static constexpr float ADC_REFERENCE_V = 3.30f;
static constexpr float BATTERY_DIVIDER_RATIO = 2.00f; // e.g. equal-value divider: battery voltage = ADC voltage * 2
static constexpr float BATTERY_EMPTY_V = 3.30f;
static constexpr float BATTERY_FULL_V = 4.20f;
static constexpr uint32_t STATUS_REFRESH_MS = 1000;
static constexpr uint32_t WIFI_RETRY_MS = 10000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 18000;
static constexpr size_t MAX_PROMPT_CHARS = 240;
static constexpr size_t MAX_RESPONSE_CHARS = 1300;

// 4x4 matrix keypad. Change these pins to match the wiring.
const byte ROWS = 4, COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {5, 6, 7, 15};
byte colPins[COLS] = {16, 17, 18, 21};
Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;

// ------------------------------- UI state ----------------------------------
enum class AppState : uint8_t { CALCULATOR, SECRET_MENU, WIFI_MENU, WIFI_SCAN, WIFI_PASSWORD, AI_MENU, AI_PROMPT, AI_RESPONSE, SETTINGS };
AppState state = AppState::CALCULATOR;
enum class Provider : uint8_t { OPENAI, DEEPSEEK, GEMINI, CLAUDE };
Provider selectedProvider = Provider::OPENAI;

String calcInput = "0";
String calcStored = "";
char calcOperator = 0;
bool calcNewNumber = true;
bool calcError = false;
String statusMessage = "Ready";
uint32_t lastStatusUpdate = 0;
uint32_t lastWifiAttempt = 0;
uint32_t wifiAttemptStarted = 0;
bool wifiConnecting = false;

String wifiSsid;
String wifiPassword;
String textBuffer;
String aiPrompt;
String aiResponse;
String apiKeys[4];
String aiModels[4] = {"gpt-4.1-mini", "deepseek-chat", "gemini-2.5-flash", "claude-sonnet-4-20250514"};
int menuSelection = 0;
int scannedNetworkCount = 0;
String scannedNetworks[12];
int responseScrollLine = 0;

// Secret sequence is A, B, C, D, #. Change it before publishing your own device.
const char SECRET_SEQUENCE[] = "ABCD#";
String secretProgress;
uint32_t secretLastKeyMs = 0;

// ------------------------------ UI helpers ----------------------------------
static const uint16_t COLOR_BG = ST77XX_BLACK;
static const uint16_t COLOR_FG = ST77XX_WHITE;
static const uint16_t COLOR_ACCENT = ST77XX_CYAN;
static const uint16_t COLOR_WARN = ST77XX_YELLOW;
static const uint16_t COLOR_ERROR = ST77XX_RED;
static const uint16_t COLOR_MUTED = 0x8410;

void drawText(int16_t x, int16_t y, const String &text, uint16_t color = COLOR_FG, uint8_t size = 2) {
  tft.setTextColor(color, COLOR_BG);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print(text);
}

String providerName(Provider p) {
  switch (p) {
    case Provider::OPENAI: return "ChatGPT / OpenAI";
    case Provider::DEEPSEEK: return "DeepSeek";
    case Provider::GEMINI: return "Google Gemini";
    case Provider::CLAUDE: return "Anthropic Claude";
  }
  return "Unknown";
}

int providerIndex(Provider p) { return static_cast<int>(p); }

String clockText() {
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 5)) {
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeInfo);
    return String(buffer);
  }
  uint32_t seconds = millis() / 1000UL;
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "+%02lu:%02lu:%02lu", (unsigned long)(seconds / 3600UL), (unsigned long)((seconds / 60UL) % 60UL), (unsigned long)(seconds % 60UL));
  return String(buffer);
}

int batteryPercent() {
  uint32_t raw = 0;
  for (int i = 0; i < 8; ++i) { raw += analogRead(BATTERY_ADC_PIN); delayMicroseconds(250); }
  float adc = (raw / 8.0f) / 4095.0f * ADC_REFERENCE_V;
  float voltage = adc * BATTERY_DIVIDER_RATIO;
  float percent = (voltage - BATTERY_EMPTY_V) * 100.0f / (BATTERY_FULL_V - BATTERY_EMPTY_V);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return (int)lroundf(percent);
}

void drawStatusBar() {
  tft.fillRect(0, 0, TFT_WIDTH, 22, 0x2104);
  drawText(4, 5, clockText(), COLOR_FG, 1);
  String battery = String(batteryPercent()) + "%";
  drawText(151, 5, battery, COLOR_FG, 1);
  drawText(205, 5, WiFi.status() == WL_CONNECTED ? "WiFi" : "--", WiFi.status() == WL_CONNECTED ? ST77XX_GREEN : COLOR_MUTED, 1);
}

void drawHeader(const String &title) {
  tft.fillScreen(COLOR_BG);
  drawStatusBar();
  drawText(6, 30, title, COLOR_ACCENT, 2);
  tft.drawFastHLine(5, 53, TFT_WIDTH - 10, COLOR_MUTED);
}

void drawWrappedText(const String &text, int x, int y, int maxChars, int lineHeight, uint16_t color = COLOR_FG, int maxLines = 15) {
  int lineCount = 0;
  int start = 0;
  while (start < (int)text.length() && lineCount < maxLines) {
    int end = start;
    int lastSpace = -1;
    while (end < (int)text.length() && end - start < maxChars && text[end] != '\n') {
      if (text[end] == ' ') lastSpace = end;
      ++end;
    }
    if (end < (int)text.length() && text[end] != '\n' && end - start >= maxChars && lastSpace > start) end = lastSpace;
    String line = text.substring(start, end);
    line.trim();
    drawText(x, y + lineCount * lineHeight, line, color, 1);
    ++lineCount;
    start = end;
    if (start < (int)text.length() && text[start] == '\n') ++start;
    else while (start < (int)text.length() && text[start] == ' ') ++start;
  }
  if (start < (int)text.length() && lineCount >= maxLines) drawText(x, y + lineCount * lineHeight, "...more", COLOR_WARN, 1);
}

void showMessage(const String &title, const String &message, uint16_t color = COLOR_FG) {
  drawHeader(title);
  drawWrappedText(message, 8, 66, 37, 13, color, 17);
  drawText(8, TFT_HEIGHT - 20, "D: Back", COLOR_MUTED, 1);
}

void drawCalculator() {
  drawHeader("Calculator");
  drawText(8, 76, "AI CALC  /  ESP32-S3", COLOR_MUTED, 1);
  tft.fillRoundRect(8, 105, TFT_WIDTH - 16, 58, 5, 0x18E3);
  String shown = calcError ? "ERROR" : calcInput;
  if (shown.length() > 18) shown = shown.substring(shown.length() - 18);
  int textSize = shown.length() > 12 ? 2 : 3;
  int16_t x1, y1; uint16_t w, h;
  tft.setTextSize(textSize); tft.getTextBounds(shown, 0, 0, &x1, &y1, &w, &h);
  drawText(TFT_WIDTH - 14 - (int)w, 124, shown, calcError ? COLOR_ERROR : COLOR_FG, textSize);
  const char *rows[] = {"7  8  9  /", "4  5  6  *", "1  2  3  -", "C  0  .  +", "A=Enter  B=Clear"};
  for (int i = 0; i < 5; ++i) drawText(14, 183 + i * 25, rows[i], i == 4 ? COLOR_MUTED : COLOR_FG, i == 4 ? 1 : 2);
  drawText(8, 303, "Secret access: enter code", COLOR_MUTED, 1);
}

void drawSecretMenu() {
  drawHeader("Secret Menu");
  const char *items[] = {"Wi-Fi setup", "AI assistants", "Settings", "Return to calculator"};
  for (int i = 0; i < 4; ++i) {
    uint16_t c = i == menuSelection ? COLOR_BG : COLOR_FG;
    uint16_t bg = i == menuSelection ? COLOR_ACCENT : COLOR_BG;
    tft.fillRoundRect(8, 68 + i * 45, TFT_WIDTH - 16, 36, 4, bg);
    drawText(16, 79 + i * 45, String(i + 1) + ". " + items[i], c, 1);
  }
  drawText(8, 267, "2/8: move  A: select", COLOR_MUTED, 1);
  drawText(8, 284, "D: back", COLOR_MUTED, 1);
}

void drawWifiMenu() {
  drawHeader("Wi-Fi Setup");
  drawText(8, 69, WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected", WiFi.status() == WL_CONNECTED ? ST77XX_GREEN : COLOR_WARN, 2);
  drawWrappedText("1 Scan networks\n2 Enter SSID manually\n3 Enter API keys\n4 Disconnect Wi-Fi", 8, 108, 35, 25, COLOR_FG, 8);
  drawText(8, 285, "D: back", COLOR_MUTED, 1);
}

void drawScanMenu() {
  drawHeader("Networks");
  if (scannedNetworkCount == 0) drawText(8, 70, "No networks found", COLOR_WARN, 1);
  for (int i = 0; i < scannedNetworkCount && i < 10; ++i) {
    uint16_t bg = i == menuSelection ? COLOR_ACCENT : COLOR_BG;
    uint16_t fg = i == menuSelection ? COLOR_BG : COLOR_FG;
    tft.fillRect(5, 62 + i * 22, TFT_WIDTH - 10, 20, bg);
    String s = String(i + 1) + ". " + scannedNetworks[i];
    if (s.length() > 32) s = s.substring(0, 32);
    drawText(8, 68 + i * 22, s, fg, 1);
  }
  drawText(8, 290, "A: choose  D: back", COLOR_MUTED, 1);
}

void drawAiMenu() {
  drawHeader("AI Assistants");
  for (int i = 0; i < 4; ++i) {
    uint16_t bg = i == menuSelection ? COLOR_ACCENT : COLOR_BG;
    uint16_t fg = i == menuSelection ? COLOR_BG : COLOR_FG;
    tft.fillRect(6, 65 + i * 37, TFT_WIDTH - 12, 32, bg);
    drawText(12, 75 + i * 37, String(i + 1) + ". " + providerName((Provider)i), fg, 1);
  }
  drawText(8, 226, "A: select provider", COLOR_MUTED, 1);
  drawText(8, 243, "B: ask question", COLOR_MUTED, 1);
  drawText(8, 260, "C: change model", COLOR_MUTED, 1);
  drawText(8, 277, "D: back", COLOR_MUTED, 1);
}

void drawPromptScreen() {
  drawHeader(providerName(selectedProvider));
  drawWrappedText("Type prompt using keypad multi-tap. A=send, B=backspace, C=space, D=cancel.", 8, 63, 36, 14, COLOR_MUTED, 4);
  drawWrappedText(textBuffer, 8, 125, 36, 14, COLOR_FG, 10);
  drawText(8, 287, "Length: " + String(textBuffer.length()) + "/" + String(MAX_PROMPT_CHARS), COLOR_ACCENT, 1);
}

void drawResponseScreen() {
  drawHeader(providerName(selectedProvider));
  int maxLines = (TFT_HEIGHT - 100) / 13;
  String visible = aiResponse;
  int start = 0;
  int currentLine = 0;
  int skipLines = responseScrollLine;
  while (start < (int)visible.length() && skipLines > 0) {
    int e = visible.indexOf('\n', start);
    if (e < 0) e = visible.length();
    start = e + 1; --skipLines;
  }
  String tail = visible.substring(min(start, (int)visible.length()));
  drawWrappedText(tail, 7, 62, 37, 13, COLOR_FG, maxLines);
  drawText(8, TFT_HEIGHT - 20, "2/8 scroll  D back", COLOR_MUTED, 1);
}

void render() {
  switch (state) {
    case AppState::CALCULATOR: drawCalculator(); break;
    case AppState::SECRET_MENU: drawSecretMenu(); break;
    case AppState::WIFI_MENU: drawWifiMenu(); break;
    case AppState::WIFI_SCAN: drawScanMenu(); break;
    case AppState::WIFI_PASSWORD: showMessage("Wi-Fi Password", "Enter password with keypad multi-tap. A: connect, B: delete, D: cancel.\n\n" + textBuffer); break;
    case AppState::AI_MENU: drawAiMenu(); break;
    case AppState::AI_PROMPT: drawPromptScreen(); break;
    case AppState::AI_RESPONSE: drawResponseScreen(); break;
    case AppState::SETTINGS: showMessage("Settings", "Battery ADC pin: " + String(BATTERY_ADC_PIN) + "\nWi-Fi reconnect: enabled\nTLS certificate verification: enabled\nD: back"); break;
  }
}

// ------------------------------ Calculator ----------------------------------
bool parseNumber(const String &s, double &value) {
  if (!s.length()) return false;
  char *end = nullptr;
  value = strtod(s.c_str(), &end);
  return end != s.c_str() && *end == '\0' && isfinite(value);
}

void clearCalculator() {
  calcInput = "0"; calcStored = ""; calcOperator = 0; calcNewNumber = true; calcError = false;
}

void appendDigit(char key) {
  if (calcError) clearCalculator();
  if (calcNewNumber) { calcInput = "0"; calcNewNumber = false; }
  if (calcInput.length() >= 16) return;
  if (calcInput == "0") calcInput = String(key); else calcInput += key;
}

void appendDecimal() {
  if (calcError) clearCalculator();
  if (calcNewNumber) { calcInput = "0"; calcNewNumber = false; }
  if (calcInput.indexOf('.') < 0 && calcInput.length() < 16) calcInput += '.';
}

void setOperator(char op) {
  if (calcError) clearCalculator();
  if (calcOperator && !calcNewNumber) {
    // Chained operations are evaluated left-to-right.
    char old = calcOperator;
    double a, b;
    if (!parseNumber(calcStored, a) || !parseNumber(calcInput, b)) { calcError = true; return; }
    if (old == '+') a += b; else if (old == '-') a -= b; else if (old == '*') a *= b; else if (old == '/') { if (fabs(b) < 1e-12) { calcError = true; statusMessage = "Division by zero"; return; } a /= b; }
    if (!isfinite(a)) { calcError = true; return; }
    calcStored = String(a, 8); calcInput = calcStored; while (calcInput.indexOf('.') >= 0 && calcInput.endsWith("0")) calcInput.remove(calcInput.length() - 1); if (calcInput.endsWith(".")) calcInput.remove(calcInput.length() - 1);
  } else calcStored = calcInput;
  calcOperator = op; calcNewNumber = true;
}

void calculateResult() {
  if (!calcOperator || calcError) return;
  double a, b;
  if (!parseNumber(calcStored, a) || !parseNumber(calcInput, b)) { calcError = true; return; }
  double result = 0;
  switch (calcOperator) {
    case '+': result = a + b; break;
    case '-': result = a - b; break;
    case '*': result = a * b; break;
    case '/': if (fabs(b) < 1e-12) { calcError = true; statusMessage = "Division by zero"; return; } result = a / b; break;
    default: return;
  }
  if (!isfinite(result)) { calcError = true; return; }
  calcInput = String(result, 8);
  while (calcInput.indexOf('.') >= 0 && calcInput.endsWith("0")) calcInput.remove(calcInput.length() - 1);
  if (calcInput.endsWith(".")) calcInput.remove(calcInput.length() - 1);
  if (calcInput == "-0") calcInput = "0";
  calcOperator = 0; calcStored = ""; calcNewNumber = true;
}

// ----------------------------- Wi-Fi / NTP ----------------------------------
void beginWifiConnect(const String &ssid, const String &password) {
  if (!ssid.length()) { statusMessage = "SSID is empty"; return; }
  wifiSsid = ssid; wifiPassword = password;
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wifi_pw", wifiPassword);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  wifiConnecting = true;
  wifiAttemptStarted = millis();
  lastWifiAttempt = millis();
  showMessage("Connecting", "Connecting to " + wifiSsid + "...\nPlease wait.", COLOR_WARN);
}

void onWifiConnected() {
  wifiConnecting = false;
  configTzTime("UTC0", "pool.ntp.org", "time.google.com", "time.nist.gov");
  statusMessage = "Wi-Fi connected; NTP sync requested";
  prefs.putString("ssid", wifiSsid);
  render();
}

void serviceWifi() {
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) onWifiConnected();
    else if (millis() - wifiAttemptStarted > 20000) { wifiConnecting = false; statusMessage = "Wi-Fi connection timed out"; showMessage("Wi-Fi Error", statusMessage, COLOR_ERROR); }
  }
  if (WiFi.status() != WL_CONNECTED && wifiSsid.length() && !wifiConnecting && millis() - lastWifiAttempt > WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    wifiConnecting = true; wifiAttemptStarted = millis();
  }
}

void scanNetworks() {
  WiFi.mode(WIFI_STA);
  int count = WiFi.scanNetworks(false, true);
  scannedNetworkCount = 0;
  if (count > 0) {
    for (int i = 0; i < count && scannedNetworkCount < 12; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.length()) scannedNetworks[scannedNetworkCount++] = ssid;
    }
  }
  WiFi.scanDelete();
  menuSelection = 0;
  state = AppState::WIFI_SCAN;
  render();
}

// ------------------------------- AI APIs ------------------------------------
// Uses the ESP32 core's CA store. If the server certificate chain cannot be validated,
// the request fails closed. Install/refresh root certificates in production firmware.
bool configureTls(WiFiClientSecure &client) {
  // Uses the ESP-IDF trusted root certificate bundle. In Arduino IDE, select an ESP32
  // core/build configuration with MBEDTLS certificate bundle support enabled. This keeps
  // certificate verification active and avoids setInsecure(). If the bundle is not enabled
  // in the installed core, enable it in the board's sdkconfig or replace this with a current
  // PEM root CA using client.setCACert(rootCaPem).
  client.setCACertBundle(esp_crt_bundle);
  return true;
}

String apiHost(Provider p) {
  switch (p) {
    case Provider::OPENAI: return "https://api.openai.com/v1/responses";
    case Provider::DEEPSEEK: return "https://api.deepseek.com/chat/completions";
    case Provider::GEMINI: return "https://generativelanguage.googleapis.com/v1beta/models/" + aiModels[providerIndex(p)] + ":generateContent";
    case Provider::CLAUDE: return "https://api.anthropic.com/v1/messages";
  }
  return "";
}

bool makeAiPayload(Provider p, const String &prompt, String &payload) {
  JsonDocument doc;
  int idx = providerIndex(p);
  if (p == Provider::OPENAI) {
    doc["model"] = aiModels[idx];
    doc["input"] = prompt;
    doc["max_output_tokens"] = 350;
  } else if (p == Provider::DEEPSEEK) {
    doc["model"] = aiModels[idx];
    doc["messages"][0]["role"] = "user";
    doc["messages"][0]["content"] = prompt;
    doc["max_tokens"] = 350;
    doc["stream"] = false;
  } else if (p == Provider::GEMINI) {
    doc["contents"][0]["parts"][0]["text"] = prompt;
    doc["generationConfig"]["maxOutputTokens"] = 350;
  } else {
    doc["model"] = aiModels[idx];
    doc["max_tokens"] = 350;
    doc["messages"][0]["role"] = "user";
    doc["messages"][0]["content"] = prompt;
  }
  payload = "";
  return serializeJson(doc, payload) > 0;
}

bool extractAiText(Provider p, const String &body, String &out, String &error) {
  JsonDocument doc;
  DeserializationError parseErr = deserializeJson(doc, body);
  if (parseErr) { error = "Invalid JSON response: " + String(parseErr.c_str()); return false; }
  if (doc["error"].is<JsonVariantConst>()) {
    const char *msg = doc["error"]["message"] | "Provider returned an error";
    error = String(msg); return false;
  }
  if (p == Provider::OPENAI) {
    // Responses API commonly exposes output[].content[].text.
    JsonArray output = doc["output"].as<JsonArray>();
    for (JsonVariant item : output) {
      JsonArray content = item["content"].as<JsonArray>();
      for (JsonVariant part : content) {
        const char *type = part["type"] | "";
        if (String(type) == "output_text" || String(type) == "text") out += String((const char *)(part["text"] | ""));
      }
    }
    if (!out.length()) out = String((const char *)(doc["output_text"] | ""));
  } else if (p == Provider::DEEPSEEK) {
    out = String((const char *)(doc["choices"][0]["message"]["content"] | ""));
  } else if (p == Provider::GEMINI) {
    JsonArray parts = doc["candidates"][0]["content"]["parts"].as<JsonArray>();
    for (JsonVariant part : parts) out += String((const char *)(part["text"] | ""));
  } else {
    JsonArray content = doc["content"].as<JsonArray>();
    for (JsonVariant part : content) if (String((const char *)(part["type"] | "")) == "text") out += String((const char *)(part["text"] | ""));
  }
  if (!out.length()) { error = "No text found in response. Check model/API access."; return false; }
  if (out.length() > MAX_RESPONSE_CHARS) out.remove(MAX_RESPONSE_CHARS);
  return true;
}

bool requestAi(Provider p, const String &prompt, String &answer, String &error) {
  if (WiFi.status() != WL_CONNECTED) { error = "Connect to Wi-Fi first"; return false; }
  String key = apiKeys[providerIndex(p)];
  if (!key.length()) { error = "API key missing. Configure it in Wi-Fi Setup > API keys."; return false; }
  String payload;
  if (!makeAiPayload(p, prompt, payload)) { error = "Could not create JSON payload"; return false; }
  WiFiClientSecure client;
  // Verify HTTPS certificates using the ESP-IDF trusted root certificate bundle.
  if (!configureTls(client)) { error = "TLS root CA not configured. Add a valid root CA PEM in configureTls()."; return false; }
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = apiHost(p);
  if (!http.begin(client, url)) { error = "HTTPS initialization failed"; return false; }
  http.addHeader("Content-Type", "application/json");
  if (p == Provider::OPENAI || p == Provider::DEEPSEEK) http.addHeader("Authorization", "Bearer " + key);
  else if (p == Provider::GEMINI) http.addHeader("x-goog-api-key", key);
  else {
    http.addHeader("x-api-key", key);
    http.addHeader("anthropic-version", "2023-06-01");
  }
  int code = http.POST((uint8_t *)payload.c_str(), payload.length());
  if (code < 200 || code >= 300) {
    String body = http.getString();
    error = "HTTP " + String(code) + ": " + body.substring(0, 220);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  return extractAiText(p, body, answer, error);
}

void saveApiKey(Provider p, const String &key) {
  int idx = providerIndex(p);
  apiKeys[idx] = key;
  const char *names[] = {"key_openai", "key_deepseek", "key_gemini", "key_claude"};
  prefs.putString(names[idx], key);
}

// Keypad multi-tap text entry. Keys 2-9 map to letters; 0 inserts a digit/space cycle.
String keyChars(char key) {
  switch (key) {
    case '1': return "1.,!?-_'\"():;";
    case '2': return "abc2ABC";
    case '3': return "def3DEF";
    case '4': return "ghi4GHI";
    case '5': return "jkl5JKL";
    case '6': return "mno6MNO";
    case '7': return "pqrs7PQRS";
    case '8': return "tuv8TUV";
    case '9': return "wxyz9WXYZ";
    case '0': return " 0";
    case '*': return "@#$%&*/+=<>[]{}";
    default: return "";
  }
}
char lastMultiKey = 0;
uint32_t lastMultiKeyMs = 0;
size_t lastMultiIndex = 0;
void multiTap(char key) {
  String chars = keyChars(key);
  if (!chars.length()) return;
  uint32_t now = millis();
  if (key == lastMultiKey && now - lastMultiKeyMs < 900 && textBuffer.length()) {
    lastMultiIndex = (lastMultiIndex + 1) % chars.length();
    textBuffer.setCharAt(textBuffer.length() - 1, chars[lastMultiIndex]);
  } else {
    if (textBuffer.length() < MAX_PROMPT_CHARS) {
      textBuffer += chars[0];
      lastMultiIndex = 0;
    }
  }
  lastMultiKey = key; lastMultiKeyMs = now;
}

// ------------------------------ Input routing -------------------------------
void handleCalculatorKey(char key) {
  if (key >= '0' && key <= '9') appendDigit(key);
  else if (key == '*') appendDecimal();
  else if (key == 'A') calculateResult();
  else if (key == 'B') clearCalculator();
  else if (key == 'C') { /* reserved */ }
  else if (key == 'D') { /* no-op in calculator */ }
  else if (key == '#') { /* sequence detector handles this */ }
  else if (key == '/') setOperator('/');
  // Matrix keypad letters A-D are mapped as UI keys. Operators are selected using long-press-free sequence:
  // A = equals, B = clear, C = +, D = -, and use keypad * for decimal.
  if (key == 'C') setOperator('+');
  if (key == 'D') setOperator('-');
  // Multiplication/division are accessed by repeated key mapping below to avoid extra physical keys.
  if (key == '#') setOperator('*');
}

void enterWifiPasswordForSelected() {
  textBuffer = ""; wifiPassword = ""; lastMultiKey = 0;
  state = AppState::WIFI_PASSWORD;
  render();
}

void handleKey(char key) {
  if (!key) return;
  // Secret sequence detection is active from the calculator only.
  if (state == AppState::CALCULATOR) {
    uint32_t now = millis();
    if (now - secretLastKeyMs > 2500) secretProgress = "";
    secretLastKeyMs = now;
    secretProgress += key;
    if (secretProgress.length() > strlen(SECRET_SEQUENCE)) secretProgress.remove(0, secretProgress.length() - strlen(SECRET_SEQUENCE));
    if (secretProgress == SECRET_SEQUENCE) { secretProgress = ""; state = AppState::SECRET_MENU; menuSelection = 0; render(); return; }
    // Keypad layout uses A=equals, B=clear, C=plus, D=minus, *=decimal, #=multiply.
    if (key == 'A' || key == 'B' || key == 'C' || key == 'D' || key == '*' || key == '#') {
      if (key == 'A') calculateResult();
      else if (key == 'B') clearCalculator();
      else if (key == 'C') setOperator('+');
      else if (key == 'D') setOperator('-');
      else if (key == '*') appendDecimal();
      else if (key == '#') setOperator('*');
    } else if (key >= '0' && key <= '9') appendDigit(key);
    render(); return;
  }

  if (state == AppState::SECRET_MENU) {
    if (key == '2') menuSelection = (menuSelection + 3) % 4;
    else if (key == '8') menuSelection = (menuSelection + 1) % 4;
    else if (key == 'A') {
      if (menuSelection == 0) { state = AppState::WIFI_MENU; }
      else if (menuSelection == 1) { state = AppState::AI_MENU; menuSelection = 0; }
      else if (menuSelection == 2) { state = AppState::SETTINGS; }
      else { state = AppState::CALCULATOR; }
    } else if (key == 'D') state = AppState::CALCULATOR;
    render(); return;
  }

  if (state == AppState::WIFI_MENU) {
    if (key == '1') scanNetworks();
    else if (key == '2') { textBuffer = ""; lastMultiKey = 0; showMessage("Enter SSID", "Use keypad multi-tap. A: next, B: delete, D: cancel.\n\n" + textBuffer); state = AppState::WIFI_PASSWORD; wifiSsid = ""; }
    else if (key == '3') { state = AppState::AI_MENU; menuSelection = 0; statusMessage = "Select provider to configure its key"; }
    else if (key == '4') { WiFi.disconnect(true); wifiSsid = ""; wifiPassword = ""; prefs.remove("ssid"); prefs.remove("wifi_pw"); statusMessage = "Wi-Fi disconnected"; }
    else if (key == 'D') state = AppState::SECRET_MENU;
    render(); return;
  }

  if (state == AppState::WIFI_SCAN) {
    if (key == '2') menuSelection = (menuSelection + scannedNetworkCount - 1) % max(1, scannedNetworkCount);
    else if (key == '8') menuSelection = (menuSelection + 1) % max(1, scannedNetworkCount);
    else if (key == 'A' && scannedNetworkCount > 0) {
      wifiSsid = scannedNetworks[menuSelection];
      textBuffer = ""; lastMultiKey = 0; state = AppState::WIFI_PASSWORD;
      showMessage("Wi-Fi Password", "SSID: " + wifiSsid + "\nEnter password, then A. B deletes. D cancels.");
    } else if (key == 'D') state = AppState::WIFI_MENU;
    render(); return;
  }

  if (state == AppState::WIFI_PASSWORD) {
    if (key == 'D') { state = AppState::WIFI_MENU; textBuffer = ""; render(); return; }
    if (key == 'B') { if (textBuffer.length()) textBuffer.remove(textBuffer.length() - 1); lastMultiKey = 0; render(); return; }
    if (key == 'A') {
      if (wifiSsid.length()) { wifiPassword = textBuffer; beginWifiConnect(wifiSsid, wifiPassword); state = AppState::WIFI_MENU; }
      else { wifiSsid = textBuffer; textBuffer = ""; lastMultiKey = 0; showMessage("Enter Password", "SSID saved: " + wifiSsid + "\nType password and press A."); }
      render(); return;
    }
    if (key >= '0' && key <= '9' || key == '*') multiTap(key);
    render(); return;
  }

  if (state == AppState::AI_MENU) {
    if (key == '2') menuSelection = (menuSelection + 3) % 4;
    else if (key == '8') menuSelection = (menuSelection + 1) % 4;
    else if (key >= '1' && key <= '4') menuSelection = key - '1';
    else if (key == 'A') {
      selectedProvider = (Provider)menuSelection;
      textBuffer = ""; lastMultiKey = 0;
      // If this provider has no key, let user enter one using multi-tap, then persist it.
      if (!apiKeys[providerIndex(selectedProvider)].length()) {
        state = AppState::AI_PROMPT;
        statusMessage = "KEY_ENTRY";
        showMessage("API Key", providerName(selectedProvider) + " key missing. Enter it with keypad, A to save, B to delete, D cancel.\n" + textBuffer);
      } else { state = AppState::AI_PROMPT; statusMessage = "PROMPT_ENTRY"; }
    } else if (key == 'B') { selectedProvider = (Provider)menuSelection; textBuffer = ""; lastMultiKey = 0; state = AppState::AI_PROMPT; statusMessage = "PROMPT_ENTRY"; }
    else if (key == 'C') {
      int idx = menuSelection;
      if (idx == 0) aiModels[idx] = aiModels[idx] == "gpt-4.1-mini" ? "gpt-4.1" : "gpt-4.1-mini";
      if (idx == 1) aiModels[idx] = aiModels[idx] == "deepseek-chat" ? "deepseek-reasoner" : "deepseek-chat";
      if (idx == 2) aiModels[idx] = aiModels[idx] == "gemini-2.5-flash" ? "gemini-2.5-pro" : "gemini-2.5-flash";
      if (idx == 3) aiModels[idx] = aiModels[idx] == "claude-sonnet-4-20250514" ? "claude-3-7-sonnet-latest" : "claude-sonnet-4-20250514";
      prefs.putString((String("model") + idx).c_str(), aiModels[idx]);
    } else if (key == 'D') state = AppState::SECRET_MENU;
    render(); return;
  }

  if (state == AppState::AI_PROMPT) {
    if (key == 'D') { state = AppState::AI_MENU; textBuffer = ""; render(); return; }
    if (key == 'B') { if (textBuffer.length()) textBuffer.remove(textBuffer.length() - 1); lastMultiKey = 0; render(); return; }
    if (key == 'C') { if (textBuffer.length() < MAX_PROMPT_CHARS) textBuffer += ' '; lastMultiKey = 0; render(); return; }
    if (key == 'A') {
      if (statusMessage == "KEY_ENTRY") {
        saveApiKey(selectedProvider, textBuffer);
        textBuffer = ""; statusMessage = "PROMPT_ENTRY";
        showMessage("API Key Saved", "Stored in NVS. Now enter a prompt; A sends, D cancels.");
      } else {
        aiPrompt = textBuffer; textBuffer = "";
        showMessage("Contacting AI", "Sending request to " + providerName(selectedProvider) + "...", COLOR_WARN);
        String answer, error;
        bool ok = requestAi(selectedProvider, aiPrompt, answer, error);
        if (ok) aiResponse = answer; else aiResponse = "Request failed:\n" + error;
        responseScrollLine = 0; state = AppState::AI_RESPONSE; render();
      }
      return;
    }
    if ((key >= '0' && key <= '9') || key == '*') multiTap(key);
    render(); return;
  }

  if (state == AppState::AI_RESPONSE) {
    if (key == '2' && responseScrollLine > 0) --responseScrollLine;
    else if (key == '8') responseScrollLine++;
    else if (key == 'D' || key == 'A') state = AppState::AI_MENU;
    render(); return;
  }

  if (state == AppState::SETTINGS && key == 'D') { state = AppState::SECRET_MENU; render(); }
}

// --------------------------------- Setup ------------------------------------
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  prefs.begin("ai-calc", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("wifi_pw", "");
  const char *names[] = {"key_openai", "key_deepseek", "key_gemini", "key_claude"};
  for (int i = 0; i < 4; ++i) {
    apiKeys[i] = prefs.getString(names[i], "");
    aiModels[i] = prefs.getString((String("model") + i).c_str(), aiModels[i]);
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (wifiSsid.length()) { WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str()); wifiConnecting = true; wifiAttemptStarted = millis(); lastWifiAttempt = millis(); }
  render();
}

void loop() {
  char key = keypad.getKey();
  if (key) handleKey(key);
  serviceWifi();
  if (millis() - lastStatusUpdate >= STATUS_REFRESH_MS) {
    lastStatusUpdate = millis();
    if (state == AppState::CALCULATOR) drawStatusBar();
  }
  delay(2); // yield to Wi-Fi/RTOS; no long blocking loop
}
