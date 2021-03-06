/*
  Copyright (c) 2016, SODAQ
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its contributors
  may be used to endorse or promote products derived from this software without
  specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/

#include <Arduino.h>
#include <Wire.h>
#include "RTCTimer.h"
#include "RTCZero.h"
#include "Sodaq_RN2483.h"
#include "Sodaq_wdt.h"
#include "Config.h"
#include "BootMenu.h"
#include "ublox.h"
#include "MyTime.h"
#include "ReportDataRecord.h"
#include "GpsFixDataRecord.h"
#include "OverTheAirConfigDataRecord.h"
#include "GpsFixLiFoRingBuffer.h"
#include "Switch.h"
#include "LTC2943.h"  // verbruiksmeter
#include "LSM303.h"   // accelerometer
#include "HTU21DF.h"  // humudity/temp sensor
#include "pb_encode.h"
#include "pb_decode.h"
#include "sensor.pb.h"

//#define DEBUG

#define PROJECT_NAME "SodaqOne Monitoring Node"
#define VERSION "1.0"
#define STARTUP_DELAY 5000

#define GPS_TIME_VALIDITY 0b00000011 // date and time (but not fully resolved)
#define GPS_FIX_FLAGS 0b00000001 // just gnssFixOK

#define MAX_RTC_EPOCH_OFFSET 25
#define NUM_TX_RETRIES 5

#define ADC_AREF 3.3f
#define BATVOLT_R1 2.0f
#define BATVOLT_R2 2.0f

#define TEMPERATURE_OFFSET 20.0
#define MICROSWITCH1_PIN 2
#define MICROSWITCH2_PIN 3

#define DEBUG_STREAM SerialUSB
#define CONSOLE_STREAM SerialUSB
#define LORA_STREAM Serial1

#define NIBBLE_TO_HEX_CHAR(i) ((i <= 9) ? ('0' + i) : ('A' - 10 + i))
#define HIGH_NIBBLE(i) ((i >> 4) & 0x0F)
#define LOW_NIBBLE(i) (i & 0x0F)

// version of "hex to bin" macro that supports both lower and upper case
#define HEX_CHAR_TO_NIBBLE(c) ((c >= 'a') ? (c - 'a' + 0x0A) : ((c >= 'A') ? (c - 'A' + 0x0A) : (c - '0')))
#define HEX_PAIR_TO_BYTE(h, l) ((HEX_CHAR_TO_NIBBLE(h) << 4) + HEX_CHAR_TO_NIBBLE(l))

#define consolePrint(x) CONSOLE_STREAM.print(x)
#define consolePrintln(x) CONSOLE_STREAM.println(x)

#define debugPrint(x)   { if (params.getIsDbgEnabled()) DEBUG_STREAM.print(x); }
#define debugPrintln(x) { if (params.getIsDbgEnabled()) DEBUG_STREAM.println(x); }

enum LedColor {
  NONE = 0,
  RED,
  GREEN,
  BLUE
};

RTCZero rtcMinute;
RTCTimer timer;
UBlox ublox;
Time time;
LSM303 lsm303;
//Switch microSwitch1(MICROSWITCH1_PIN);
//Switch microSwitch2(MICROSWITCH2_PIN);
LTC ltc(1);
HTU21DF htu;

ReportDataRecord pendingReportDataRecord;
bool isPendingReportDataRecordNew; // this is set to true only when pendingReportDataRecord is written by the delegate

volatile bool minuteFlag;
volatile bool switch1Flag;
volatile bool switch2Flag;
volatile bool accel1Flag;
volatile bool accel2Flag;
volatile bool movedFlag;
LedColor ledColor = RED;
// LSM303 code
int ax_o, ay_o, az_o;
int d_x, d_y, d_z;
bool hasMoved;
bool powerlossSend = false;
static uint8_t lastResetCause;
static bool isLoraInitialized;
static bool isRtcInitialized;
static bool isDeviceInitialized;
static int64_t rtcEpochDelta; // set in setNow() and used in getGpsFixAndTransmit() for correcting time in loop
int numTxRetries = 0;

static uint8_t receiveBuffer[16];
static uint8_t receiveBufferSize;
static uint8_t sendBuffer[128];
static uint8_t sendBufferSize;
static uint8_t loraHWEui[8];
static uint8_t loraFirmVer[40];
static bool isLoraHWEuiInitialized;

void setup();
void loop();

uint32_t getNow();
void setNow(uint32_t now);
void handleBootUpCommands();
void initRtc();
void rtcMinuteAlarmHandler();
void switch1Handler();
void switch2Handler();
void accel1Handler();
void accel2Handler();
void attachHandlers();
void initRtcTimer();
void resetRtcTimerEvents();
void initSleep();
bool initLora(bool suppressMessages = false);
void systemSleep();
void runDefaultFixEvent(uint32_t now);
void runAlternativeFixEvent(uint32_t now);
void runLoraModuleSleepExtendEvent(uint32_t now);
void setLedColor(LedColor color);
void setGpsActive(bool on);
void setLoraActive(bool on);
void setLsm303Active(bool on);
void LSM303_Update();
bool convertAndCheckHexArray(uint8_t* result, const char* hex, size_t resultSize);
bool isAlternativeFixEventApplicable();
bool isCurrentTimeOfDayWithin(uint32_t daySecondsFrom, uint32_t daySecondsTo);
void delegateNavPvt(NavigationPositionVelocityTimeSolution* NavPvt);
bool getDataAndTransmit();
bool getGpsFixAndTransmit();
bool getAliveDataAndTransmit();
void getSwitchDataAndTransmit(int SwitchNo);
void getMoveDataAndTransmit();
void getPowerlossDataAndTransmit();
uint8_t getBatteryVoltage();
int8_t getBoardTemperature();
void updateGpsSendBuffer();
void updateAliveSendBuffer();
bool updateSwitchSendBuffer(int SwitchNo);
bool updateMoveSendBuffer();
bool updatePowerlossSendBuffer();
bool transmit();
void updateConfigOverTheAir();
void getHWEUI();
void setDevAddrOrEUItoHWEUI();
void onConfigReset(void);
bool switch1sensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool switch2sensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool tempsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool humsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool voltsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool currentsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool chargesensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool movesensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool powerlosssensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);
bool retriessensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg);

static void printCpuResetCause(Stream& stream);
static void printBootUpMessage(Stream& stream);


void setup()
{
  // Allow power to remain on
  pinMode(ENABLE_PIN_IO, OUTPUT);
  digitalWrite(ENABLE_PIN_IO, HIGH);
  // enable power to the grove shield
  pinMode(11, OUTPUT);
  digitalWrite(11, HIGH);
  // Define the switch Pin as interrupt input line
  pinMode(MICROSWITCH1_PIN, INPUT);
  pinMode(MICROSWITCH2_PIN, INPUT);
  // Define the accelerometer pins as interrupt input line
  pinMode(ACCEL_INT1, INPUT_PULLUP);
  pinMode(ACCEL_INT2, INPUT_PULLUP);

  lastResetCause = PM->RCAUSE.reg;
  sodaq_wdt_enable();
  sodaq_wdt_reset();

  SerialUSB.begin(115200);
  setLedColor(RED);
  sodaq_wdt_safe_delay(STARTUP_DELAY);
  printBootUpMessage(SerialUSB);

  gpsFixLiFoRingBuffer_init();
  // attach the interrupts
  attachHandlers();
  initSleep();
  initRtc();

  Wire.begin();
  ublox.enable(); // turn power on early for faster initial fix
  htu.begin();
  htu.setDiag(DEBUG_STREAM, params.getIsDbgEnabled());
  ltc.setDiag(DEBUG_STREAM, params.getIsDbgEnabled());
  lsm303.setDiag(DEBUG_STREAM, params.getIsDbgEnabled());

  // init params
  params.setConfigResetCallback(onConfigReset);
  params.read();

  // disable the watchdog only for the boot menu
  sodaq_wdt_disable();
  handleBootUpCommands();
  sodaq_wdt_enable();

  isLoraInitialized = initLora();
  initRtcTimer();
  initLsm303();

  isDeviceInitialized = true;

  consolePrintln("** Boot-up completed successfully!");
  sodaq_wdt_reset();

  // disable the USB if the app is not in debug mode
  //#ifndef DEBUG
  if (!params.getIsDbgEnabled()) {
    consolePrintln("The USB is going to be disabled now.");
    SerialUSB.flush();
    sodaq_wdt_safe_delay(3000);
    SerialUSB.end();
    USBDevice.detach();
    USB->DEVICE.CTRLA.reg &= ~USB_CTRLA_ENABLE; // Disable USB
  }
  //#endif

  if (getDataAndTransmit()) {
    setLedColor(GREEN);
    sodaq_wdt_safe_delay(800);
  }
}

void loop()
{
  // Reset watchdog
  sodaq_wdt_reset();
  sodaq_wdt_flag = false;

  if (!params.getIsGpsEnabled())
  {
    if (switch1Flag)
    {
      debugPrintln("switch 1 Flag set");
      if (params.getIsLedEnabled())
      {
        setLedColor(RED);
      }

      getSwitchDataAndTransmit(1);
      switch1Flag = false;
    }
    if (switch2Flag)
    {
      debugPrintln("switch 2 Flag set");
      if (params.getIsLedEnabled())
      {
        setLedColor(GREEN);
      }

      getSwitchDataAndTransmit(2);
      switch2Flag = false;
    }
    if (accel1Flag || accel2Flag)
    {
      debugPrintln("accelerator Flag set");
      if (params.getIsLedEnabled())
      {
        setLedColor(GREEN);
      }
      getMoveDataAndTransmit();
      accel1Flag = false;
      accel2Flag = false;
    }
  }
  if (minuteFlag)
  {
    if (params.getIsLedEnabled())
    {
      setLedColor(BLUE);
    }
    if (!params.getIsGpsEnabled())
    {
      getPowerlossDataAndTransmit();
    }
    timer.update(); // handle scheduled events
    minuteFlag = false;
  }

  systemSleep();
}

/**
   Initializes the CPU sleep mode.
*/
void initSleep()
{
  // Set the XOSC32K to run in standby
  SYSCTRL->XOSC32K.bit.RUNSTDBY = 1;

  // Configure EIC to use GCLK1 which uses XOSC32K
  // This has to be done after the first call to attachInterrupt()
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCM_EIC) |
                      GCLK_CLKCTRL_GEN_GCLK1 |
                      GCLK_CLKCTRL_CLKEN;

  // Set the sleep mode
  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
}

