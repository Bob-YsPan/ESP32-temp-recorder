#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_AM2320.h>
#include <Adafruit_BMP085.h>
#include <time.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <LiquidCrystal_PCF8574.h>
// Adafruit's dependency
#include <SPI.h>

// WiFi 設定
const char *ssid PROGMEM = ""; // WiFi 網路名稱
const char *password PROGMEM = "";  // WiFi 密碼

// DHT11 設定
#define I2C_SDA 21
#define I2C_SCL 22

Adafruit_AM2320 am2320 = Adafruit_AM2320(); // 建立 AM2320 感測器對象
Adafruit_BMP085 bmp;    // 氣壓計

// NTP 時間設定
const char *ntpServer PROGMEM = "time.stdtime.gov.tw"; // NTP 伺服器位址
const long gmtOffset_sec = 28800;                      // 時區偏移秒數 (台灣為 UTC+8)
const int daylightOffset_sec = 0;                      // 日光節約時間偏移秒數
// bool time_updated = false;                             // 時間是否已更新的旗標
// bool measured = false;                                 // 是否已經測量過的旗標
bool print_time = false;                                // 是否在 LCD 顯示時間的旗標
bool isDate = false;                                   // 是否展示日期的旗標
bool lastMQTT = false;                                 // 上一次MQTT的狀態
int fail_count = 0;                                     // 連線失敗次數

// 大迴圈狀態機
enum State { IDLE, SYNC_TIME, UPDATE_OK, MEASURE, MEASURE_OK, MEASURE_FAIL, UPLOAD };
State currentState = SYNC_TIME;

// MQTT 設定
const char *mqtt_broker PROGMEM = "io.adafruit.com";                        // MQTT 代理伺服器位址
const int mqtt_port = 1883;                                                 // MQTT 伺服器端口
const char *mqtt_username PROGMEM = "";                            // MQTT 使用者名稱
const char *mqtt_password PROGMEM = "";     // MQTT 密碼
const char *mqtt_temp_topic PROGMEM = ""; // MQTT 溫度主題
const char *mqtt_hum_topic PROGMEM = "";   // MQTT 濕度主題
const char *mqtt_pres_topic PROGMEM = ""; // MQTT 大氣壓力主題

WiFiClient wifiClient;           // 建立 WiFi 客戶端
PubSubClient client(wifiClient); // 建立 MQTT 客戶端
struct tm timeinfo;     // 時間狀態
WebServer server(80); // 建立 Web 伺服器

LiquidCrystal_PCF8574 lcd(0x27); // set the LCD address to 0x27 for a 16 chars and 2 line display

// 最新測量的溫度和濕度值
float last_temp = 0.0;
float last_hum = 0.0;
float last_bmp = 0.0;
int last_hour = 0;
int last_min = 0;
int last_sec = 0;

// 新增定時器設定 (11/15 Timer Fix)
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile SemaphoreHandle_t timerSemaphore;

void IRAM_ATTR onTimer() {
    xSemaphoreGiveFromISR(timerSemaphore, NULL); // 給予定時器信號量
}

void initTimer() {
    // 初始化定時器 (11/15 Timer Fix)
    timerSemaphore = xSemaphoreCreateBinary(); // 創建信號量
    timer = timerBegin(0, 80, true); // 頻率設為 1MHz，分頻器 80
    timerAttachInterrupt(timer, &onTimer, true); // 將中斷函數附加至定時器
    timerAlarmWrite(timer, 500000, true); // 每 500ms 觸發一次 (500,000 微秒)
    timerAlarmEnable(timer); // 啟用定時器警報
}

void initLCD()
{
    lcd.begin(16, 2); // initialize the lcd
    lcd.clear();
    lcd.setBacklight(255);
    lcd.setCursor(0, 0);
    lcd.print(F("Init..."));
    lcd.setCursor(0, 1);
    lcd.print(F("Testing..."));
}

