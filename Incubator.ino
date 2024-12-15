#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Servo.h> // Подключаем библиотеку для управления сервоприводами
#include <DHT.h>  // Подключаем библиотеку для работы с датчиком DHT11

// Параметры Wi-Fi
const char* ssid = "ASUS_B4";
const char* password = "5560832b";

// Пины
#define ONE_WIRE_BUS D4  // DS18B20 на пине D4
#define HEATER_PIN D3    // Нагревательный элемент на пине D3
#define SERVO1_PIN D0    // Сервопривод 1 на пине D0
#define SERVO2_PIN D1    // Сервопривод 2 на пине D1
#define DHT_PIN D7       // Датчик DHT11 на пине D7

// Настройки температуры
#define TARGET_TEMP 37.5     // Целевая температура для инкубатора
#define TEMP_HYSTERESIS 0.5  // Гистерезис (погрешность)

// Интервал для смены положения сервоприводов (по умолчанию 2 часа)
unsigned long SERVO_INTERVAL = 2 * 60 * 60 * 1000;  // 2 часа = 2 * 60 * 60 * 1000 миллисекунд

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Инициализация датчика DHT11
DHT dht(DHT_PIN, DHT11);

// Веб-сервер
AsyncWebServer server(80);

// Глобальные переменные
float currentTemp = 0.0;  // Текущая температура
float humidity = 0.0;     // Текущая влажность
bool heaterState = false; // Состояние нагревателя
unsigned long incubationStart = 0; // Время начала инкубации
unsigned long lastServoTime = 0;   // Время последнего изменения сервоприводов
int servoState = 0;  // Состояние сервоприводов (0 - первая конфигурация, 1 - вторая конфигурация)

Servo servo1; // Объект для управления первым сервоприводом
Servo servo2; // Объект для управления вторым сервоприводом

void setup() {
  Serial.begin(115200);

  // Инициализация датчика температуры
  sensors.begin();
  dht.begin();  // Инициализация датчика DHT11
  Serial.println("DS18B20 и DHT11 датчики готовы.");

  // Настройка нагревательного элемента
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  // Инициализация сервоприводов
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);

  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi подключено!");
  Serial.print("IP-адрес: ");
  Serial.println(WiFi.localIP());

  // Установить начальное время инкубации
  incubationStart = millis();

  // Обработчик маршрута "/data" для JSON
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    unsigned long elapsedTime = millis() - incubationStart;
    int incubationDay = elapsedTime / (1000 * 60 * 60 * 24); // Перевод в дни

    // Время с последнего поворота и время до следующего
    unsigned long timeSinceLastTurn = millis() - lastServoTime;
    unsigned long timeToNextTurn = SERVO_INTERVAL - timeSinceLastTurn;

    // Преобразуем в чч:мм:сс
    String timeSinceLastTurnStr = millisToTimeFormat(timeSinceLastTurn);
    String timeToNextTurnStr = millisToTimeFormat(timeToNextTurn);

    String json = "{";
    json += "\"temperature\":" + String(currentTemp) + ",";
    json += "\"humidity\":" + String(humidity) + ",";  // Добавляем влажность
    json += "\"heater\":" + String(heaterState ? "true" : "false") + ",";
    json += "\"day\":" + String(incubationDay) + ",";
    json += "\"target_temp\":" + String(TARGET_TEMP) + ",";
    json += "\"time_since_last_turn\":\"" + timeSinceLastTurnStr + "\",";
    json += "\"time_to_next_turn\":\"" + timeToNextTurnStr + "\"";
    json += "}";

    request->send(200, "application/json", json);
  });

  // Обработчик маршрута "/set_interval" для изменения интервала переворота
  server.on("/set_interval", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("interval", true)) {
      String intervalStr = request->getParam("interval", true)->value();
      unsigned long newInterval = intervalStr.toInt() * 60 * 60 * 1000; // Переводим в миллисекунды (час в миллисекунды)

      if (newInterval > 0) {
        SERVO_INTERVAL = newInterval;
        String response = "Новый интервал переворота: " + String(SERVO_INTERVAL / (60 * 60 * 1000)) + " час(ов).";
        request->send(200, "text/plain; charset=UTF-8", response); // Указание кодировки
        Serial.println("Новый интервал переворота: " + String(SERVO_INTERVAL / (60 * 60 * 1000)) + " час(ов).");
        
        // Перенаправляем обратно на главную страницу
        request->redirect("/");
      } else {
        request->send(400, "text/plain; charset=UTF-8", "Неверное значение интервала."); // Указание кодировки
      }
    } else {
      request->send(400, "text/plain; charset=UTF-8", "Интервал не указан."); // Указание кодировки
    }
  });

  // Главная страница
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html; charset=UTF-8", generateWebPage()); // Указание кодировки
  });

  // Запуск сервера
  server.begin();
}