/**
   Returns the battery voltage minus 3 volts.
*/
uint8_t getBatteryVoltage()
{
  uint16_t voltage = (uint16_t)((ADC_AREF / 1.023) * (BATVOLT_R1 + BATVOLT_R2) / BATVOLT_R2 * (float)analogRead(BAT_VOLT));
  voltage = (voltage - 3000) / 10;

  return (voltage > 255 ? 255 : (uint8_t)voltage);
}

/**
   Returns the board temperature.
*/
int8_t getBoardTemperature()
{
  setLsm303Active(true);

  uint8_t tempL = lsm303.readReg(LSM303::TEMP_OUT_L);
  uint8_t tempH = lsm303.readReg(LSM303::TEMP_OUT_H);

  // Note: tempH has the 4 "unused" bits set correctly by the sensor (0x0 or 0xF)
  int16_t rawTemp = ((uint16_t)tempH << 8) | tempL;

  setLsm303Active(false);

  return round(TEMPERATURE_OFFSET + rawTemp / 8.0);
}

void attachHandlers()
{
  attachInterrupt(MICROSWITCH1_PIN, switch1Handler, CHANGE);
  attachInterrupt(MICROSWITCH2_PIN, switch2Handler, CHANGE);
  attachInterrupt(ACCEL_INT1, accel1Handler, FALLING);
  attachInterrupt(ACCEL_INT2, accel2Handler, FALLING);
}

/**
   Updates the "sendBuffer" using the current "pendingReportDataRecord" and its "sendBufferSize".
*/
void updateGpsSendBuffer()
{
  // copy the pendingReportDataRecord into the sendBuffer
  memcpy(sendBuffer, pendingReportDataRecord.getBuffer(), pendingReportDataRecord.getSize());
  sendBufferSize = pendingReportDataRecord.getSize();

  // copy the previous coordinates if applicable (-1 because one coordinate is already in the report record)
  GpsFixDataRecord record;
  for (uint8_t i = 0; i < params.getCoordinateUploadCount() - 1; i++) {
    record.init();

    // (skip first record because it is in the report record already)
    if (!gpsFixLiFoRingBuffer_peek(1 + i, &record)) {
      break;
    }

    if (!record.isValid()) {
      break;
    }

    record.updatePreviousFixValue(pendingReportDataRecord.getTimestamp());
    memcpy(&sendBuffer[sendBufferSize - 1], record.getBuffer(), record.getSize());
    sendBufferSize += record.getSize();
  }
}

