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