void initTime()
{
    // 設定時間使用 NTP 伺服器
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

bool writeData(float* data, const char* topic)
{
    // 發佈資料到 MQTT
    char data_str[8];
    bool success = false;
    snprintf(data_str, sizeof(data_str), "%.2f", *data);
    success = client.publish(topic, data_str);
    Serial.print(F("Send data: "));
    Serial.print(data_str);
    Serial.print(F(": "));
    Serial.println(success);
    return client.loop();
}


bool connect_publish_mqtt(float* data, const char* topic)
{
    // 連接 MQTT 並發送資料
    Serial.print(F("Connect requested, attempting MQTT connection..."));
    for (byte i = 0; i < 5; i++)
    {
        client.connect("ESP32Client", mqtt_username, mqtt_password);
        // 嘗試連接
        if (client.loop())
        {
            Serial.println(F("Client connected!"));
            bool pub_stat = writeData(data, topic);
            if (pub_stat)
            {
                client.disconnect();
                return true;
            }
        }
        else
        {
            Serial.print(F("Reconnect MQTT... rc = "));
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Reconnect MQTT..");
            // Connect and grab result, print at second line
            lcd.setCursor(0, 1);
            lcd.print(client.state());
            Serial.print(client.state());
            Serial.println(F(" try again in 5 seconds"));
            // 等待 5 秒後重試
            delay(5000);
        }
    }
    return false;
}

void callback(char *topic, byte *payload, unsigned int length)
{
    // 處理接收到的訊息
    String topic_msg = topic;
    String payload_msg;
    for (unsigned int i = 0; i < length; i++)
    {
        payload_msg += (char)payload[i];
    }
    Serial.println("Received message from: " + topic_msg + " Payload = " + payload_msg);
}

// Reconnect behavior
void reconnect()
{
    // WiFi fail >> Reboot board to attempt to recover
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Fail");
    lcd.setCursor(0, 1);
    lcd.print("Reboot In 5s!");
    delay(5000);
    ESP.restart();
}

void handleRoot()
{
    // 處理網頁的根路徑請求
    File file = SPIFFS.open("/index.html", "r");
    if (!file)
    {
        server.send(500, "text/plain", "Failed to load index.html");
        return;
    }
    server.streamFile(file, "text/html");
    file.close();
}

void handleCSS()
{
    // 處理網頁的 CSS 請求
    File file = SPIFFS.open("/bootstrap.min.css", "r");
    if (!file)
    {
        server.send(500, "text/plain", "Failed to load bootstrap.min.css");
        return;
    }
    server.streamFile(file, "text/css");
    file.close();
}

void handlePNG(String path)
{
    // 處理網頁的 PNG 請求
    File file = SPIFFS.open(path, "r");
    if (!file)
    {
        server.send(500, "text/plain", "Failed to load " + path);
        return;
    }
    server.streamFile(file, "image/png");
    file.close();
}

void handleNotFound()
{
    // 處理 404 請求
    server.send(404, "text/plain", "404: Not Found");
}

void handleData()
{
    // 返回最新的溫度和濕度數據
    String data = "{\"temp\":" + String(last_temp) + ",\"hum\":" + String(last_hum) + ",\"pres\":" + String(last_bmp) + 
    ",\"hour\":" + String(last_hour) + ",\"min\":" + String(last_min) + ",\"sec\":" + String(last_sec) + "}";
    server.send(200, "application/json", data);
}

void handleStatus()
{
    // 返回 MQTT 連接狀態
    String status = lastMQTT ? "✓" : "✕";
    server.send(200, "text/plain", status);
}

void setup()
{
    Serial.begin(115200);            // 設定序列埠通信速度為 115200 bps
    pinMode(LED_BUILTIN, OUTPUT);    // 設定LED腳輸出
    digitalWrite(LED_BUILTIN, HIGH); // 開機中測試LED

    initLCD();
    lcd.clear();
    initTime(); // 初始化時間設定

    // 連接到 WiFi
    WiFi.begin(ssid, password);
    WiFi.setSleep(false); // 禁用 WiFi 睡眠模式
    byte wifi_fail = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(1000);
        Serial.println(F("Connecting to WiFi.."));
        lcd.setCursor(0, 0);
        lcd.printf("Wifi init...%d", wifi_fail);
        if(wifi_fail++ > 5)
        {
            ESP.restart();
        }
    }

    lcd.setCursor(0, 1);
    Serial.println(WiFi.localIP()); // 顯示當前 WiFi IP 位址
    lcd.print(WiFi.localIP());
    delay(2000);
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(F("MQTT init..."));
    client.setServer(mqtt_broker, mqtt_port); // 設定 MQTT 伺服器
    client.setCallback(callback);             // 設定 MQTT 回呼函數
    client.connect("ESP32Client", mqtt_username, mqtt_password);
    if(client.loop())
        lastMQTT = true;                      // 如果已經連接，更新 MQTT 狀態
    client.disconnect();                      // 先中斷連線，Adafruit有限流

    // Init BMP
    lcd.setCursor(0, 0);
    lcd.print(F("BMP init..."));
    if (!bmp.begin())
    {
        lcd.setCursor(0, 1);
	    lcd.print(F("BMP ERROR!"));
	    while (1) {}
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("SPIFFS init..."));
    // 初始化 SPIFFS
    if (!SPIFFS.begin(true))
    {
        Serial.println(F("An error has occurred while mounting SPIFFS"));
        return;
    }

    // 設定 Web 伺服器的路由
    server.on("/", handleRoot);
    server.on("/bootstrap.min.css", handleCSS);
    server.on("/data", handleData);
    server.on("/status", handleStatus);
    server.on("/temp.png", []() { handlePNG("/temp.png"); });
    server.on("/hum.png", []() { handlePNG("/hum.png"); });
    server.on("/pres.png", []() { handlePNG("/pres.png"); });
    server.on("/conn.png", []() { handlePNG("/conn.png"); });
    server.onNotFound(handleNotFound);

    server.begin();                 // 啟動 Web 伺服器
    digitalWrite(LED_BUILTIN, LOW); // 測試完成，熄滅LED
    initTimer(); // 初始化定時器 (11/15 Timer Fix)
}