/**
   Updates the "sendBuffer" using the current "pendingReportDataRecord" and its "sendBufferSize".
*/
void updateAliveSendBuffer()
{
  debugPrint("LTC values: Valid ");
  debugPrint(ltc.isValid());
  debugPrint(" Charge ");
  debugPrint(ltc.getCharge());
  debugPrint(F(" mAh, Current "));
  debugPrint(ltc.getCurrent());
  debugPrint(F(" A, Voltage "));
  debugPrint(ltc.getVoltage());
  debugPrint(F(" V, Temperature "));
  debugPrint(ltc.getTemp());
  debugPrintln(F(" C"));

  debugPrint("HTU values: Temperature ");
  debugPrint(htu.getTemp());
  debugPrint(F(" C, Humidity "));
  debugPrint(htu.getHum());
  debugPrintln(F(" %"));

  NodeMessage nodemsg2 = NodeMessage_init_zero;

  /* Create a stream that will write to our buffer. */
  pb_ostream_t stream2 = pb_ostream_from_buffer(sendBuffer, sizeof(sendBuffer));

  // add the temperature data to the output buffer
  nodemsg2.reading.funcs.encode = &tempsensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream2, NodeMessage_fields, &nodemsg2))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream2));
  }
  else
  {
    sendBufferSize = stream2.bytes_written;
  }

  // add the humidity data to the output buffer
  nodemsg2.reading.funcs.encode = &humsensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream2, NodeMessage_fields, &nodemsg2))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream2));
  }
  else
  {
    sendBufferSize = stream2.bytes_written;
  }

  // add the voltage data to the output buffer
  nodemsg2.reading.funcs.encode = &voltsensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream2, NodeMessage_fields, &nodemsg2))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream2));
  }
  else
  {
    sendBufferSize = stream2.bytes_written;
  }

  // add the current data to the output buffer
  nodemsg2.reading.funcs.encode = &currentsensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream2, NodeMessage_fields, &nodemsg2))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream2));
  }
  else
  {
    sendBufferSize = stream2.bytes_written;
  }

  // add the charge data to the output buffer
  nodemsg2.reading.funcs.encode = &chargesensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream2, NodeMessage_fields, &nodemsg2))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream2));
  }
  else
  {
    sendBufferSize = stream2.bytes_written;
  }

  // only send the number of retries if it is > 0
  if (numTxRetries > 0)
  {
    // add the retry counter data to the output buffer
    nodemsg2.reading.funcs.encode = &retriessensor_callback;

    /* Now we are ready to encode the message! */
    /* Then just check for any errors.. */
    if (!pb_encode(&stream2, NodeMessage_fields, &nodemsg2))
    {
      debugPrint("Encoding failed: ");
      debugPrintln(PB_GET_ERROR(&stream2));
    }
    else
    {
      sendBufferSize = stream2.bytes_written;
    }
  }

  debugPrint("sendBufferSize:");
  debugPrintln(sendBufferSize);
  debugPrint("message:<");
  for (uint8_t i = 0; i < sendBufferSize; i++)
  {
    debugPrint(sendBuffer[i]);
    if (i < sendBufferSize - 1)
      debugPrint(" ");
  }
  debugPrintln(">");
}

bool updateSwitchSendBuffer(int switchNo)
{
  bool isSuccessful = false;
  debugPrintln("updateSwitchSendBuffer....");
  debugPrint("Get Switch: ");
  debugPrint(switchNo);

  NodeMessage nodemsg = NodeMessage_init_zero;

  /* Create a stream that will write to our buffer. */
  pb_ostream_t stream = pb_ostream_from_buffer(sendBuffer, sizeof(sendBuffer));

  //Read the state of microSwitch1
  if (switchNo == 1) {
    debugPrint(" state: ");
    debugPrintln(digitalRead(MICROSWITCH1_PIN));

    nodemsg.reading.funcs.encode = &switch1sensor_callback;

    /* Now we are ready to encode the message! */
    /* Then just check for any errors.. */
    if (!pb_encode(&stream, NodeMessage_fields, &nodemsg))
    {
      debugPrint("Encoding failed: ");
      debugPrintln(PB_GET_ERROR(&stream));
    }
    else
    {
      sendBufferSize = stream.bytes_written;
      debugPrint("sendBufferSize:");
      debugPrintln(sendBufferSize);
      debugPrint("message:<");
      for (uint8_t i = 0; i < sendBufferSize; i++)
      {
        debugPrint(sendBuffer[i]);
        if (i < sendBufferSize - 1)
          debugPrint(" ");
      }
      debugPrintln(">");
      isSuccessful = true;
    }
  }
  //Read the state of microSwitch2
  if (switchNo == 2) {
    debugPrint(" state: ");
    debugPrintln(digitalRead(MICROSWITCH2_PIN));

    nodemsg.reading.funcs.encode = &switch2sensor_callback;

    /* Now we are ready to encode the message! */
    /* Then just check for any errors.. */
    if (!pb_encode(&stream, NodeMessage_fields, &nodemsg))
    {
      debugPrint("Encoding failed: ");
      debugPrintln(PB_GET_ERROR(&stream));
    }
    else
    {
      sendBufferSize = stream.bytes_written;
      debugPrint("sendBufferSize:");
      debugPrintln(sendBufferSize);
      debugPrint("message:<");
      for (uint8_t i = 0; i < sendBufferSize; i++)
      {
        debugPrint(sendBuffer[i]);
        if (i < sendBufferSize - 1)
          debugPrint(" ");
      }
      debugPrintln(">");
      isSuccessful = true;
    }
  }
  return isSuccessful;
}

bool updateMoveSendBuffer()
{
  bool isSuccessful = false;
  debugPrintln("updateMoveSendBuffer....");

  NodeMessage nodemsg = NodeMessage_init_zero;

  /* Create a stream that will write to our buffer. */
  pb_ostream_t stream = pb_ostream_from_buffer(sendBuffer, sizeof(sendBuffer));

  nodemsg.reading.funcs.encode = &movesensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream, NodeMessage_fields, &nodemsg))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream));
  }
  else
  {
    sendBufferSize = stream.bytes_written;
    debugPrint("sendBufferSize:");
    debugPrintln(sendBufferSize);
    debugPrint("message:<");
    for (uint8_t i = 0; i < sendBufferSize; i++)
    {
      debugPrint(sendBuffer[i]);
      if (i < sendBufferSize - 1)
        debugPrint(" ");
    }
    debugPrintln(">");
    isSuccessful = true;
  }
  return isSuccessful;
}

