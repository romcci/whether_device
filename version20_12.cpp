/*
 * Arduino NANO + BME280 + OLED 128x64
 * Версия с библиотекой Adafruit_SSD1306 для OLED
 * Выводит температуру, влажность, давление и высоту каждые 3 минуты
 * Отслеживает минимальную температуру и сохраняет её в EEPROM
 * Вычисляет барометрическую тенденцию за час
 * Сброс минимальной температуры - удержание кнопки 3 секунды
 * Включение/выключение экрана - короткое нажатие кнопки
 * 
 * Подключение BME280:
 * VCC -> 3.3V (ВАЖНО! BME280 работает на 3.3V)
 * GND -> GND
 * SCL -> A5
 * SDA -> A4
 * 
 * Подключение OLED (I2C):
 * VCC -> 5V (или 3.3V)
 * GND -> GND
 * SCL -> A5 (общая шина с BME280)
 * SDA -> A4 (общая шина с BME280)
 * 
 * Подключение кнопки:
 * Один контакт -> D2
 * Другой контакт -> GND
 */

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <EEPROM.h>

// Настройки дисплея SSD1306
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// Пин кнопки
#define BUTTON_PIN 2

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280 bme;

// Адреса в EEPROM
#define EEPROM_ADDR_MIN_TEMP 0
#define EEPROM_ADDR_INIT_FLAG 4

const unsigned long INTERVAL = 180000; // 3 минуты
const unsigned long HOUR = 3600000; // 1 час
const unsigned long BUTTON_HOLD_TIME = 3000; // 3 секунды для сброса
const unsigned long DEBOUNCE_DELAY = 50; // Антидребезг

#define SEA_LEVEL_PRESSURE_HPA 1013.25

unsigned long previousMillis = 0;
unsigned long lastDebounceTime = 0;

float minTemperature = 999.0;
float currentTemperature = 0.0;

// Массив для хранения давления (20 измерений за час)
#define PRESSURE_SAMPLES 20
float pressureHistory[PRESSURE_SAMPLES];
byte pressureIndex = 0;
unsigned long firstMeasurementTime = 0;

float pressureTrend = 0.0;

// Переменные для обработки кнопки
unsigned long buttonPressStartTime = 0;
bool buttonPressed = false;
bool buttonWasPressed = false;
bool lastButtonState = HIGH;
bool displayOn = true; // Состояние экрана

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Инициализация OLED дисплея
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while(1); // Зависаем навсегда
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 25);
  display.println(F("Inizializacija..."));
  display.display();
  delay(500);
  
  // Инициализация BME280 (адрес 0x76 или 0x77)
  if (!bme.begin(0x76)) {
    if (!bme.begin(0x77)) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(F("OSHIBKA!"));
      display.setCursor(0, 12);
      display.println(F("BME280 ne naiden"));
      display.setCursor(0, 24);
      display.println(F("Prover'te:"));
      display.setCursor(0, 36);
      display.println(F("SDA->A4,SCL->A5"));
      display.setCursor(0, 48);
      display.println(F("3.3V -> BME280"));
      display.display();
      while (1);
    }
  }
  
  // Настройка режима работы BME280
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF);
  
  // Инициализация массива давления
  for (byte i = 0; i < PRESSURE_SAMPLES; i++) {
    pressureHistory[i] = 0;
  }
  
  loadMinTemperature();
  
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 25);
  display.println(F("Gotovo!"));
  display.display();
  delay(1500);
  
  pressureTrend = 0;
  firstMeasurementTime = millis();
  displayMeasurements();
}

void loop() {
  unsigned long currentMillis = millis();
  
  handleButton();
  
  // Проверяем, прошло ли 3 минуты
  if (currentMillis - previousMillis >= INTERVAL) {
    previousMillis = currentMillis;
    displayMeasurements();
  }
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  unsigned long currentMillis = millis();
  
  // Антидребезг
  if (reading != lastButtonState) {
    lastDebounceTime = currentMillis;
  }
  
  if ((currentMillis - lastDebounceTime) > DEBOUNCE_DELAY) {
    bool buttonState = (reading == LOW);
    
    if (buttonState && !buttonPressed) {
      // Кнопка только что нажата
      buttonPressed = true;
      buttonPressStartTime = currentMillis;
      buttonWasPressed = false;
    }
    
    if (buttonState && buttonPressed) {
      // Кнопка удерживается
      unsigned long holdTime = currentMillis - buttonPressStartTime;
      
      // Показываем прогресс если удерживается >= 500ms
      if (holdTime >= 500 && !buttonWasPressed) {
        showResetProgress(holdTime);
      }
      
      // Если кнопка удерживается 3 секунды - сброс минимума
      if (holdTime >= BUTTON_HOLD_TIME && !buttonWasPressed) {
        buttonWasPressed = true;
        resetMinTemperature();
      }
    }
    
    if (!buttonState && buttonPressed) {
      // Кнопка отпущена
      unsigned long holdTime = currentMillis - buttonPressStartTime;
      buttonPressed = false;
      
      // Короткое нажатие - переключение экрана
      if (holdTime < 500) {
        toggleDisplay();
      } 
      // Если отпущена до 3 секунд и не был сброс - возвращаем экран
      else if (!buttonWasPressed) {
        if (displayOn) {
          displayMeasurements();
        }
      }
    }
  }
  
  lastButtonState = reading;
}