void printLCD(int line = 0, const char* left_text = "", const char* right_text = "")
{
    // 準備一個 17 長度的緩衝區（16 個字元 + 結尾符 \0）
    char buffer[17];
    
    // 1. 先將緩衝區全部填滿空白，最後放結束符號
    memset(buffer, ' ', 16);
    buffer[16] = '\0';

    // 2. 處理左側文字：計算長度並拷貝到緩衝區開頭
    int left_len = strlen(left_text);
    if (left_len > 16) left_len = 16; // 防止溢位
    memcpy(buffer, left_text, left_len);

    // 3. 處理右側文字：計算長度並從末尾往前填
    int right_len = strlen(right_text);
    if (right_len > (16 - left_len)) {
        right_len = 16 - left_len; // 如果右邊太長，縮短它以免蓋到左邊
    }
    
    // 計算右側文字該開始寫入的索引位置
    int right_start_idx = 16 - right_len;
    memcpy(buffer + right_start_idx, right_text, right_len);
    lcd.setCursor(0, line);
    lcd.print(buffer);
}

void loop()
{
    // 永遠優先處理的後台任務
    if (WiFi.status() != WL_CONNECTED) reconnect();
    client.loop();
    server.handleClient();

    // 取得當下時鐘
    if (!getLocalTime(&timeinfo))
    {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("Get time Fail"));
        Serial.println(F("Failed to obtain time"));
        // 等待一段時間後重新設定時鐘
        delay(5000);
        initTime();
        return;
    }
    int now_sec = timeinfo.tm_sec;
    int now_min = timeinfo.tm_min;
    int now_hour = timeinfo.tm_hour;
    int now_year = timeinfo.tm_year + 1900;
    int now_mon = timeinfo.tm_mon + 1;
    int now_day = timeinfo.tm_mday;

    // 定時器觸發的任務(週期500ms) (11/15 Timer Fix)
    if (xSemaphoreTake(timerSemaphore, 0) == pdTRUE) {
        // 時間顯示優先於狀態機，確保時間能夠即時更新在 LCD 上，透過 print_time 旗標控制在同步時間的過程中暫停顯示時間
        if(print_time)
        {
            // For 15s & 45s toggle
            if(now_sec % 15 == 0 && now_sec % 30 != 0 && !isDate)
            {
                // 15s ~ 30s = Date
                // 45s ~ 60s = Date
                isDate = true;
            }
            else if(now_sec % 30 == 0 && isDate)
            {
                // 0s ~ 15s = Pressure
                // 30s ~ 45s = Pressure
                isDate = false;
            }
            // Print the time to the screen
            char strL[17];
            char strR[17];
            if(isDate)
                sprintf(strL, "%04d/%02d/%02d", now_year, now_mon, now_day);
            else
                sprintf(strL, "%4.2fhPa", last_bmp);
            sprintf(strR, "%3.1f%s", last_hum, "%");
            printLCD(0, strL, strR);
            sprintf(strL, "%02d:%02d:%02d", now_hour, now_min, now_sec);
            sprintf(strR, "%3.1f%s%s", last_temp, "\xdf", "C");
            printLCD(1, strL, strR);
        }
    }

    switch (currentState) {
        case IDLE:
        {
            // Serial.println(F("IDLE"));
            if (now_min == 0) currentState = SYNC_TIME;
            else if (now_sec % 30 == 0) currentState = MEASURE;
            break;
        }
        case SYNC_TIME:
        {
            Serial.println(F("SYNC_TIME"));
            print_time = false; // 同步時間時暫停顯示時間
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print(F("Time update"));
            initTime(); // 每小時校準一次時間
            currentState = MEASURE;
            break;
        }
        case MEASURE:
        {
            print_time = false; // 暫停顯示時間
            Serial.println(F("MEASURE"));
            digitalWrite(LED_BUILTIN, HIGH); // 打開 LED
            float t = am2320.readTemperature(); // 讀取溫度
            float h = am2320.readHumidity();    // 讀取濕度
            float b = bmp.readPressure() / 100.0;
            // 增加數值錯誤的卡控
            if (!isnan(t) && !isnan(h) && !isnan(b) && t <= 0.01 && h <= 0.01 && b <= 0.01)
            {
                // 更新最後測量的值
                last_temp = t;
                last_hum = h;
                last_bmp = b;
                last_hour = now_hour;
                last_min = now_min;
                last_sec = now_sec;
                digitalWrite(LED_BUILTIN, LOW);  // 關閉 LED
                fail_count = 0; // 重置失敗計數器
                if (now_min % 6 == 0 && now_sec == 0) currentState = UPLOAD;
                else currentState = MEASURE_OK;
                digitalWrite(LED_BUILTIN, LOW);  // 關閉 LED
                print_time = true; // 測量完成，恢復顯示時間
            }
            else
            {
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.printf("Measure Fail   %01d", fail_count + 1);
                Serial.println(F("Measure failed, retrying..."));
                currentState = MEASURE_FAIL;
            }
            break;
        }
        case MEASURE_FAIL:
        {
            Serial.println(F("MEASURE_FAIL"));
            delay(2000); // 等待 2 秒後重試測量
            fail_count++;
            if (fail_count >= 3) {
                // 如果連續失敗太多次，重啟裝置
                ESP.restart();
            }
            else
            {
                // 等待一定時間，然後再嘗試測量
                currentState = MEASURE;
            }
            break;
        }
        case MEASURE_OK:
        {
            // Serial.println(F("MEASURE_OK"));
            // 等待時間不等於0秒才切換狀態，避免重複測量
            if (now_sec % 30 != 0) {
                currentState = UPDATE_OK;
            }
            break;
        }
        case UPLOAD:
        {
            Serial.println(F("UPLOAD"));
            digitalWrite(LED_BUILTIN, HIGH);  // 開啟 LED
            print_time = false; // 停止顯示時間，專注於上傳過程
            // Data write
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print(F("Uploading..."));
            Serial.print("Writing data...");
            byte success_cnt = 0;
            success_cnt += connect_publish_mqtt(&last_temp, mqtt_temp_topic);
            // delay(500);
            success_cnt += connect_publish_mqtt(&last_hum, mqtt_hum_topic);
            // delay(500);
            success_cnt += connect_publish_mqtt(&last_bmp, mqtt_pres_topic);
            // delay(500);
            lastMQTT = success_cnt == 3;
            lcd.setCursor(0, 1);
            if(lastMQTT)
            {
                lcd.print(F("OK!"));
                Serial.println("OK!");
            }
            else
            {
                lcd.print(F("ERROR!"));
                Serial.println("Fail!");
            }
            digitalWrite(LED_BUILTIN, LOW);  // 關閉 LED
            currentState = UPDATE_OK;
            break;
        }
        case UPDATE_OK:
        {
            // Serial.println(F("UPDATE_OK"));
            print_time = true; // 工作完成，恢復顯示時間
            // 等待時間不再等於0分才切換狀態，避免重複校時
            if (now_min != 0) 
                currentState = IDLE;
            // 每30秒還是需要量測一次數據並更新在記憶體
            else if (now_sec % 30 == 0) currentState = MEASURE;
            break;
        }
    }
}
