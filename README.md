# ESP32-S3 AI Calculator

🇷🇺 **Русский язык** | 🇬🇧 **English**


## 🇷🇺 Русский

### О проекте

Многофункциональное портативное устройство на базе микроконтроллера **ESP32-S3**, объединяющее в себе инженерный калькулятор, поддержку Wi-Fi, синхронизацию времени по NTP, мониторинг заряда батареи и прямую интеграцию с популярными языковыми моделями (OpenAI, DeepSeek, Google Gemini, Anthropic Claude). Управление осуществляется с помощью матричной клавиатуры 4x4 и цветного TFT-дисплея ST7789.

### Основные возможности

* **Инженерный калькулятор:** Выполнение базовых арифметических операций с обработкой ошибок деления на ноль и поддержкой цепочки вычислений.


* **AI-ассистенты:** Отправка текстовых запросов к OpenAI, DeepSeek, Gemini и Claude с отображением ответа на экране.


* **Wi-Fi и NTP:** Сканирование доступных сетей, ручной ввод SSID и пароля (реализован мультитап с клавиатуры) и автоматическая синхронизация времени через NTP.


* **Скрытое меню:** Доступ к настройкам Wi-Fi, ключам API и системной информации по секретной комбинации клавиш (`ABCD#`).


* **Контроль питания:** Измерение напряжения батареи через встроенный АЦП с учетом резистивного делителя.


* **Безопасность:** Проверка TLS-сертификатов с использованием встроенного пакета корневых сертификатов ESP-IDF (`esp_crt_bundle`) без использования небезопасного режима (`setInsecure`).



### Необходимые библиотеки

Для сборки проекта в Arduino IDE установите следующие зависимости через менеджер библиотек:

* `Adafruit GFX Library`


* `Adafruit ST7735 and ST7789 Library`


* `Keypad`


* `ArduinoJson (7.x)`


* **Пакет плат:** Espressif ESP32 (актуальная стабильная версия)



### Конфигурация пинов (по умолчанию)

* **TFT Дисплей (ST7789, 240x320):** CS = 10, DC = 9, RST = 8, SCLK = 12, MOSI = 11


* **АЦП батареи:** Пин 4 (`BATTERY_ADC_PIN`)


* **Матричная клавиатура 4x4:**
* Строки (Rows): пины 5, 6, 7, 15


* Столбцы (Cols): пины 16, 17, 18, 21



## 🇬🇧 English

### About the Project

An advanced multi-functional handheld device powered by the **ESP32-S3** microcontroller, combining a calculator, Wi-Fi connectivity, NTP time synchronization, battery monitoring, and direct integration with major AI APIs (OpenAI, DeepSeek, Google Gemini, Anthropic Claude). It features a 4x4 matrix keypad and an ST7789 color TFT display.

### Key Features

* **Calculator:** Basic arithmetic operations with division-by-zero error handling and chained calculations.


* **AI Assistants:** Connect to OpenAI, DeepSeek, Gemini, and Claude to send prompts and read responses directly on the screen.


* **Wi-Fi & NTP:** Network scanning, manual SSID/password entry via keypad multi-tap, and NTP time synchronization.


* **Secret Menu:** Access configuration settings via a secret key sequence (`ABCD#`).


* **Power Management:** Battery voltage tracking via ADC using a voltage divider configuration.


* **Security:** HTTPS/TLS certificate verification enabled using the native ESP-IDF root CA bundle (`esp_crt_bundle`).



### Required Dependencies

* `Adafruit GFX Library`


* `Adafruit ST7735 and ST7789 Library`


* `Keypad`


* `ArduinoJson (7.x)`


* **Board Package:** Espressif ESP32 (current stable)



## 📄 Автор и Лицензия / Author & License

* **Автор / Author:** Chalov Matvey
* **Лицензия / License:** MIT License