bool updatePowerlossSendBuffer()
{
  bool isSuccessful = false;
  debugPrintln("updatePowerlossSendBuffer....");

  NodeMessage nodemsg = NodeMessage_init_zero;

  /* Create a stream that will write to our buffer. */
  pb_ostream_t stream = pb_ostream_from_buffer(sendBuffer, sizeof(sendBuffer));

  nodemsg.reading.funcs.encode = &powerlosssensor_callback;

  /* Now we are ready to encode the message! */
  /* Then just check for any errors.. */
  if (!pb_encode(&stream, NodeMessage_fields, &nodemsg))
  {
    debugPrint("Encoding failed: ");
    debugPrintln(PB_GET_ERROR(&stream));
  }
  else
  {
    sendBufferSize = stream.bytes_written;
    debugPrint("sendBufferSize:");
    debugPrintln(sendBufferSize);
    debugPrint("message:<");
    for (uint8_t i = 0; i < sendBufferSize; i++)
    {
      debugPrint(sendBuffer[i]);
      if (i < sendBufferSize - 1)
        debugPrint(" ");
    }
    debugPrintln(">");
    isSuccessful = true;
  }
  return isSuccessful;
}

/**
   Sends the current sendBuffer through lora (if enabled).
   Repeats the transmitions according to params.getRepeatCount().
*/
bool transmit()
{
  bool retVal = true;
  uint8_t sendReturn;
  uint32_t downCnt, upCnt;
  uint16_t recvSize;
  bool retry = true;
  int retCount = 0;

  setLoraActive(true);

  while (isLoraInitialized && retry)
  {
    if (params.getIsAckEnabled())
    {
      sendReturn = LoRaBee.sendReqAck(1, sendBuffer, sendBufferSize, 1);
    }
    else
    {
      sendReturn = LoRaBee.send(1, sendBuffer, sendBufferSize);
    }
    switch (sendReturn)
    {
      case NoError:
        debugPrintln("Data transmitted successfully.");
        receiveBufferSize = LoRaBee.receive(receiveBuffer, sizeof(receiveBuffer));
        if (receiveBufferSize > 0)
        {
          if (receiveBufferSize > 1)
          {
            debugPrint("Received OTA Configuration (#)");
            debugPrintln(receiveBufferSize);
            for (uint16_t i = 0; i < receiveBufferSize; i++)
            {
              debugPrintln(receiveBuffer[i]);
            }
            updateConfigOverTheAir();
          }
        }
        downCnt = LoRaBee.getDownCntr();
        upCnt = LoRaBee.getUpCntr();
        debugPrint("Down counter:");
        debugPrintln(downCnt);
        debugPrint("Up counter:");
        debugPrintln(upCnt);
        params._frameUpCount = upCnt;
        params._frameDownCount = downCnt;
        params.commit(true);
        retry = false;
        break;
      case NoResponse:
        debugPrintln("There was no response from the device.");
        break;
      case Timeout:
        debugPrintln("Connection timed-out. Check your serial connection to the device! Sleeping for 20sec.");
        delay(20000);
        break;
      case PayloadSizeError:
        debugPrintln("The size of the payload is greater than allowed. Transmission failed!");
        break;
      case InternalError:
        debugPrintln("Oh No! This shouldn't happen. Something is really wrong! Try restarting the device!\r\nThe program will now halt.");
        //      while (1) {};
        retVal = false;
        break;
      case Busy:
        debugPrintln("The device is busy. Sleeping for 2 extra seconds.");
        delay(2000);
        break;
      case NetworkFatalError:
        debugPrintln("There is a non-recoverable error with the network connection. You should re-connect.\r\nThe program will now halt.");
        //        while (1) {};
        retVal = false;
        break;
      case NotConnected:
        debugPrintln("The device is not connected to the network. Please connect to the network before attempting to send data.\r\nThe program will now halt.");
        //        while (1) {};
        retVal = false;
        break;
      case NoAcknowledgment:
        debugPrintln("There was no acknowledgment sent back!");
        break;
      default:
        break;
    }
    if (sendReturn != NoError)
    {
      retCount++;
      debugPrint("Retry number: ");
      debugPrintln(retCount);
      if (retCount > params.getRepeatCount())
      {
        retry = false;
        retVal = false;
      }
    }
  }
  // bewaar het aantal zendpogingen
  numTxRetries = retCount;
  setLoraActive(false);
  return retVal;
}

/**
   Uses the "receiveBuffer" (received from LoRaWAN) to update the configuration.
*/
void updateConfigOverTheAir()
{
  OverTheAirConfigDataRecord record;
  record.init();
  record.copyFrom(receiveBuffer, receiveBufferSize);

  if (record.isValid()) {
    params._defaultFixInterval = record.getDefaultFixInterval();
    params._alternativeFixInterval = record.getAlternativeFixInterval();

    // time of day seconds assumed
    params._alternativeFixFromHours = record.getAlternativeFixFrom() / 3600;
    params._alternativeFixFromMinutes = (record.getAlternativeFixFrom() - params._alternativeFixFromHours * 3600) / 60;

    params._alternativeFixToHours = record.getAlternativeFixTo() / 3600;
    params._alternativeFixToMinutes = (record.getAlternativeFixTo() - params._alternativeFixToHours * 3600) / 60;

    params._gpsFixTimeout = record.getGpsFixTimeout();

    params.commit();
    debugPrintln("OTAA Config commited!");

    // apply the rtcMinute timer changes
    resetRtcTimerEvents();
  }
  else {
    debugPrintln("OTAA Config record is not valid!");
  }
}

