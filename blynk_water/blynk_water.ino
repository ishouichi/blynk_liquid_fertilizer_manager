#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <time.h>
#include "OneWire.h"
#include "DallasTemperature.h"
#define JST     3600* 9

// blynk
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
BlynkTimer timer;

// 日照センサ
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_TSL2591.h"

//------------------------------------------------------
// 潅水自動化
//------------------------------------------------------
Adafruit_TSL2591 tsl = Adafruit_TSL2591(2591);

// wifi
char auth[] = "blynk_authを書く";
char ssid[] = "wifi_ssidを書く";
char pass[] = "wifi_passを書く";

// 日照計算
double total_sun;
double base_total_sun = 0;
double max_sun;

// 潅水計算
unsigned long prev_advancedRead_time;
unsigned long watering_time = 0;
unsigned long watering_time_at = 0; //朝一の潅水
double watering = 0;

// ポンプ出力
#define WATER_PIN_OUTPUT 2

//------------------------------------------------------
// 液肥自動化
//------------------------------------------------------

// 温度
#define TEMP_INPUT 14

// EC
#define EC_INPUT 35
#define EC_POWER 33

float Temperature = 25;
float EC = 0;
float EC25 = 0;
float R1 = 500;
float Ra = 25;
float K = 2.3;

//int Ra = 150;
//float K = 0.64;

float  TemperatureCoef = 0.019;
float raw = 0;
float Vin = 2.7;
float Vdrop = 0;
float Rc = 0;
float setEC = 2;

// MOTER
#define FERTILIZER_OUTPUT 27
float pump_time = 1; // 20秒 液肥を投入

// 水道 電磁弁
#define WATER_LEVEL_INPUT 34
#define WATER_LEVEL_POWER 25
#define WATER_LEVEL_OUTPUT 26

// 液肥が出ているかチェック
#define FERTILIZER_OUTPUT_CHECK_INPUT 32

// 液肥作成時間かどうか
boolean fertilizer_create_start = 0;
double fertilizer_add_time = 0;
int readEC_count = 0;
int fertilizer_add_term = 0;

OneWire oneWire(TEMP_INPUT);
DallasTemperature tempSensor(&oneWire);
//------------------------------------------------------


void setup()
{
  Serial.begin(115200);

  Blynk.begin(auth, ssid, pass);
  
  //------------------------------------------------------
  // 潅水自動化
  //------------------------------------------------------
  Blynk.syncVirtual(V0);// 冠水日射量初期化
  Blynk.syncVirtual(V3);// 冠水時間初期化
  Blynk.syncVirtual(V11);// 光合成日射量上限
  Blynk.syncVirtual(V13);// 冠水時間指定時刻初期化
  
  // 日照センサ
  tsl.begin();
  configureSensor();
  
  pinMode(WATER_PIN_OUTPUT, OUTPUT);


  //------------------------------------------------------


  //------------------------------------------------------
  // 液肥自動化
  //------------------------------------------------------
  pinMode(EC_INPUT, INPUT);
  pinMode(WATER_LEVEL_INPUT, INPUT);
  pinMode(FERTILIZER_OUTPUT_CHECK_INPUT, INPUT);
  pinMode(WATER_LEVEL_OUTPUT, OUTPUT);
  pinMode(EC_POWER, OUTPUT);
  pinMode(FERTILIZER_OUTPUT, OUTPUT);
  pinMode(WATER_LEVEL_POWER, OUTPUT);
  
  digitalWrite(WATER_LEVEL_POWER, LOW);
  digitalWrite(EC_POWER, LOW); 
  digitalWrite(FERTILIZER_OUTPUT, LOW);
  digitalWrite(WATER_LEVEL_OUTPUT, HIGH);
  
  tempSensor.begin();
  
  Blynk.syncVirtual(V4);//  液肥作成時間初期化
  Blynk.syncVirtual(V5);//  液肥追加間隔初期化
  Blynk.syncVirtual(V6);//  液肥追加間隔初期化
  Blynk.syncVirtual(V7);//  作成する液肥のEC初期化
  
  R1 = (R1 + Ra); 
  
  //------------------------------------------------------


  // スレッド開始
  timer.setInterval(1000L, readSunPower);
  timer.setInterval(60000L, readEC);
  timer.setInterval(1000L, addFertilizer);
  timer.setInterval(60000L, addWater);
  timer.setInterval(1000L, fertilizerOutputCheck);
  
  
  
}

void loop()
{
  Blynk.run();
  timer.run();  
}

