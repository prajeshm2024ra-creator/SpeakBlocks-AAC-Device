#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <FS.h>
#include <Adafruit_PN532.h>
#include <math.h>

// ============================================================
// INMP441 MICROPHONE
// ============================================================
#define I2S_MIC_BCK_PIN 14
#define I2S_MIC_WS_PIN  15
#define I2S_MIC_SD_PIN  33
#define I2S_MIC_PORT    I2S_NUM_0

// ============================================================
// MAX98357A AMPLIFIER
// ============================================================
#define I2S_AMP_BCLK 26
#define I2S_AMP_LRC  25
#define I2S_AMP_DOUT 27
#define I2S_AMP_PORT I2S_NUM_1

// ============================================================
// SD CARD
// ============================================================
#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#define SD_SCK_PIN  18

// ============================================================
// I2C BUS
// ============================================================
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define PCF_ADDR       0x27   // 4x3 keypad
#define MODE_PCF_ADDR  0x26   // mode/control buttons
#define PN532_ADDRESS  0x24

// ============================================================
// PN532 RFID READER
// ============================================================
#define PN532_IRQ   4
#define PN532_RESET 13

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET, &Wire);

uint8_t uid[7];
uint8_t uidLength;

String lastUID = "";
unsigned long lastRFIDTime = 0;

#define RFID_POLL_TIMEOUT_MS 50
unsigned long lastRFIDPollMs = 0;
#define RFID_POLL_INTERVAL_MS 150

// ============================================================
// 4x3 KEYPAD (PCF8574 @ 0x27)
// ============================================================
const byte C1 = 7;
const byte C2 = 6;
const byte C3 = 5;

const byte R1 = 4;
const byte R2 = 2;
const byte R3 = 1;
const byte R4 = 0;

const byte rowPins[4] = { R1, R2, R3, R4 };
const byte colPins[3] = { C1, C2, C3 };

char keys[4][3] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// ============================================================
// MODE BUTTONS (PCF8574 @ 0x26)
// ============================================================
#define MODE_VOL_UP_PIN       7
#define MODE_VOL_DOWN_PIN     6
#define MODE_YES_PIN          5
#define MODE_NO_PIN           4
#define MODE_EMERGENCY_PIN    2
#define MODE_DELETE_PIN       1

// ============================================================
// AUDIO SETTINGS
// ============================================================
#define SAMPLE_RATE 16000
#define RECORD_TIME_MS 5000
#define WAV_HEADER_SIZE 44
#define MAX_BUTTONS 12
#define DEBOUNCE_MS 100

// ---- Recording gain (FIXED) ----
// Lower this if recordings sound distorted, raise if too quiet.
#define REC_GAIN 4

// ---- Playback volume (changed with P7 / P6) ----
// Level 1..30. Level 20 = original recorded loudness.
// Each step = 1.5 dB.
#define MIN_VOLUME_LEVEL   1
#define MAX_VOLUME_LEVEL   30
#define REF_VOLUME_LEVEL   20
#define VOLUME_DB_PER_STEP 1.5f

int volumeLevel = REF_VOLUME_LEVEL;

// ============================================================
// GLOBAL VARIABLES
// ============================================================
File file;
uint32_t totalDataBytes = 0;

// ============================================================
// APPLICATION STATES
// ============================================================
enum AppState {
  APP_IDLE,
  APP_RECORDING,
  APP_PLAYING
};

AppState appState = APP_IDLE;

// ============================================================
// I2S STATUS
// ============================================================
bool micRunning = false;
bool ampRunning = false;

// ============================================================
// RFID KEYPAD VARIABLES
// ============================================================
char activeKey = 0;
int activeButton = 0;

bool recorded[MAX_BUTTONS + 1] = {false};
bool finishedPrinted = false;

unsigned long lastKeyEventMs = 0;

String currentPageUID = "DEFAULT";

// ============================================================
// SPECIAL BUTTON RECORDING STATE
// 0 = not recorded, 1 = recording, 2 = saved
// ============================================================
#define SPECIAL_NOT_RECORDED 0
#define SPECIAL_RECORDING    1
#define SPECIAL_SAVED        2

