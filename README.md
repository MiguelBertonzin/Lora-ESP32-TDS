# Emissor e Receptor 
Medidor de TDS Sensor de Condutividade da Água Analógico.
Módulo LoRa 915Mhz XL1276/ SX1276.
ESP32

idf.py set-target ESP32
idf.py menuconfig
Lora configuration -> freq to use (915Mhz) -> Enable Advanced settings -> pinagem
SCLK  5
MOSI  27
MISO  19
NSS       18
RST       14
DIO0      26
DIO1      35
Necessário realizar a troca do modo emissor ou receptor no menuconfig.
idf.py flash
