#include <SmingCore.h>
#include <Wire.h>
#include <SPI.h>
#include <Libraries/MCP23S17/MCP23S17.h>

// // Instantiate an object called "inputchip" on an MCP23S17 device at  address 1 = 0b00000001 and CS pin = GPIO16
MCP inputchip(1, 16);
// Instantiate an object called "outputchip" on an MCP23S17 device at address 0 = 0b00000010 and CS pin = GPIO16
MCP outputchip(0, 16);

void loop();

Timer procTimer;

void init()
{
	Serial.begin(SERIAL_BAUD_RATE);  // 115200 by default
	Serial.systemDebugOutput(false); // Allow debug output to serial
	Serial.println("<-= Sming start =->");

	// Set higher CPU freq & disable wifi sleep
	System.setCpuFrequency(CpuCycleClockFast::cpuFrequency());

#ifndef DISABLE_WIFI
	wifi_set_sleep_type(NONE_SLEEP_T);
	WifiStation.enable(false);
	WifiAccessPoint.enable(false);
#endif

	inputchip.begin();
	outputchip.begin();

	inputchip.pinMode(0xFFFF);	 // Use word-write mode to set all of the pins on inputchip to be inputs
	inputchip.pullupMode(0xFFFF);  // Use word-write mode to Turn on the internal pull-up resistors.
	inputchip.inputInvert(0x0000); // Use word-write mode to invert the inputs so that logic 0 is read as HIGH
	outputchip.pinMode(0x0000);	// Use word-write mode to Set all of the pins on outputchip to be outputs

	procTimer.initializeMs(200, loop).start();
}

void loop()
{
	int value;						 // declare an integer to hold the value temporarily.
	value = inputchip.digitalRead(); // read the input chip in word-mode, storing the result in "value"
	outputchip.digitalWrite(value);  // write the output chip in word-mode, using our variable "value" as the argument
	// outputchip.digitalWrite(inputchip.digitalRead()); // this one line replaces the three above, and is more efficient
}