byte yesState = SPECIAL_NOT_RECORDED;
byte noState = SPECIAL_NOT_RECORDED;
byte emergencyState = SPECIAL_NOT_RECORDED;

int activeSpecialButton = 0;   // 1 = YES, 2 = NO, 3 = EMERGENCY

// ============================================================
// SPECIAL BUTTON FILE NAMES
// ============================================================
String specialFileName(int specialButton)
{
  if (specialButton == 1) return "/yes.wav";
  if (specialButton == 2) return "/no.wav";
  if (specialButton == 3) return "/emergency.wav";
  return "";
}

// ============================================================
// PCF8574 WRITE / READ (KEYPAD @ 0x27)
// ============================================================
void writePCF8574(byte value)
{
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(value);
  Wire.endTransmission();
}

byte readPCF8574()
{
  Wire.requestFrom(PCF_ADDR, (byte)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

// ============================================================
// PCF8574 WRITE / READ (MODE BUTTONS @ 0x26)
// ============================================================
void writeModePCF8574(byte value)
{
  Wire.beginTransmission(MODE_PCF_ADDR);
  Wire.write(value);
  Wire.endTransmission();
}

byte readModePCF8574()
{
  Wire.requestFrom(MODE_PCF_ADDR, (byte)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

// ============================================================
// KEYPAD SCAN
// ============================================================
char scanKeypad()
{
  for (byte c = 0; c < 3; c++)
  {
    byte value = 0xFF;
    value &= ~(1 << colPins[c]);
    writePCF8574(value);
    delayMicroseconds(200);
    byte data = readPCF8574();

    for (byte r = 0; r < 4; r++)
    {
      if ((data & (1 << rowPins[r])) == 0)
      {
        writePCF8574(0xFF);
        return keys[r][c];
      }
    }
  }

  writePCF8574(0xFF);
  return 0;
}

// ============================================================
// MODE BUTTON SCAN
// 1 = Vol UP, 2 = Vol DOWN, 3 = YES, 4 = NO,
// 5 = EMERGENCY, 6 = DELETE, 0 = none
// ============================================================
int scanModeButtons()
{
  writeModePCF8574(0xFF);
  delayMicroseconds(200);
  byte data = readModePCF8574();

  if ((data & (1 << MODE_VOL_UP_PIN)) == 0)    return 1;
  if ((data & (1 << MODE_VOL_DOWN_PIN)) == 0)  return 2;
  if ((data & (1 << MODE_YES_PIN)) == 0)       return 3;
  if ((data & (1 << MODE_NO_PIN)) == 0)        return 4;
  if ((data & (1 << MODE_EMERGENCY_PIN)) == 0) return 5;
  if ((data & (1 << MODE_DELETE_PIN)) == 0)    return 6;

  return 0;
}

void waitModeButtonRelease()
{
  while (scanModeButtons() != 0)
  {
    delay(10);
  }
  writeModePCF8574(0xFF);
}

// ============================================================
// FILE NAME FOR RFID PAGE BUTTON
// ============================================================
String fileNameForButton(int btn)
{
  return "/" + currentPageUID + "/btn" + String(btn) + ".wav";
}

int buttonIndexFromKey(char k)
{
  if (k == '1') return 1;
  if (k == '2') return 2;
  if (k == '3') return 3;
  if (k == '4') return 4;
  if (k == '5') return 5;
  if (k == '6') return 6;
  if (k == '7') return 7;
  if (k == '8') return 8;
  if (k == '9') return 9;
  if (k == '*') return 10;
  if (k == '0') return 11;
  if (k == '#') return 12;
  return 0;
}

// ============================================================
// WAV HEADER
// ============================================================
void writeWavHeader(
  File &file,
  int sampleRate,
  int bitsPerSample,
  int channels,
  uint32_t dataSize)
{
  byte header[WAV_HEADER_SIZE];

  uint32_t fileSize = dataSize + WAV_HEADER_SIZE - 8;
  uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
  uint16_t blockAlign = channels * (bitsPerSample / 8);

  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';

  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);

  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';

  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1;  header[21] = 0;

  header[22] = channels;
  header[23] = 0;

  header[24] = (byte)(sampleRate & 0xFF);
  header[25] = (byte)((sampleRate >> 8) & 0xFF);
  header[26] = (byte)((sampleRate >> 16) & 0xFF);
  header[27] = (byte)((sampleRate >> 24) & 0xFF);

  header[28] = (byte)(byteRate & 0xFF);
  header[29] = (byte)((byteRate >> 8) & 0xFF);
  header[30] = (byte)((byteRate >> 16) & 0xFF);
  header[31] = (byte)((byteRate >> 24) & 0xFF);

  header[32] = (byte)(blockAlign & 0xFF);
  header[33] = (byte)((blockAlign >> 8) & 0xFF);

  header[34] = (byte)bitsPerSample;
  header[35] = 0;

  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';

  header[40] = (byte)(dataSize & 0xFF);
  header[41] = (byte)((dataSize >> 8) & 0xFF);
  header[42] = (byte)((dataSize >> 16) & 0xFF);
  header[43] = (byte)((dataSize >> 24) & 0xFF);

  file.seek(0);
  file.write(header, WAV_HEADER_SIZE);
  file.flush();
}

// ============================================================
// MICROPHONE I2S CONFIGURATION
// ============================================================
const i2s_config_t mic_i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 256,
  .use_apll = true,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

const i2s_pin_config_t mic_pin_config = {
  .bck_io_num = I2S_MIC_BCK_PIN,
  .ws_io_num = I2S_MIC_WS_PIN,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_MIC_SD_PIN
};

// ============================================================
// AMPLIFIER I2S CONFIGURATION
// ============================================================
const i2s_config_t amp_i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = 256,
  .use_apll = true,
  .tx_desc_auto_clear = true,
  .fixed_mclk = 0
};

const i2s_pin_config_t amp_pin_config = {
  .bck_io_num = I2S_AMP_BCLK,
  .ws_io_num = I2S_AMP_LRC,
  .data_out_num = I2S_AMP_DOUT,
  .data_in_num = I2S_PIN_NO_CHANGE
};

// ============================================================
// START / STOP MIC
// ============================================================
void startMic()
{
  if (ampRunning)
  {
    i2s_driver_uninstall(I2S_AMP_PORT);
    ampRunning = false;
    delay(100);
  }

  i2s_driver_install(I2S_MIC_PORT, &mic_i2s_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &mic_pin_config);
  i2s_zero_dma_buffer(I2S_MIC_PORT);

  micRunning = true;
  Serial.println("Microphone I2S started");
}

void stopMic()
{
  if (micRunning)
  {
    i2s_driver_uninstall(I2S_MIC_PORT);
    micRunning = false;
    delay(300);
    Serial.println("Microphone I2S stopped");
  }
}

// ============================================================
// START / STOP AMPLIFIER
// ============================================================
void startAmp()
{
  if (micRunning)
  {
    i2s_driver_uninstall(I2S_MIC_PORT);
    micRunning = false;
    delay(300);
  }

  i2s_driver_install(I2S_AMP_PORT, &amp_i2s_config, 0, NULL);
  i2s_set_pin(I2S_AMP_PORT, &amp_pin_config);
  i2s_zero_dma_buffer(I2S_AMP_PORT);

  ampRunning = true;
  Serial.println("Amplifier I2S started");
}

void stopAmp()
{
  if (ampRunning)
  {
    i2s_driver_uninstall(I2S_AMP_PORT);
    ampRunning = false;
    delay(100);
    Serial.println("Amplifier I2S stopped");
  }
}

// ============================================================
// PLAYBACK VOLUME HELPERS
// ============================================================

// Convert volume level (1..30) to a linear multiplier
float volumeToGain()
{
  return powf(
    10.0f,
    ((volumeLevel - REF_VOLUME_LEVEL) * VOLUME_DB_PER_STEP) / 20.0f
  );
}

// Play a short beep at the current volume so the change is audible
void playVolumeBeep()
{
  startAmp();

  float gain = volumeToGain();

  const int beepSamples = SAMPLE_RATE / 5;   // 200 ms
  int16_t buf[256];
  size_t bytesWritten = 0;
  int written = 0;

  while (written < beepSamples)
  {
    int n = min(256, beepSamples - written);

    for (int i = 0; i < n; i++)
    {
      float t = (float)(written + i) / SAMPLE_RATE;
      float v = sinf(2.0f * PI * 1000.0f * t) * 3000.0f * gain;

      if (v > 32767.0f)  v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;

      buf[i] = (int16_t)v;
    }

    i2s_write(I2S_AMP_PORT, (const char *)buf,
              n * sizeof(int16_t), &bytesWritten, portMAX_DELAY);

    written += n;
  }

  delay(50);
  i2s_zero_dma_buffer(I2S_AMP_PORT);
  stopAmp();
}

// Stream a WAV file to the amp with volume applied
void streamWavToAmp(File &f)
{
  f.seek(44);   // skip WAV header

  float gain = volumeToGain();

  Serial.print("Playback level ");
  Serial.print(volumeLevel);
  Serial.print(" -> gain x");
  Serial.println(gain, 2);

  int16_t buffer[256];
  size_t bytesWritten = 0;
  int peak = 0;

  while (f.available())
  {
    int bytesRead = f.read((uint8_t *)buffer, sizeof(buffer));

    if (bytesRead <= 0) break;

    int samples = bytesRead / 2;

    for (int i = 0; i < samples; i++)
    {
      int32_t s = (int32_t)((float)buffer[i] * gain);

      if (s > 32767)  s = 32767;
      if (s < -32768) s = -32768;

      buffer[i] = (int16_t)s;

      if (abs(s) > peak) peak = abs(s);
    }

    i2s_write(I2S_AMP_PORT, (const char *)buffer,
              samples * 2, &bytesWritten, portMAX_DELAY);
  }

  Serial.print("Peak sample: ");
  Serial.println(peak);
}

// ============================================================
// BUTTON STATE HELPERS
// ============================================================
bool allRecorded()
{
  for (int i = 1; i <= MAX_BUTTONS; i++)
  {
    if (!recorded[i]) return false;
  }
  return true;
}

void printFinishedOnce()
{
  if (!finishedPrinted && allRecorded())
  {
    finishedPrinted = true;
    Serial.println("finished");
  }
}

// ============================================================
// RECORD RFID KEYPAD BUTTON
// ============================================================
bool startRecording(int btn)
{
  stopAmp();
  startMic();

  String fn = fileNameForButton(btn);

  if (SD.exists(fn.c_str()))
  {
    SD.remove(fn.c_str());
  }

  file = SD.open(fn.c_str(), FILE_WRITE);

  if (!file)
  {
    Serial.println("Failed to open WAV file");
    stopMic();
    return false;
  }

  writeWavHeader(file, SAMPLE_RATE, 16, 1, 0);

  totalDataBytes = 0;

  Serial.print("Recording RFID button ");
  Serial.println(btn);
  Serial.println("Speak now...");

  activeButton = btn;
  appState = APP_RECORDING;

  return true;
}

// ============================================================
// START SPECIAL RECORDING (1 = YES, 2 = NO, 3 = EMERGENCY)
// ============================================================
bool startSpecialRecording(int specialButton)
{
  stopAmp();
  startMic();

  String fn = specialFileName(specialButton);

  if (SD.exists(fn.c_str()))
  {
    SD.remove(fn.c_str());
  }

  file = SD.open(fn.c_str(), FILE_WRITE);

  if (!file)
  {
    Serial.println("Failed to open special WAV file");
    stopMic();
    return false;
  }

  writeWavHeader(file, SAMPLE_RATE, 16, 1, 0);

  totalDataBytes = 0;
  activeSpecialButton = specialButton;

  if (specialButton == 1) Serial.println("Recording YES...");
  if (specialButton == 2) Serial.println("Recording NO...");
  if (specialButton == 3) Serial.println("Recording EMERGENCY...");

  Serial.println("Speak now...");
  Serial.println("Press the SAME button again to SAVE.");

  appState = APP_RECORDING;

  return true;
}

// ============================================================
// FINISH RFID RECORDING
// ============================================================
void finishRecording()
{
  if (file)
  {
    writeWavHeader(file, SAMPLE_RATE, 16, 1, totalDataBytes);
    file.close();
  }

  stopMic();

  recorded[activeButton] = true;

  Serial.print("Saved RFID button ");
  Serial.println(activeButton);

  activeButton = 0;
  activeKey = 0;
  appState = APP_IDLE;

  printFinishedOnce();
}

// ============================================================
// FINISH SPECIAL RECORDING
// ============================================================
void finishSpecialRecording()
{
  if (file)
  {
    writeWavHeader(file, SAMPLE_RATE, 16, 1, totalDataBytes);
    file.close();
  }

  stopMic();

  if (activeSpecialButton == 1)
  {
    yesState = SPECIAL_SAVED;
    Serial.println("YES audio permanently saved.");
  }

  if (activeSpecialButton == 2)
  {
    noState = SPECIAL_SAVED;
    Serial.println("NO audio permanently saved.");
  }

  if (activeSpecialButton == 3)
  {
    emergencyState = SPECIAL_SAVED;
    Serial.println("EMERGENCY audio permanently saved.");
  }

  activeSpecialButton = 0;
  appState = APP_IDLE;
}

// ============================================================
// RECORD AUDIO CHUNK
// ============================================================
void recordAudioChunk()
{
  if (appState != APP_RECORDING || !micRunning)
  {
    return;
  }

  int32_t i2sBuffer[256];
  int16_t samples16[256];
  size_t bytesRead = 0;

  i2s_read(
    I2S_MIC_PORT,
    (void *)i2sBuffer,
    sizeof(i2sBuffer),
    &bytesRead,
    portMAX_DELAY
  );

  int samplesCount = bytesRead / sizeof(int32_t);

  for (int i = 0; i < samplesCount; i++)
  {
    int32_t sample = i2sBuffer[i] >> 16;

    // Fixed recording gain
    sample = sample * REC_GAIN;

    if (sample > 32767)  sample = 32767;
    if (sample < -32768) sample = -32768;

    samples16[i] = (int16_t)sample;
  }

  file.write((uint8_t *)samples16, samplesCount * sizeof(int16_t));
  totalDataBytes += samplesCount * sizeof(int16_t);
}

// ============================================================
// PLAY RFID BUTTON
// ============================================================
void playButton(int btn)
{
  stopMic();
  startAmp();

  String fn = fileNameForButton(btn);
  File f = SD.open(fn.c_str(), FILE_READ);

  if (!f)
  {
    Serial.println("No recording found for this button");
    stopAmp();
    return;
  }

  Serial.print("Playing RFID button ");
  Serial.println(btn);

  streamWavToAmp(f);

  f.close();

  delay(50);
  i2s_zero_dma_buffer(I2S_AMP_PORT);

  Serial.println("Playback done");

  stopAmp();
}

// ============================================================
// PLAY SPECIAL BUTTON (1 = YES, 2 = NO, 3 = EMERGENCY)
// ============================================================
void playSpecialButton(int specialButton)
{
  stopMic();
  startAmp();

  String fn = specialFileName(specialButton);
  File f = SD.open(fn.c_str(), FILE_READ);

  if (!f)
  {
    Serial.println("No special recording found");
    stopAmp();
    return;
  }

  if (specialButton == 1) Serial.println("Playing YES");
  if (specialButton == 2) Serial.println("Playing NO");
  if (specialButton == 3) Serial.println("Playing EMERGENCY");

  streamWavToAmp(f);

  f.close();

  delay(50);
  i2s_zero_dma_buffer(I2S_AMP_PORT);

  Serial.println("Special playback done");

  stopAmp();
}

// ============================================================
// HANDLE RFID KEYPAD KEY
// ============================================================
void handleKey(char k)
{
  int btn = buttonIndexFromKey(k);

  if (btn == 0) return;

  if (appState == APP_IDLE)
  {
    if (!recorded[btn])
    {
      activeKey = k;

      if (!startRecording(btn))
      {
        activeKey = 0;
      }
    }
    else
    {
      appState = APP_PLAYING;
      playButton(btn);
      appState = APP_IDLE;
    }
  }
  else if (appState == APP_RECORDING)
  {
    if (
      activeSpecialButton == 0 &&
      activeKey == k &&
      btn == activeButton
    )
    {
      finishRecording();
    }
  }
}

// ============================================================
// HANDLE SPECIAL BUTTON
// First press = RECORD, second = SAVE, later = PLAY
// ============================================================
void handleSpecialButton(int specialButton)
{
  byte *state = nullptr;

  if (specialButton == 1) state = &yesState;
  if (specialButton == 2) state = &noState;
  if (specialButton == 3) state = &emergencyState;

  if (state == nullptr) return;

  if (*state == SPECIAL_NOT_RECORDED)
  {
    if (startSpecialRecording(specialButton))
    {
      *state = SPECIAL_RECORDING;
    }
    return;
  }

  if (*state == SPECIAL_RECORDING && activeSpecialButton == specialButton)
  {
    finishSpecialRecording();
    return;
  }

  if (*state == SPECIAL_SAVED)
  {
    appState = APP_PLAYING;
    playSpecialButton(specialButton);
    appState = APP_IDLE;
    return;
  }
}

// ============================================================
// VOLUME UP / DOWN
// ============================================================
void volumeUp()
{
  if (volumeLevel < MAX_VOLUME_LEVEL)
  {
    volumeLevel++;
    Serial.print("Volume level: ");
    Serial.println(volumeLevel);
  }
  else
  {
    Serial.println("Volume already at maximum: 30");
  }

  playVolumeBeep();
}

void volumeDown()
{
  if (volumeLevel > MIN_VOLUME_LEVEL)
  {
    volumeLevel--;
    Serial.print("Volume level: ");
    Serial.println(volumeLevel);
  }
  else
  {
    Serial.println("Volume already at minimum: 1");
  }

  playVolumeBeep();
}

// ============================================================
// DELETE CURRENT RFID PAGE RECORDINGS
// (does NOT delete yes/no/emergency)
// ============================================================
void deleteCurrentRFIDRecordings()
{
  if (currentPageUID == "DEFAULT")
  {
    Serial.println("DELETE ignored: no RFID card is currently loaded.");
    return;
  }

  Serial.println();
  Serial.println("==============================");
  Serial.println("DELETING CURRENT RFID PAGE");
  Serial.print("RFID UID: ");
  Serial.println(currentPageUID);
  Serial.println("==============================");

  for (int i = 1; i <= MAX_BUTTONS; i++)
  {
    String fn = fileNameForButton(i);

    if (SD.exists(fn.c_str()))
    {
      if (SD.remove(fn.c_str()))
      {
        Serial.print("Deleted: ");
        Serial.println(fn);
      }
      else
      {
        Serial.print("Failed to delete: ");
        Serial.println(fn);
      }
    }

    recorded[i] = false;
  }

  finishedPrinted = false;

  Serial.println("RFID recordings deleted.");
  Serial.println("YES/NO/EMERGENCY recordings were NOT deleted.");
  Serial.println();
}

// ============================================================
// LOAD SPECIAL AUDIO STATES FROM SD
// ============================================================
void loadSpecialAudioStates()
{
  if (SD.exists("/yes.wav"))
  {
    yesState = SPECIAL_SAVED;
    Serial.println("YES audio found on SD -> permanently saved");
  }
  else
  {
    yesState = SPECIAL_NOT_RECORDED;
  }

  if (SD.exists("/no.wav"))
  {
    noState = SPECIAL_SAVED;
    Serial.println("NO audio found on SD -> permanently saved");
  }
  else
  {
    noState = SPECIAL_NOT_RECORDED;
  }

  if (SD.exists("/emergency.wav"))
  {
    emergencyState = SPECIAL_SAVED;
    Serial.println("EMERGENCY audio found on SD -> permanently saved");
  }
  else
  {
    emergencyState = SPECIAL_NOT_RECORDED;
  }
}

// ============================================================
// RFID: UID PRINTING
// ============================================================
void printUID(uint8_t *uid, uint8_t uidLength)
{
  Serial.print("UID: ");

  for (uint8_t i = 0; i < uidLength; i++)
  {
    if (uid[i] < 0x10) Serial.print("0");

    Serial.print(uid[i], HEX);

    if (i < uidLength - 1) Serial.print(":");
  }

  Serial.println();
}

String uidToString(uint8_t *uid, uint8_t uidLength)
{
  String s = "";

  for (uint8_t i = 0; i < uidLength; i++)
  {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }

  s.toUpperCase();
  return s;
}

// ============================================================
// RFID: LOAD PAGE
// ============================================================
void loadPage(const String &pageUID)
{
  currentPageUID = pageUID;

  String dir = "/" + currentPageUID;

  if (!SD.exists(dir.c_str()))
  {
    SD.mkdir(dir.c_str());
    Serial.print("Created new page folder: ");
    Serial.println(dir);
  }

  finishedPrinted = false;

  for (int i = 1; i <= MAX_BUTTONS; i++)
  {
    String fn = fileNameForButton(i);
    recorded[i] = SD.exists(fn.c_str());
  }

  Serial.print("Page loaded: ");
  Serial.println(currentPageUID);

  Serial.println("YES/NO/EMERGENCY files are independent of this RFID page.");
}

// ============================================================
// RFID: CHECK FOR CARD
// ============================================================
void checkRFID()
{
  if (appState != APP_IDLE) return;

  unsigned long now = millis();

  if (now - lastRFIDPollMs < RFID_POLL_INTERVAL_MS) return;

  lastRFIDPollMs = now;

  bool success = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A,
    uid,
    &uidLength,
    RFID_POLL_TIMEOUT_MS
  );

  if (!success) return;

  String currentUID = uidToString(uid, uidLength);

  if (currentUID == lastUID && millis() - lastRFIDTime < 2000)
  {
    return;
  }

  lastUID = currentUID;
  lastRFIDTime = millis();

  Serial.println();
  Serial.println("==============================");
  Serial.println("RFID CARD DETECTED");
  printUID(uid, uidLength);
  Serial.println("==============================");
  Serial.println();

  if (currentUID != currentPageUID)
  {
    loadPage(currentUID);
  }
}

// ============================================================
// HANDLE MODE BUTTONS
// ============================================================
void handleModeButton(int modeButton)
{
  if (modeButton == 1)
  {
    if (appState == APP_IDLE) volumeUp();
    return;
  }

  if (modeButton == 2)
  {
    if (appState == APP_IDLE) volumeDown();
    return;
  }

  if (modeButton == 3)
  {
    if (appState == APP_IDLE ||
        (appState == APP_RECORDING && activeSpecialButton == 1))
    {
      handleSpecialButton(1);
    }
    return;
  }

  if (modeButton == 4)
  {
    if (appState == APP_IDLE ||
        (appState == APP_RECORDING && activeSpecialButton == 2))
    {
      handleSpecialButton(2);
    }
    return;
  }

  if (modeButton == 5)
  {
    if (appState == APP_IDLE ||
        (appState == APP_RECORDING && activeSpecialButton == 3))
    {
      handleSpecialButton(3);
    }
    return;
  }

  if (modeButton == 6)
  {
    if (appState == APP_IDLE) deleteCurrentRFIDRecordings();
    return;
  }
}

// ============================================================
// SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  writePCF8574(0xFF);

  Serial.println();
  Serial.println("PCF8574 I2C Keypad Ready");
  Serial.println("Keypad PCF8574 address: 0x27");
  Serial.println("C1=P7 C2=P6 C3=P5");
  Serial.println("R1=P4 R2=P2 R3=P1 R4=P0");

  writeModePCF8574(0xFF);

  Serial.println();
  Serial.println("Mode PCF8574 Ready");
  Serial.println("Mode PCF8574 address: 0x26");
  Serial.println("P7 = Volume UP");
  Serial.println("P6 = Volume DOWN");
  Serial.println("P5 = YES");
  Serial.println("P4 = NO");
  Serial.println("P2 = EMERGENCY");
  Serial.println("P1 = DELETE RFID AUDIO");

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN))
  {
    Serial.println("SD Card init failed!");
    while (1);
  }

  Serial.println("SD Card Ready");

  if (!SD.exists("/DEFAULT"))
  {
    SD.mkdir("/DEFAULT");
  }

  for (int i = 1; i <= MAX_BUTTONS; i++)
  {
    String fn = fileNameForButton(i);
    recorded[i] = SD.exists(fn.c_str());
  }

  loadSpecialAudioStates();

  Wire.beginTransmission(PN532_ADDRESS);
  byte error = Wire.endTransmission();

  if (error == 0)
  {
    Serial.println("PN532 found at 0x24");
  }
  else
  {
    Serial.println("ERROR: PN532 not found at 0x24");
    Serial.print("I2C error code: ");
    Serial.println(error);
  }

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();

  if (!versiondata)
  {
    Serial.println("ERROR: Could not communicate with PN532");
    Serial.println("Check wiring, address (0x24), and I2C mode jumpers.");

    while (1)
    {
      delay(1000);
    }
  }

  Serial.print("PN532 firmware: ");
  Serial.print((versiondata >> 24) & 0xFF, HEX);
  Serial.print(".");
  Serial.print((versiondata >> 16) & 0xFF, HEX);
  Serial.print(".");
  Serial.println((versiondata >> 8) & 0xFF, HEX);

  nfc.SAMConfig();

  Serial.println("RFID reader ready");

  Serial.println();
  Serial.println("======================================");
  Serial.println("AAC DEVICE READY");
  Serial.println("======================================");

  Serial.println();
  Serial.println("RFID KEYPAD:");
  Serial.println("First press  = Record");
  Serial.println("Second press = Save");
  Serial.println("Later presses = Play");

  Serial.println();
  Serial.println("GLOBAL BUTTONS:");
  Serial.println("YES: First=Record, Second=Save, Later=Play");
  Serial.println("NO: First=Record, Second=Save, Later=Play");
  Serial.println("EMERGENCY: First=Record, Second=Save, Later=Play");

  Serial.println();
  Serial.println("DELETE P1 deletes RFID recordings ONLY.");
  Serial.println("YES/NO/EMERGENCY are NOT deleted by P1.");

  Serial.println();
  Serial.print("Recording gain (fixed): ");
  Serial.println(REC_GAIN);
  Serial.print("Playback volume level: ");
  Serial.println(volumeLevel);
  Serial.println("Volume range: 1 to 30");
  Serial.println("P7 = volume UP, P6 = volume DOWN (beep confirms)");

  Serial.println();
  Serial.println("Tap an RFID card to switch pages.");
  Serial.println("======================================");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop()
{
  // Must run continuously so mic data is not lost
  if (appState == APP_RECORDING)
  {
    recordAudioChunk();
  }

  // ---------------- MODE BUTTONS ----------------
  int modeButton = scanModeButtons();

  if (modeButton != 0)
  {
    unsigned long now = millis();

    if (now - lastKeyEventMs > DEBOUNCE_MS)
    {
      lastKeyEventMs = now;

      if (appState == APP_RECORDING)
      {
        if (
          activeSpecialButton != 0 &&
          (modeButton == 3 || modeButton == 4 || modeButton == 5)
        )
        {
          int pressedSpecial = 0;

          if (modeButton == 3) pressedSpecial = 1;
          if (modeButton == 4) pressedSpecial = 2;
          if (modeButton == 5) pressedSpecial = 3;

          if (pressedSpecial == activeSpecialButton)
          {
            Serial.println("Save button pressed.");
            handleSpecialButton(pressedSpecial);
          }
        }
      }
      else if (appState == APP_IDLE)
      {
        Serial.print("Mode button: ");
        Serial.println(modeButton);

        handleModeButton(modeButton);
      }
    }

    waitModeButtonRelease();
  }

  // ---------------- 4x3 KEYPAD ----------------
  char k = scanKeypad();

  if (k != 0)
  {
    unsigned long now = millis();

    if (now - lastKeyEventMs > DEBOUNCE_MS)
    {
      lastKeyEventMs = now;

      Serial.print("Key: ");
      Serial.println(k);

      handleKey(k);
    }

    while (scanKeypad() != 0)
    {
      delay(10);
    }
  }

  // ---------------- RFID ----------------
  checkRFID();

  delay(10);
}