void loop() {
  // Чтение температуры с датчика DS18B20
  sensors.requestTemperatures();
  currentTemp = sensors.getTempCByIndex(0);

  // Чтение температуры и влажности с датчика DHT11
  humidity = dht.readHumidity();

  // Если данные с датчика DHT11 невалидны, выведем ошибку
  if (isnan(humidity)) {
    Serial.println("Ошибка чтения данных с DHT11!");
  }

  // Управление нагревателем
  if (currentTemp < TARGET_TEMP - TEMP_HYSTERESIS) {
    digitalWrite(HEATER_PIN, HIGH);
    heaterState = true;
  } else if (currentTemp > TARGET_TEMP + TEMP_HYSTERESIS) {
    digitalWrite(HEATER_PIN, LOW);
    heaterState = false;
  }

  // Управление сервоприводами
  unsigned long currentMillis = millis();
  if (currentMillis - lastServoTime >= SERVO_INTERVAL) {
    if (servoState == 0) {
      // Первая конфигурация
      servo1.write(0);   // Устанавливаем угол сервопривода 1 в 0 градусов
      servo2.write(90);  // Устанавливаем угол сервопривода 2 в 90 градусов
    } else {
      // Вторая конфигурация
      servo1.write(90);  // Устанавливаем угол сервопривода 1 в 90 градусов
      servo2.write(0);   // Устанавливаем угол сервопривода 2 в 0 градусов
    }

    // Обновляем состояние сервоприводов
    servoState = (servoState + 1) % 2;  // Переход между двумя конфигурациями (0 и 1)
    lastServoTime = currentMillis;  // Обновляем время последней смены конфигурации
  }

  // Отладочная информация
  Serial.print("Температура: ");
  Serial.print(currentTemp);
  Serial.print(" °C, Влажность: ");
  Serial.print(humidity);
  Serial.print("%, Нагреватель: ");
  Serial.println(heaterState ? "ВКЛ" : "ВЫКЛ");

  delay(1000);
}

// Функция для преобразования миллисекунд в формат чч:мм:сс
String millisToTimeFormat(unsigned long millis) {
  unsigned long seconds = millis / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;

  minutes = minutes % 60;
  seconds = seconds % 60;

  String timeStr = String(hours) + ":" + (minutes < 10 ? "0" : "") + String(minutes) + ":" + (seconds < 10 ? "0" : "") + String(seconds);
  return timeStr;
}

// Генерация HTML страницы
String generateWebPage() {
  return R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset="UTF-8">
      <title>Инкубатор</title>
      <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
      <style>
        body { font-family: Arial, sans-serif; text-align: center; }
        canvas { max-width: 100%; margin: 20px auto; }
        h1, h2 { margin: 10px; }
      </style>
    </head>
    <body>
      <h1>Инкубатор</h1>
      <h2>День инкубации: <span id="day">0</span></h2>
      <h2>Текущая температура: <span id="temperature">0.0</span> °C</h2>
      <h2>Целевая температура: <span id="target_temp">37.5</span> °C</h2>
      <h2>Нагреватель: <span id="heater">Отключён</span></h2>
      <h2>Текущая влажность: <span id="humidity">0</span>%</h2> <!-- Добавляем влажность -->
      <h2>Время с последнего поворота: <span id="time_since_last_turn">00:00:00</span></h2>
      <h2>Время до следующего поворота: <span id="time_to_next_turn">00:00:00</span></h2>

      <form action="/set_interval" method="POST" onsubmit="return updateInterval();">
        <label for="interval">Интервал переворота (часы): </label>
        <input type="number" id="interval" name="interval" value="2" min="1">
        <input type="submit" value="Обновить">
      </form>

      <canvas id="tempChart"></canvas>
      <script>
        const ctx = document.getElementById('tempChart').getContext('2d');
        const tempData = { labels: [], datasets: [
          { label: 'Температура (°C)', data: [], borderColor: 'blue', fill: false }
        ]};
        const tempChart = new Chart(ctx, { type: 'line', data: tempData, options: { responsive: true } });

        async function fetchData() {
          const response = await fetch('/data');
          const data = await response.json();

          document.getElementById('temperature').textContent = data.temperature.toFixed(1);
          document.getElementById('humidity').textContent = data.humidity.toFixed(1); // Выводим влажность
          document.getElementById('heater').textContent = data.heater ? 'Включён' : 'Отключён';
          document.getElementById('day').textContent = data.day;
          document.getElementById('target_temp').textContent = data.target_temp;
          document.getElementById('time_since_last_turn').textContent = data.time_since_last_turn;
          document.getElementById('time_to_next_turn').textContent = data.time_to_next_turn;

          const time = new Date().toLocaleTimeString();
          tempData.labels.push(time);
          tempData.datasets[0].data.push(data.temperature);

          if (tempData.labels.length > 20) {
            tempData.labels.shift();
            tempData.datasets[0].data.shift();
          }

          tempChart.update();
        }

        function updateInterval() {
          const interval = document.getElementById('interval').value;
          fetch('/set_interval', {
            method: 'POST',
            body: new URLSearchParams({ interval: interval })
          })
          .then(response => response.text())
          .then(data => {
            alert(data);
            fetchData(); // Обновим данные на странице
          });

          return false; // предотвращаем перезагрузку страницы
        }

        setInterval(fetchData, 5000);
        fetchData();
      </script>
    </body>
    </html>
  )rawliteral";
}
