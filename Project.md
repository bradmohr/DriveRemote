I want to make a remote program Variable Speed Drive.

ABB ACS550 Drive (ACS550-U1-072A-4)
Serial Number: 2111201781
Manufacturing Date: March 22, 2011
Input: 3-phase, 380–480 Vac, 72 A
Output: 3-phase, 72 A, 37 kW / 50 HP
Firmware: V3.13D


Will install following screen to wired to the rs459 terminal of the wave share

ESP32 S3 4inch LCD Development Board 480x480 TouchScreen Display TVBOX LVGL HMI Sensor CAN RS485 Waveshare ESP32-S3-Touch-LCD-4

I have included in the project html page from another porject as an example only  of the modus paramaters.
I want to to create a program on the wavesharre similar looking send modbus commards to the connected ABB drive.

I want a similar app on the waveshare device using platformio. I also want to use mqtt messages in parellel with the modus message. 

Example Out going from waveshare:  if a modbus message came in from parameter 1010 then Mqtt message would be Drive/1010 and the payload would be scaled to the correct value. Do this for all out going messages 

Example In coming or mqtt messages to the waveshare:
So if i set the speed to 3000 by button press on the waveshare or and incoming Mqtt message (Drive/XXXX) apply the scalling factor as payload to adjust the speed.The message would have he rpm so it needs to be scaled.
The topic name of the mqttt message is be the same as the parmater number. XXXX is the paramater number.

The waveshare would need to connect to wifi and mqtt so have a setup page for that. Mqtt needs username and password with an option for that.Needs to remember the settings.

I would like a loging page or maybe at the bottom of the display .Try to fit the start and stop and speed control on the first page. 

I would also like a watchdog function that reboots the device if it hangs.