/**
   Converts the given hex array and returns true if it is valid hex and non-zero.
   "hex" is assumed to be 2*resultSize bytes.
*/
bool convertAndCheckHexArray(uint8_t* result, const char* hex, size_t resultSize)
{
  bool foundNonZero = false;

  uint16_t inputIndex = 0;
  uint16_t outputIndex = 0;

  // stop at the first string termination char, or if output buffer is over
  while (outputIndex < resultSize && hex[inputIndex] != 0 && hex[inputIndex + 1] != 0) {
    if (!isxdigit(hex[inputIndex]) || !isxdigit(hex[inputIndex + 1])) {
      return false;
    }

    result[outputIndex] = HEX_PAIR_TO_BYTE(hex[inputIndex], hex[inputIndex + 1]);

    if (result[outputIndex] > 0) {
      foundNonZero = true;
    }

    inputIndex += 2;
    outputIndex++;
  }

  result[outputIndex] = 0; // terminate the string

  return foundNonZero;
}

/**
   Initializes the lora module.
   Returns true if successful.
*/
bool initLora(bool supressMessages)
{
  if (!supressMessages) {
    consolePrintln("Initializing LoRa...");
  }

  LORA_STREAM.begin(LoRaBee.getDefaultBaudRate());
  //#ifdef DEBUG
  if (params.getIsDbgEnabled())
  {
    LoRaBee.setDiag(DEBUG_STREAM, true);
  }
  //#endif

  bool allParametersValid;
  bool result;
  if (params.getIsOtaaEnabled()) {
    uint8_t devEui[8];
    uint8_t appEui[8];
    uint8_t appKey[16];

    allParametersValid = convertAndCheckHexArray((uint8_t*)devEui, params.getDevAddrOrEUI(), sizeof(devEui))
                         && convertAndCheckHexArray((uint8_t*)appEui, params.getAppSKeyOrEUI(), sizeof(appEui))
                         && convertAndCheckHexArray((uint8_t*)appKey, params.getNwSKeyOrAppKey(), sizeof(appKey));

    // try to initialize the lorabee regardless the validity of the parameters,
    // in order to allow the sleeping mechanism to work
    if (LoRaBee.initOTA(LORA_STREAM, devEui, appEui, appKey, true)) {
      result = true;
    }
    else {
      if (!supressMessages) {
        consolePrintln("LoRa init failed!");
      }

      result = false;
    }
  }
  else {
    uint8_t devAddr[4];
    uint8_t appSKey[16];
    uint8_t nwkSKey[16];

    allParametersValid = convertAndCheckHexArray((uint8_t*)devAddr, params.getDevAddrOrEUI(), sizeof(devAddr))
                         && convertAndCheckHexArray((uint8_t*)appSKey, params.getAppSKeyOrEUI(), sizeof(appSKey))
                         && convertAndCheckHexArray((uint8_t*)nwkSKey, params.getNwSKeyOrAppKey(), sizeof(nwkSKey));

    // try to initialize the lorabee regardless the validity of the parameters,
    // in order to allow the sleeping mechanism to work
    if (LoRaBee.initABP(LORA_STREAM, devAddr, appSKey, nwkSKey, true)) {
      result = true;
    }
    else {
      if (!supressMessages) {
        consolePrintln("LoRa init failed!");
      }

      result = false;
    }
  }

  if (!allParametersValid) {
    if (!supressMessages) {
      consolePrintln("The parameters for LoRa are not valid. LoRa will not be enabled.");
    }

    result = false; // override the result from the initialization above
  }
  LoRaBee.setUpCntr(params.getFrameUpCount());
  LoRaBee.setDownCntr(params.getFrameDownCount());

  setLoraActive(false);
  return result; // false by default
}

/**
   Powers down all devices and puts the system to deep sleep.
*/
void systemSleep()
{
  setLedColor(NONE);
  setGpsActive(false); // explicitly disable after resetting the pins

  sodaq_wdt_disable();

  // do not go to sleep if DEBUG is enabled, to keep USB connected
  //#ifndef DEBUG
  if (!params.getIsDbgEnabled())
  {
    noInterrupts();
    if (!(sodaq_wdt_flag || minuteFlag || switch1Flag || switch2Flag)) {
      interrupts();

      __WFI(); // SAMD sleep
    }
    interrupts();
  }
  //#endif

  // Re-enable watchdog
  sodaq_wdt_enable();
}

/**
   Returns the current datetime (seconds since unix epoch).
*/
uint32_t getNow()
{
  return rtcMinute.getEpoch();
}

/**
   Sets the RTC epoch and "rtcEpochDelta".
*/
void setNow(uint32_t newEpoch)
{
  uint32_t currentEpoch = getNow();

  debugPrint("Setting RTC from ");
  debugPrint(currentEpoch);
  debugPrint(" to ");
  debugPrintln(newEpoch);

  rtcEpochDelta = newEpoch - currentEpoch;
  rtcMinute.setEpoch(newEpoch);

  timer.adjust(currentEpoch, newEpoch);

  isRtcInitialized = true;
}

/**
   Shows and handles the boot up commands.
*/
void handleBootUpCommands()
{
  setResetDevAddrOrEUItoHWEUICallback(setDevAddrOrEUItoHWEUI);

  do {
    showBootMenu(CONSOLE_STREAM);
  } while (!params.checkConfig(CONSOLE_STREAM));

  params.showConfig(&CONSOLE_STREAM);
  params.commit();
}

/**
   Initializes the RTC.
*/
void initRtc()
{
  // activate the minute event
  rtcMinute.begin();

  // Schedule the wakeup interrupt for every minute
  // Alarm is triggered 1 cycle after match
  rtcMinute.setAlarmSeconds(59);
  rtcMinute.enableAlarm(RTCZero::MATCH_SS); // alarm every minute

  // Attach handler
  rtcMinute.attachInterrupt(rtcMinuteAlarmHandler);

  // This sets it to 2000-01-01
  rtcMinute.setEpoch(0);
}

/**
   Runs every minute by the rtcMinute alarm.
*/
void rtcMinuteAlarmHandler()
{
  minuteFlag = true;
}

/**
   Runs if the switch is pressed.
*/
void switch1Handler()
{
  switch1Flag = true;
}

/**
   Runs if the switch is pressed.
*/
void switch2Handler()
{
  switch2Flag = true;
}

/**
   Runs if the accelerometer is activated
*/
void accel1Handler()
{
  accel1Flag = true;
}

/**
   Runs if the accelerometer is activated
*/
void accel2Handler()
{
  accel2Flag = true;
}

/**
   Initializes the RTC Timer and schedules the default events.
*/
void initRtcTimer()
{
  timer.setNowCallback(getNow); // set how to get the current time
  timer.allowMultipleEvents();

  resetRtcTimerEvents();
}