void toggleDisplay() {
  displayOn = !displayOn;
  
  if (displayOn) {
    // Включаем экран и показываем данные
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayMeasurements();
  } else {
    // Выключаем экран
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

void showResetProgress(unsigned long holdTime) {
  if (!displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  
  display.setCursor(10, 10);
  display.println(F("Sbros Min Temp?"));
  
  display.setCursor(10, 25);
  display.print(F("Uderzhivajte: "));
  display.print((BUTTON_HOLD_TIME - holdTime) / 1000 + 1);
  display.print(F("s"));
  
  // Прогресс-бар
  int barWidth = (holdTime * 108) / BUTTON_HOLD_TIME;
  display.drawRect(10, 45, 108, 10, SSD1306_WHITE);
  display.fillRect(10, 45, barWidth, 10, SSD1306_WHITE);
  
  display.display();
}

void resetMinTemperature() {
  minTemperature = currentTemperature;
  saveMinTemperature();
  
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(15, 15);
  display.println(F("SBROS!"));
  display.setTextSize(1);
  display.setCursor(10, 40);
  display.print(F("Min temp: "));
  display.print(minTemperature, 1);
  display.print(F("C"));
  display.display();
  delay(2000);
  
  if (displayOn) {
    displayMeasurements();
  } else {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

void loadMinTemperature() {
  byte initFlag;
  EEPROM.get(EEPROM_ADDR_INIT_FLAG, initFlag);
  
  if (initFlag == 0xAA) {
    EEPROM.get(EEPROM_ADDR_MIN_TEMP, minTemperature);
  } else {
    minTemperature = 999.0;
  }
}

void saveMinTemperature() {
  EEPROM.put(EEPROM_ADDR_MIN_TEMP, minTemperature);
  byte initFlag = 0xAA;
  EEPROM.put(EEPROM_ADDR_INIT_FLAG, initFlag);
}

void calculatePressureTrend(float currentPressure) {
  pressureHistory[pressureIndex] = currentPressure;
  
  if (pressureIndex == PRESSURE_SAMPLES - 1) {
    // Линейная регрессия для вычисления тренда
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    byte n = PRESSURE_SAMPLES;
    
    for (byte i = 0; i < n; i++) {
      float x = i;
      float y = pressureHistory[i];
      sumX += x;
      sumY += y;
      sumXY += x * y;
      sumX2 += x * x;
    }
    
    float slope = (n * sumXY - sumX * sumY) / (n * sumX2 - sumX * sumX);
    pressureTrend = slope * 20.0;
  }
  
  pressureIndex = (pressureIndex + 1) % PRESSURE_SAMPLES;
}

const char* getPressureTrendText() {
  if ((millis() - firstMeasurementTime) < HOUR) {
    return "Wait";
  }
  
  if (pressureTrend > 1.5) {
    return "Rast";
  } else if (pressureTrend > 0.5) {
    return "Rost";
  } else if (pressureTrend > -0.5) {
    return "Stab";
  } else if (pressureTrend > -1.5) {
    return "Pad";
  } else {
    return "Spad";
  }
}

void displayMeasurements() {
  // Не обновляем экран если он выключен
  if (!displayOn) {
    // Но измерения проводим в любом случае
    bme.takeForcedMeasurement();
    
    float temperature = bme.readTemperature();
    float pressure = bme.readPressure();
    float pressureMmHg = pressure * 0.00750062;
    
    currentTemperature = temperature;
    
    if (!isnan(temperature) && !isnan(pressure)) {
      calculatePressureTrend(pressureMmHg);
      
      if (temperature < minTemperature) {
        minTemperature = temperature;
        saveMinTemperature();
      }
    }
    return;
  }
  
  // Принудительное измерение для режима FORCED
  bme.takeForcedMeasurement();
  
  // Чтение данных с BME280
  float temperature = bme.readTemperature();
  float pressure = bme.readPressure();
  float pressureMmHg = pressure * 0.00750062;
  float humidity = bme.readHumidity();
  float altitude = bme.readAltitude(SEA_LEVEL_PRESSURE_HPA);
  
  currentTemperature = temperature;
  
  // Проверка корректности данных
  bool sensorError = isnan(temperature) || isnan(pressure) || isnan(humidity);
  
  // Вычисление барометрической тенденции
  if (!sensorError) {
    calculatePressureTrend(pressureMmHg);
    
    // Проверка и обновление минимальной температуры
    if (temperature < minTemperature) {
      minTemperature = temperature;
      saveMinTemperature();
    }
  }
  
  // Отрисовка на OLED
  display.clearDisplay();
  
  if (sensorError) {
    display.setTextSize(1);
    display.setCursor(10, 5);
    display.println(F("OSHIBKA DATCHIKA"));
    display.setCursor(5, 20);
    display.println(F("Prover'te podkluchenie"));
    display.setCursor(15, 35);
    display.println(F("BME280 i pitanie"));
    display.setCursor(25, 50);
    display.println(F("3.3V -> BME280"));
  } else {
    // ТЕМПЕРАТУРА (крупно)
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print(temperature, 1);
    
    // Минимальная температура
    display.setCursor(80, 0);
    display.print(minTemperature, 1);
    display.setTextSize(1);
    
    // Разделительная линия
    display.drawLine(0, 18, 128, 18, SSD1306_WHITE);
    
    // ВЛАЖНОСТЬ
    display.setCursor(0, 22);
    display.print(F("Vlazhn: "));
    display.print(humidity, 0);
    display.print(F("%"));
    
    // ДАВЛЕНИЕ
    display.setCursor(0, 32);
    display.print(F("Davl:   "));
    display.print(pressureMmHg, 1);
    display.print(F(" mmHg"));
    
    // ТЕНДЕНЦИЯ
    display.setCursor(0, 42);
    display.print(F("Trend:  "));
    display.print(pressureTrend, 1);
    display.print(F(" ("));
    display.print(getPressureTrendText());
    display.print(F(")"));
    
    // ВЫСОТА
    display.setCursor(0, 52);
    display.print(F("Visota: "));
    display.print(altitude, 0);
    display.print(F(" m"));
  }
  
  display.display();
}