//------------------------------------------------------
// 再起動
//------------------------------------------------------
BLYNK_WRITE(V9)
{
  if (param.asInt() == 1)
  {
    ESP.restart();
  }
}

BLYNK_APP_DISCONNECTED()
{
  while(!Blynk.connected())
  {
    // 再接続
    Blynk.connect();
  }
}

//------------------------------------------------------
// 液肥自動化
//------------------------------------------------------

BLYNK_WRITE(V4)
{
  fertilizer_create_start = param.asInt();
  if (fertilizer_create_start)
  {
    Serial.println("液肥タイマー - 開始");    
  } else {
    Serial.println("液肥タイマー - 終了");  
  }
}

BLYNK_WRITE(V6)
{
  fertilizer_add_time = param.asDouble() * 1000;
  Serial.print("原液追加時間: ");
  Serial.print(param.asDouble());
  Serial.println("秒");

  // 間違えて時間を長くしてしまったときのために液肥を止める
  timer.setTimer(fertilizer_add_time, stopFertilizer, 1);
}

BLYNK_WRITE(V7)
{
  setEC = param.asInt();
  Serial.print("液肥目標値 EC: ");
  Serial.println(param.asDouble());
}

BLYNK_WRITE(V5)
{
  fertilizer_add_term = param.asFloat();
  Serial.print("液肥が混ざるまでの時間: ");
  Serial.print(param.asDouble());
  Serial.println("分");
}

// 1分に一度読む
void readEC()
{
  tempSensor.requestTemperaturesByIndex(0);

  Temperature = tempSensor.getTempCByIndex(0);
 
  digitalWrite(EC_POWER, HIGH);
  raw = analogRead(EC_INPUT);
  raw = analogRead(EC_INPUT); // 2回やらないとダメみたい
  digitalWrite(EC_POWER, LOW);

  Vdrop = (Vin * raw) / 4095.0;  
  Rc = (Vdrop * R1) / (Vin - Vdrop);  
  Rc = Rc - Ra; //acounting for Digital Pin Resitance
  EC = 1000 / (Rc * K);
  
  EC25  =  EC / (1 + TemperatureCoef * (Temperature - 25.0));

  Serial.print("Temperature: ");
  Serial.print(Temperature);
  Serial.println(" C");
  Serial.print("EC: ");
  Serial.println(EC25);
  Blynk.virtualWrite(V8, EC25);

  // 一度読む毎に足す
  if (fertilizer_create_start)
  {
    readEC_count = readEC_count + 1;
  }
}

// 指定回数に達したときに液肥を追加
void addFertilizer()
{
  if (fertilizer_create_start && readEC_count > (fertilizer_add_term -1)  && EC25 < setEC && EC25 > 0) {
    digitalWrite(FERTILIZER_OUTPUT, HIGH);
    Serial.print(F("[ ")); Serial.print(millis()); Serial.print(F(" ms ] "));
    Serial.println("液肥追加 - 開始");

    readEC_count = 0;
    
    // 液肥を止める
    timer.setTimer(fertilizer_add_time, stopFertilizer, 1);
   

  } 
}

void stopFertilizer()
{
    Serial.print(F("[ ")); Serial.print(millis()); Serial.print(F(" ms ] "));
    Serial.println("液肥追加 - 終了");
    digitalWrite(FERTILIZER_OUTPUT, LOW);
    readEC_count = 0;
}

//------------------------------------------------------


//------------------------------------------------------
// 潅水自動化
//------------------------------------------------------

BLYNK_WRITE(V0)
{
  base_total_sun = param.asDouble();
  Serial.print("冠水日射量: ");
  Serial.println(base_total_sun);
}

BLYNK_WRITE(V3)
{
  watering_time = param.asDouble() * 60000;
  Serial.print("冠水時間: ");
  Serial.println(watering_time);
}

BLYNK_WRITE(V11)
{
  max_sun = param.asDouble();
  Serial.print("光合成日射量上限: ");
  Serial.println(max_sun);
}

BLYNK_WRITE(V10)
{
  if (param.asInt() == 1)
  {
    watering_time = watering_time_at;
    start_watering();
    Blynk.syncVirtual(V3);
  }
}

BLYNK_WRITE(V13)
{
  watering_time_at = param.asDouble() * 60000;
  Serial.print("指定時刻潅水時間: ");
  Serial.println(watering_time_at);
}


