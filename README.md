# ESP32-temp-recorder
ESP32 Temp, Humidity, Pressure Recorder. With MQTT upload and NTP time update function.

### How to use that
1. Make sure both VSCode and Platform I/O extension are installed.
2. Clone this repo, open it using the Open Folder method by VSCode.
3. Fill in your MQTT's server Token and Topic, and Wi-Fi's SSID and Password.
4. Edit the button's link of the simple webpage, like links to the Dashboard website.
4. Build and upload it, both the program and the file system image!

### Power source and devices
* AM2320 - 3.3v
* BMP085 - 3.3v
* 1602 LCD with I2C board - 5v

### Important Pinouts for the board
* Pin 21: SDA
* Pin 22: SCL
* Just wired up all device's I2C pin together, and add the pull-up resistors!

## Functions
* Updates time from NTP server every hour (Stdtime server)
* Updates clock every 500ms
* Measure Temperature, Humidity, Pressure every 30 seconds
* Uploads data to the MQTT every 6 minutes (10 data points each hour)
* Shows recently data on the LCD and simple web server

## Gallery
* Adafruit I/O dashboard example  
  ![capture1](/captures/cap1.png)
* Simple web server  
  ![capture2](/captures/cap2.png)