/**
   Initializes the RTC Timer and schedules the default events.
*/
void initLsm303()
{
  if (!lsm303.init(LSM303::device_D, LSM303::sa0_low)) {
    debugPrintln("Initialization of the LSM303 failed!");
    return;
  }
  lsm303.enableAccelInterrupt();
}
/**
   Clears the RTC Timer events and schedules the default events.
*/
void resetRtcTimerEvents()
{
  timer.clearAllEvents();

  // Schedule the default fix event
  timer.every(params.getDefaultFixInterval() * 60, runDefaultFixEvent);

  // check if the alternative fix event should be scheduled at all
  if (params.getAlternativeFixInterval() > 0) {
    // Schedule the alternative fix event
    timer.every(params.getAlternativeFixInterval() * 60, runAlternativeFixEvent);
  }

  // if lora is not enabled, schedule an event that takes care of extending the sleep time of the module
  if (!isLoraInitialized) {
    timer.every(24 * 60 * 60, runLoraModuleSleepExtendEvent); // once a day
  }
}

/**
   Returns true if the alternative fix event should run at the current time.
*/
bool isAlternativeFixEventApplicable()
{
  // - RTC should be initialized (synced time)
  // - alternative fix interval should be set
  // - the span between FROM and TO should be at least as much as the alternative fix interval
  // - current time should be within the FROM and TO times set
  return (isRtcInitialized
          && (params.getAlternativeFixInterval() > 0)
          && (params.getAlternativeFixTo() - params.getAlternativeFixFrom() >= params.getAlternativeFixInterval() * 60)
          && (isCurrentTimeOfDayWithin(params.getAlternativeFixFrom(), params.getAlternativeFixTo())));
}

/**
   Returns true if the current rtc time is within the given times of day (in seconds).
*/
bool isCurrentTimeOfDayWithin(uint32_t daySecondsFrom, uint32_t daySecondsTo)
{
  uint32_t daySecondsCurrent = rtcMinute.getHours() * 60 * 60 + rtcMinute.getMinutes() * 60;

  return (daySecondsCurrent >= daySecondsFrom && daySecondsCurrent < daySecondsTo);
}

/**
   Runs the default fix event sequence (only if applicable).
*/
void runDefaultFixEvent(uint32_t now)
{
  if (!isAlternativeFixEventApplicable()) {
    debugPrintln("Default data event started.");
    getDataAndTransmit();
  }
}

/**
   Runs the alternative fix event sequence (only if it set and is within the set time).
*/
void runAlternativeFixEvent(uint32_t now)
{
  if (isAlternativeFixEventApplicable()) {
    debugPrintln("Alternative fix event started.");
    getDataAndTransmit();
  }
}

/**
   Wakes up the lora module to put it back to sleep, i.e. extends the sleep period
*/
void runLoraModuleSleepExtendEvent(uint32_t now)
{
  debugPrintln("Extending LoRa module sleep period.");

  setLoraActive(true);
  sodaq_wdt_safe_delay(80);
  setLoraActive(false);
}

/**
   Turns the led on according to the given color. Makes no assumptions about the status of the pins
   i.e. it sets them every time,
*/
void setLedColor(LedColor color)
{
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);

  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);

  switch (color)
  {
    case NONE:
      break;
    case RED:
      digitalWrite(LED_RED, LOW);
      break;
    case GREEN:
      digitalWrite(LED_GREEN, LOW);
      break;
    case BLUE:
      digitalWrite(LED_BLUE, LOW);
      break;
    default:
      break;
  }
}

/**
    Checks validity of data, adds valid points to the points list, syncs the RTC
*/
void delegateNavPvt(NavigationPositionVelocityTimeSolution* NavPvt)
{
  sodaq_wdt_reset();

  // note: db_printf gets enabled/disabled according to the "DEBUG" define (ublox.cpp)
  ublox.db_printf("%4.4d-%2.2d-%2.2d %2.2d:%2.2d:%2.2d.%d valid=%2.2x lat=%d lon=%d sats=%d fixType=%2.2x\r\n",
                  NavPvt->year, NavPvt->month, NavPvt->day,
                  NavPvt->hour, NavPvt->minute, NavPvt->seconds, NavPvt->nano, NavPvt->valid,
                  NavPvt->lat, NavPvt->lon, NavPvt->numSV, NavPvt->fixType);

  // sync the RTC time
  if ((NavPvt->valid & GPS_TIME_VALIDITY) == GPS_TIME_VALIDITY) {
    uint32_t epoch = time.mktime(NavPvt->year, NavPvt->month, NavPvt->day, NavPvt->hour, NavPvt->minute, NavPvt->seconds);

    // check if there is an actual offset before setting the RTC
    if (abs((int64_t)getNow() - (int64_t)epoch) > MAX_RTC_EPOCH_OFFSET) {
      setNow(epoch);
    }
  }

  // check that the fix is OK and that it is a 3d fix or GNSS + dead reckoning combined
  if (((NavPvt->flags & GPS_FIX_FLAGS) == GPS_FIX_FLAGS) && ((NavPvt->fixType == 3) || (NavPvt->fixType == 4))) {
    pendingReportDataRecord.setAltitude(NavPvt->hMSL < 0 ? 0xFFFF : (uint16_t)(NavPvt->hMSL / 1000)); // mm to m
    pendingReportDataRecord.setCourse(NavPvt->heading);
    pendingReportDataRecord.setLat(NavPvt->lat);
    pendingReportDataRecord.setLong(NavPvt->lon);
    pendingReportDataRecord.setSatelliteCount(NavPvt->numSV);
    pendingReportDataRecord.setSpeed((NavPvt->gSpeed * 36) / 10000); // mm/s converted to km/h

    isPendingReportDataRecordNew = true;
  }
}

bool getDataAndTransmit()
{
  if (params.getIsGpsEnabled())
  {
    getGpsFixAndTransmit();
  }
  else
  {
    getAliveDataAndTransmit();
  }
}

