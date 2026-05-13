#Parrot ESP Project

This is the parrotpi project, originally found at ../parrotpi, but converted for use on an ESP32.

## Notes about Beak Servo Pins
-  Servo Orange -> ESP +5 (19)
-  Servo Brown  -> ESP GND (38)
-  Serv Yellow -> ESP GPIO 17 (30)

## Notes about Max357 Audio Card
- Max BCLK -> GPIO 27 (11)
- Max LRC  -> GPIO 26 (10)
- Max DIN  -> GPIO 25 (9)
- Max Vin  -> 5v (19)
- Max GND  -> GND 38


## Notes about uploading prerecorded WAV files from parrotpi project to parrotesp project.
1. This can be touchy, may have to try several times
1. You'll need the arduino-littlefs-upload-1.x.x.visix plugin installed for the Arduino IDE.
2. Put the .wav files under the ./data directory of the parrotesp folder.
3. (Important) Turn off the serial monitor window/view
4. Set the file upload speed to the 115K option
4. Hold down the BOOT switch on the ESP and, while still holding that down, press the EN switch on the ESP, then let up the BOOT switch
4. Use the "littlefs" command up upload the files to the ESP.  Use ctrl-sh-P, choose littlefs, then the Upload option.
