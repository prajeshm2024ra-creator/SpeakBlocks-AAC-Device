# SpeakBlocks-AAC-Device
Modular ESP32-based AAC device for speech-impaired users. RFID page cards, swappable keypad grids and recordable voice buttons give customizable communication.
SpeakBlocks is a low-cost, portable augmentative and alternative communication (AAC) device for people with speech difficulties, especially children and adults on the autism spectrum. Built on an ESP32, it uses RFID “page” cards to switch between vocabulary sets, with swappable keypad grids (2x2, 3x3, 4x4) that can be matched to each user’s needs. Caregivers can record, play and delete custom voice messages on each button. Each card’s unique ID links to its own folder on the SD card, so the right audio loads automatically. Dedicated YES, NO and EMERGENCY buttons, plus volume controls, work on every page. The device runs from USB or a rechargeable LiPo battery, and is housed in a 3D-printed enclosure designed in CAD.

Key Features
RFID page cards that auto-load the matching voice set from the SD card
Hot-swappable keypad grids
Record, play and delete modes for caregiver-recorded voice
Global YES / NO / EMERGENCY buttons
Adjustable volume with a beep confirmation
I2C expanders (PCF8574) to solve the ESP32’s GPIO shortage
Dual power: USB and LiPo with a buck-boost regulator
Custom KiCad schematic and CAD-designed enclosure

## Pin Connections (ESP32-WROOM-32)

### I2C Bus (shared by all I2C devices)
| Signal | ESP32 GPIO |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |

| I2C Device | Address |
|---|---|
| PCF8574 (keypad expander) | 0x27 |
| PCF8574 (mode-button expander) | 0x26 |
| PN532 RFID reader | 0x24 |

### PN532 RFID Control Pins
| PN532 Pin | ESP32 GPIO |
|---|---|
| IRQ | GPIO 4 |
| RESET | GPIO 13 |

### INMP441 Microphone (I2S_NUM_0)
| Mic Pin | ESP32 GPIO |
|---|---|
| SCK / BCLK | GPIO 14 |
| WS | GPIO 15 |
| SD | GPIO 33 |

### MAX98357A Amplifier (I2S_NUM_1)
| Amp Pin | ESP32 GPIO |
|---|---|
| BCLK | GPIO 26 |
| LRC | GPIO 25 |
| DIN | GPIO 27 |

### SD Card Module (SPI)
| SD Pin | ESP32 GPIO |
|---|---|
| CS | GPIO 5 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| SCK | GPIO 18 |

### 4x3 Keypad (via PCF8574 @ 0x27)
| Keypad Line | PCF8574 Pin |
|---|---|
| Column 1 / 2 / 3 | P7 / P6 / P5 |
| Row 1 / 2 / 3 / 4 | P4 / P2 / P1 / P0 |

Key layout:

| | C1 | C2 | C3 |
|---|---|---|---|
| R1 | 1 | 2 | 3 |
| R2 | 4 | 5 | 6 |
| R3 | 7 | 8 | 9 |
| R4 | * | 0 | # |

### Mode Buttons (via PCF8574 @ 0x26)
| Button | PCF8574 Pin |
|---|---|
| Volume UP | P7 |
| Volume DOWN | P6 |
| YES | P5 |
| NO | P4 |
| EMERGENCY | P2 |
| DELETE (current RFID page audio) | P1 |

### Power Rails
| Rail | Source | Powers |
|---|---|---|
| 3.3V | TPS63020 buck-boost | ESP32, PCF8574 x2, PN532, INMP441, SD module |
| 5V | Boost module from battery | MAX98357A only |

### Notes
- Buttons are active-low (pressed = LOW), with PCF8574 internal pull-ups.
- Programming is through the TX0 / RX0 header with an external USB-to-TTL converter.
- The firmware runs the mic and amp I2S drivers one at a time, switching between them.