void configureSensor(void)
{
  // You can change the gain on the fly, to adapt to brighter/dimmer light situations
  tsl.setGain(TSL2591_GAIN_LOW);    // 1x gain (bright light)
  //tsl.setGain(TSL2591_GAIN_MED);      // 25x gain
  //tsl.setGain(TSL2591_GAIN_HIGH);   // 428x gain
  
  // Changing the integration time gives you a longer time over which to sense light
  // longer timelines are slower, but are good in very low light situtations!
  tsl.setTiming(TSL2591_INTEGRATIONTIME_100MS);  // shortest integration time (bright light)
  // tsl.setTiming(TSL2591_INTEGRATIONTIME_200MS);
  //tsl.setTiming(TSL2591_INTEGRATIONTIME_300MS);
  // tsl.setTiming(TSL2591_INTEGRATIONTIME_400MS);
  // tsl.setTiming(TSL2591_INTEGRATIONTIME_500MS);
  // tsl.setTiming(TSL2591_INTEGRATIONTIME_600MS);  // longest integration time (dim light)
}

void fertilizerOutputCheck()
{
  
  digitalWrite(WATER_LEVEL_POWER, HIGH);
  analogRead(FERTILIZER_OUTPUT_CHECK_INPUT); // 立ち上がりの時間を考慮
  if (analogRead(FERTILIZER_OUTPUT_CHECK_INPUT) > 0)
  {
    Blynk.virtualWrite(V12,base_total_sun);
  } else {
    Blynk.virtualWrite(V12,0);
  }
  digitalWrite(WATER_LEVEL_POWER, LOW);

  
}

void readSunPower(void)
{
  
  // More advanced data read example. Read 32 bits with top 16 bits IR, bottom 16 bits full spectrum
  // That way you can do whatever math and comparisons you want!
  uint32_t lum = tsl.getFullLuminosity();
  uint16_t ir, full;
  ir = lum >> 16;
  full = lum & 0xFFFF;
//  Serial.print(F("[ ")); Serial.print(millis()); Serial.print(F(" ms ] "));
//  Serial.print(F("IR: ")); Serial.print(ir);  Serial.print(F("  "));
//  Serial.print(F("Full: ")); Serial.print(full); Serial.print(F("  "));
//  Serial.print(F("Visible: ")); Serial.print(full - ir); Serial.print(F("  "));
//  Serial.print(F("Lux: ")); Serial.println(tsl.calculateLux(full, ir), 6);

  // 太陽光の場合の変換(http://www.photosynthesis.jp/light.html)
  // w/m2 (放射照度) = lux /54 /4.57
  // * 放射照度 に 秒 を掛けたものが積算日射量 MJ/m2
  // 2400 KJ/m2 毎に 冠水
  
  float sun_power;
  sun_power =  (millis() - prev_advancedRead_time) * tsl.calculateLux(full, ir) / (54 * 4.57 * 1000 * 1000);
  // 潅水中　かつ 変な値はやらない
  if (sun_power > 0 && !watering)
  {
    if (sun_power > max_sun)
    {
      total_sun = total_sun + max_sun;
    } else {
      total_sun = total_sun + sun_power; 
    }
  }

  Blynk.virtualWrite(V1,sun_power);
  Blynk.virtualWrite(V2,total_sun);  

  if (total_sun > base_total_sun)
  {
    Serial.println("日射量 FULL");
    start_watering();
  }

  prev_advancedRead_time = millis();
}

void start_watering()
{
  Serial.println("潅水 - 開始");
  total_sun = 0;
  digitalWrite(WATER_PIN_OUTPUT, HIGH);
  watering = 1;
  timer.setTimer(watering_time, stop_watering, 1);
}

void stop_watering()
{
  Serial.println("潅水 - 終了");
  digitalWrite(WATER_PIN_OUTPUT, LOW);
  watering = 0;
}


void addWater()
{  
  digitalWrite(WATER_LEVEL_POWER, HIGH);
  analogRead(WATER_LEVEL_INPUT); // 立ち上がりの時間を考慮
  if (fertilizer_create_start && analogRead(WATER_LEVEL_INPUT) == 0)
  {
    Serial.println("タンクに水を追加 - 開始");
    digitalWrite(WATER_LEVEL_OUTPUT, HIGH);
    timer.setTimer(30000L, stopWater, 1);
  } else {
    stopWater();
  }
  digitalWrite(WATER_LEVEL_POWER, LOW);
}

void stopWater()
{
  Serial.println("タンクに水を追加 - 終了");
  digitalWrite(WATER_LEVEL_OUTPUT, LOW);
}

//------------------------------------------------------