/**
   Tries to get a GPS fix and sends the data through LoRa if applicable.
   Times-out after params.getGpsFixTimeout seconds.
   Please see the documentation for more details on how this process works.
*/
bool getGpsFixAndTransmit()
{
  debugPrintln("Starting getGpsFixAndTransmit()...");

  bool isSuccessful = false;
  setGpsActive(true);

  uint32_t startTime = getNow();
  while (getNow() - startTime <= params.getGpsFixTimeout())
  {
    sodaq_wdt_reset();
    uint16_t bytes = ublox.available();

    if (bytes) {
      rtcEpochDelta = 0;
      isPendingReportDataRecordNew = false;
      ublox.GetPeriodic(bytes); // calls the delegate method for passing results

      startTime += rtcEpochDelta; // just in case the clock was changed (by the delegate in ublox.GetPeriodic)

      if (isPendingReportDataRecordNew) {
        isSuccessful = true;
        break;
      }
    }
  }

  setGpsActive(false); // turn off gps as soon as it is not needed

  // populate all fields of the report record
  pendingReportDataRecord.setTimestamp(getNow());
  pendingReportDataRecord.setBatteryVoltage(getBatteryVoltage());
  pendingReportDataRecord.setBoardTemperature(getBoardTemperature());

  GpsFixDataRecord record;
  record.init();
  if (isSuccessful) {
    pendingReportDataRecord.setTimeToFix(pendingReportDataRecord.getTimestamp() - startTime);

    // add the new gpsFixDataRecord to the ringBuffer
    record.setLat(pendingReportDataRecord.getLat());
    record.setLong(pendingReportDataRecord.getLong());
    record.setTimestamp(pendingReportDataRecord.getTimestamp());

    gpsFixLiFoRingBuffer_push(&record);
  }
  else {
    pendingReportDataRecord.setTimeToFix(0xFF);

    // no need to check the buffer or the record for validity, default for Lat/Long is 0 anyway
    gpsFixLiFoRingBuffer_peek(0, &record);
    pendingReportDataRecord.setLat(record.getLat());
    pendingReportDataRecord.setLong(record.getLong());
  }

  //#ifdef DEBUG
  if (params.getIsDbgEnabled())
  {
    pendingReportDataRecord.printHeaderLn(&DEBUG_STREAM);
    pendingReportDataRecord.printRecordLn(&DEBUG_STREAM);
    debugPrintln();
  }
  //#endif
  updateGpsSendBuffer();
  transmit();

  return isSuccessful;
}

/**
   Tries to get the LTC and HTU data and sends the data through LoRa if applicable.
   Please see the documentation for more details on how this process works.
*/
bool getAliveDataAndTransmit()
{
  debugPrintln("Starting getDataAndTransmit()...");

  bool isSuccessful = false;

  // update the sensors, get the data
  ltc.Update();
  //  LSM303_Update();
  htu.Update();

  // check the sensor data and prepare the message
  updateAliveSendBuffer();

  // transmit the message
  transmit();

  return isSuccessful;
}

/**
   Tries to get the Switch data and sends the data through LoRa if applicable.
   Please see the documentation for more details on how this process works.
*/
void getSwitchDataAndTransmit(int SwitchNo)
{
  debugPrintln("Start getSwitchData...");
  if (updateSwitchSendBuffer(SwitchNo))
  {
    if (transmit())
    {
      // apply the rtcMinute timer changes
      resetRtcTimerEvents();
    }
  }
}

/**
   Tries to get the Move data and sends the data through LoRa if applicable.
   Please see the documentation for more details on how this process works.
*/
void getMoveDataAndTransmit()
{
  debugPrintln("Start getMoveData...");
  if (updateMoveSendBuffer())
  {
    if (transmit())
    {
      // apply the rtcMinute timer changes
      resetRtcTimerEvents();
    }
  }
}

/**
   Tries to get the Powerloss data and sends the data through LoRa if applicable.
   Please see the documentation for more details on how this process works.
*/
void getPowerlossDataAndTransmit()
{
  debugPrintln("Start getPowerlossDataAndTransmit...");
  ltc.Update();
  if (!ltc.isValid() && !powerlossSend)
  {
    debugPrintln("Powerloss...");
    powerlossSend = true;
    if (updatePowerlossSendBuffer())
    {
      transmit();
    }
  }
  if (ltc.isValid() && powerlossSend)
  {
    debugPrintln("No powerloss...");
    powerlossSend = false;
    if (updatePowerlossSendBuffer())
    {
      transmit();
    }
  }
}

/**
   Turns the GPS on or off.
*/
void setGpsActive(bool on)
{
  sodaq_wdt_reset();

  if (on) {
    pinMode(GPS_ENABLE, OUTPUT);
    pinMode(GPS_TIMEPULSE, INPUT);

    ublox.enable();
    ublox.flush();

    PortConfigurationDDC pcd;
    if (ublox.getPortConfigurationDDC(&pcd)) {
      pcd.outProtoMask = 1; // Disable NMEA
      ublox.setPortConfigurationDDC(&pcd);

      ublox.CfgMsg(UBX_NAV_PVT, 1); // Navigation Position Velocity TimeSolution
      ublox.funcNavPvt = delegateNavPvt;
    }
    else {
      debugPrintln("uBlox.getPortConfigurationDDC(&pcd) failed!");
    }
  }
  else {
    ublox.disable();
  }
}

/**
   Turns the LoRa module on or off (wake up or sleep)
*/
void setLoraActive(bool on)
{
  sodaq_wdt_reset();

  if (on) {
    LoRaBee.wakeUp();
  }
  else {
    LoRaBee.sleep();
  }
}

/**
  Initializes the LSM303 or puts it in power-down mode.
*/
void setLsm303Active(bool on)
{
  if (on) {
    if (!lsm303.init(LSM303::device_D, LSM303::sa0_low)) {
      debugPrintln("Initialization of the LSM303 failed!");
      return;
    }

    lsm303.enableDefault();
    lsm303.writeReg(LSM303::CTRL5, lsm303.readReg(LSM303::CTRL5) | 0b10001000); // enable temp and 12.5Hz ODR

    sodaq_wdt_safe_delay(100);
  }
  else {
    // disable accelerometer, power-down mode
    lsm303.writeReg(LSM303::CTRL1, 0);

    // zero CTRL5 (including turn off TEMP sensor)
    lsm303.writeReg(LSM303::CTRL5, 0);

    // disable magnetometer, power-down mode
    lsm303.writeReg(LSM303::CTRL7, 0b00000010);
  }
}

void LSM303_Update()
{
  int offset = 700;

  setLsm303Active(true);
  lsm303.read();

  d_x = abs(lsm303.m.x - ax_o);
  d_y = abs(lsm303.m.y - ay_o);
  d_z = abs(lsm303.m.z - az_o);

  if ( d_x > offset || d_y > offset || d_z > offset)
  {
    hasMoved = true;
  }
  ax_o = lsm303.m.x;
  ay_o = lsm303.m.y;
  az_o = lsm303.m.z;

  setLsm303Active(false);
}

