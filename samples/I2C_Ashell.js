var i2c = require("i2c");
var GROVE_LCD_DISPLAY_ADDR = 0x3E;
var GROVE_RGB_BACKLIGHT_ADDR = 0x62;
print("I2C sample...");
var setupData = new Buffer(2);
var msgData = new Buffer("@HELLO!");
var redData = new Buffer([0x04, 0]);
var greenData = new Buffer([0x03, 0]);
var blueData = new Buffer([0x02, 0]);
var hello = false;
var i2cDevice = i2c.open({ bus: 0, speed: 100 });
function init() {
    setupData.writeUInt8(0, 0);
    setupData.writeUInt8(1 << 0, 1);
    i2cDevice.write(GROVE_LCD_DISPLAY_ADDR, setupData);
    var setup = (1 << 5 | 1 << 3 | 0 << 2 | 1 << 4);
    setupData.writeUInt8(0, 0);
    setupData.writeUInt8(setup, 1);
    i2cDevice.write(GROVE_LCD_DISPLAY_ADDR, setupData);
    setup = (1 << 3 | 1 << 2 | 1 << 1 | 1 << 0);
    setupData.writeUInt8(setup, 1);
    i2cDevice.write(GROVE_LCD_DISPLAY_ADDR, setupData);
}
function resetCursor() {
    var col = 0x80;
    setupData.writeUInt8(1 << 7, 0);
    setupData.writeUInt8(col, 1);
    i2cDevice.write(GROVE_LCD_DISPLAY_ADDR, setupData);
}
function changeRGB(red, green, blue) {
    print("RGB = " + red + " : " + green + " : " + blue);
    redData.writeUInt8(red, 1);
    greenData.writeUInt8(green, 1);
    blueData.writeUInt8(blue, 1);
    i2cDevice.write(GROVE_RGB_BACKLIGHT_ADDR, blueData);
    i2cDevice.write(GROVE_RGB_BACKLIGHT_ADDR, redData);
    i2cDevice.write(GROVE_RGB_BACKLIGHT_ADDR, greenData);
}
function writeWord() {
    resetCursor();
    if (hello) {
        msgData.write("@HELLO!");
        changeRGB(207, 83, 0);
    }
    else {
        msgData.write("@WORLD!");
        changeRGB(64, 224, 228);
    }
    hello = !hello;
    i2cDevice.write(GROVE_LCD_DISPLAY_ADDR, msgData);
}
init();
i2cDevice.write(GROVE_LCD_DISPLAY_ADDR, msgData);
changeRGB(64, 224, 228);
setInterval(writeWord, 2000);