bool switch1sensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 1;
  sensormsg.has_id = true;
  sensormsg.value1 = digitalRead(MICROSWITCH1_PIN);
  sensormsg.has_value1 = true;

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool switch2sensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 9;
  sensormsg.has_id = true;
  sensormsg.value1 = digitalRead(MICROSWITCH2_PIN);
  sensormsg.has_value1 = true;

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool tempsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 2;
  sensormsg.has_id = true;
  if (htu.isValid())
  {
    sensormsg.value1 = (int32_t)(htu.getTemp() * 10);
    sensormsg.has_value1 = true;
  }
  else
  {
    sensormsg.error = 1;
    sensormsg.has_error = true;
  }

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool humsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 3;
  sensormsg.has_id = true;
  if (htu.isValid())
  {
    sensormsg.value1 = htu.getHum();
    sensormsg.has_value1 = true;
  }
  else
  {
    sensormsg.error = 1;
    sensormsg.has_error = true;
  }

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool voltsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 4;
  sensormsg.has_id = true;
  if (ltc.isValid())
  {
    sensormsg.value1 = (int32_t)(ltc.getVoltage() * 1000);
    sensormsg.has_value1 = true;
  }
  else
  {
    sensormsg.error = 1;
    sensormsg.has_error = true;
  }

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool currentsensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 5;
  sensormsg.has_id = true;
  if (ltc.isValid())
  {
    sensormsg.value1 = (int32_t)(ltc.getCurrent() * 1000);
    sensormsg.has_value1 = true;
  }
  else
  {
    sensormsg.error = 1;
    sensormsg.has_error = true;
  }

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool chargesensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 6;
  sensormsg.has_id = true;
  if (ltc.isValid())
  {
    sensormsg.value1 = (int32_t)(ltc.getCharge() * 1000);
    sensormsg.has_value1 = true;
  }
  else
  {
    sensormsg.error = 1;
    sensormsg.has_error = true;
  }

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool movesensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 7;
  sensormsg.has_id = true;
  sensormsg.value1 = 1;
  sensormsg.has_value1 = true;

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool powerlosssensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 10;
  sensormsg.has_id = true;
  sensormsg.value1 = !(ltc.isValid());
  sensormsg.has_value1 = true;

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

bool retriessensor_callback(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
  SensorReading sensormsg = SensorReading_init_zero;

  /* Fill in the lucky number */
  sensormsg.id = 8;
  sensormsg.has_id = true;
  sensormsg.value1 = (int32_t)(numTxRetries);
  sensormsg.has_value1 = true;

  /* This encodes the header for the field, based on the constant info
     from pb_field_t. */
  if (!pb_encode_tag_for_field(stream, field))
    return false;

  /* This encodes the data for the field, based on our FileInfo structure. */
  if (!pb_encode_submessage(stream, SensorReading_fields, &sensormsg))
    return false;

  return true;
}

/**
   Prints the cause of the last reset to the given stream.

   It uses the PM->RCAUSE register to detect the cause of the last reset.
*/
static void printCpuResetCause(Stream& stream)
{
  stream.print("CPU reset by");

  if (PM->RCAUSE.bit.SYST) {
    stream.print(" Software");
  }

  // Syntax error due to #define WDT in CMSIS/4.0.0-atmel/Device/ATMEL/samd21/include/samd21j18a.h
  // if (PM->RCAUSE.bit.WDT) {
  if ((PM->RCAUSE.reg & PM_RCAUSE_WDT) != 0) {
    stream.print(" Watchdog");
  }

  if (PM->RCAUSE.bit.EXT) {
    stream.print(" External");
  }

  if (PM->RCAUSE.bit.BOD33) {
    stream.print(" BOD33");
  }

  if (PM->RCAUSE.bit.BOD12) {
    stream.print(" BOD12");
  }

  if (PM->RCAUSE.bit.POR) {
    stream.print(" Power On Reset");
  }

  stream.print(" [");
  stream.print(PM->RCAUSE.reg);
  stream.println("]");
}

/**
   Prints a boot-up message that includes project name, version,
   and Cpu reset cause.
*/
static void printBootUpMessage(Stream& stream)
{
  stream.println("** " PROJECT_NAME " - " VERSION " **");

  getHWEUIAndFirmVer();
  stream.print("LoRa HWEUI: ");
  for (uint8_t i = 0; i < sizeof(loraHWEui); i++) {
    stream.print((char)NIBBLE_TO_HEX_CHAR(HIGH_NIBBLE(loraHWEui[i])));
    stream.print((char)NIBBLE_TO_HEX_CHAR(LOW_NIBBLE(loraHWEui[i])));
  }
  stream.println();
  stream.print("LoRa Firmware version: ");
  for (uint8_t i = 0; i < sizeof(loraFirmVer); i++) {
    stream.print((char)(loraFirmVer[i]));
  }
  stream.println();

  stream.print(" -> ");
  printCpuResetCause(stream);

  stream.println();
}

/**
   Callback from Config.reset(), used to override default values.
*/
void onConfigReset(void)
{
  setDevAddrOrEUItoHWEUI();
}

void getHWEUIAndFirmVer()
{
  // only read the HWEUI once
  if (!isLoraHWEuiInitialized) {
    initLora(true);
    sodaq_wdt_safe_delay(10);
    setLoraActive(true);
    uint8_t len = LoRaBee.getHWEUI(loraHWEui, sizeof(loraHWEui));
    if (len == sizeof(loraHWEui)) {
      isLoraHWEuiInitialized = true;
    }
    LoRaBee.getFirmVer(loraFirmVer, sizeof(loraFirmVer));
    setLoraActive(false);
  }
}

void setDevAddrOrEUItoHWEUI()
{
  getHWEUIAndFirmVer();

  if (isLoraHWEuiInitialized) {
    for (uint8_t i = 0; i < sizeof(loraHWEui); i++) {
      params._devAddrOrEUI[i * 2] = NIBBLE_TO_HEX_CHAR(HIGH_NIBBLE(loraHWEui[i]));
      params._devAddrOrEUI[i * 2 + 1] = NIBBLE_TO_HEX_CHAR(LOW_NIBBLE(loraHWEui[i]));
    }
  }
}


