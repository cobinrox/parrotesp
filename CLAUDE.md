# Parrot ESP Project

(Note: This is based on the `parrotpi` Raspberry Pi project, originally found at `../parrotpi`, but converted for use on an ESP32.)

This is a  web server for an ESP32-controlled anamatronic toy parrot. It serves a web page over a private Access Point (AP) SSID and
sends REST calls to the server such as move beak open/close, increase/
decrease volume, playback a phrase (previously recorded wav file), and
provide a push-to-talk socketio microphone near-realtime stream to
playback through the parrot.  It also has a few admin features that allow the user to save off his/her ad-hoc voice (when using push-to-talk capability) so that it can be replayed at a later tiome. When the parrot noises are played,
the beak servo opens and closes while playing.  It also has a start
up wav file that it plays upon start up to make sure that the sound card
(MAX-98357A) was initialized properly and is working ok.

## Requirements/Features
- Web-page user interface, controlled by backend REST/HTTPS server
- Uses a private Access Point/w SSID of parrotpi-test (to be changed to parrotesp in the future)
- Web page gets periodic status from backend
- Web page allows users to:
   - Set volume and pitch playback
   - Playback pre-recorded wav files
   - Use a walkie-talkie mode to have the parrot speak the user's voice over the client microphone and record that snippet and then be able to play it back
   - The walkie talkie mode uses a button called Hold to Talk which is enabled on the web page only after the user has requested access to the microphone via the Enable Microphone button.  Then, when they press the Hold to Talk button, a little red ring appears around the button to signify that it is live, and a little microphone meter appears to show relative microphone loudness/signal.


## Software
### IDE
You'll need 
- Arduino IDE and a USB cable between your PC and the ESP32 for downloading the executable software.
- LittleFS IDE plugin

- ESP Audio Library (see #include of main .ino file for specific library version)
- WebSockets Library

### Source Code
- The main directory contains several test sub-directories which can be loaded onto an ESP32 for low-level testing, but the main project is under the `esp32_parrot_full` directory as an ino (C++) file.
- Also under the directory is a subdirectory, `data`, containing supporting files:
  - *.wav These are pre-recorded phrases that the parrot can be commanded to play back
  - index.html This is the web page
  - IMPORTANT: You will also need to create YOUR OWN private/public key pair in this directory (see Public/Private Key Creation section below)

### Public/Private Key Creation
The project's walkie-talkie feature requires that the browser has permission to use the microphone of the client (e.g. the micropohone of a cell phone), and that requires that the page be served over HTTPS.  Therefore we need to provide a valid or at least self-signed public/private key for the HTTPS/TLS protocol.  So you must create a public and private key for the project.  You can follow these basic steps to do this.  This example assumes using gitbash terminal on a Windows.
```
1. Open bash/gitbash terminal
mkdir ~/parrot-certs
cd ~/parrot-certs

2. vi openssl-san.cnf

3. Paste in the following into the new file and save:
[req]
default_bits = 2048
prompt = no
default_md = sha256
x509_extensions = v3_req
distinguished_name = dn

[dn]
C = US
ST = Colorado
L = Colorado Springs
O = ParrotESP
OU = Development
CN = 192.168.4.1

[v3_req]
subjectAltName = @alt_names

[alt_names]
IP.1 = 192.168.4.1

4. Run:
 openssl req -x509 -nodes -days 10000 \
-newkey rsa:2048 \
-keyout key.pem \
-out cert.pem \
-config openssl-san.cnf

5. Copy the cert.pem and key.pem files to the directory:
esp32_parrot_full/data
```

## Hardware
- ESP32 
- MAX-98357A amplifier/audio card
- 4 Ohm Speaker
- Micro servo (to control the parrot's beak)
- Bespoke lever attached to beak and moved via the servo
- Battery pack/w at least 5v/1.5A

## Notes about Beak Servo Pins
-  Servo Orange -> ESP +5 (19)
-  Servo Brown  -> ESP GND (38)
-  Serv Yellow -> ESP GPIO 18 (30)

## Notes about Max357 Audio Card
- Max BCLK -> ESP GPIO 27 (11)
- Max LRC  -> ESP GPIO 26 (10)
- Max DIN  -> ESP GPIO 25 (9)
- Max Vin  -> ESP 5v (19)
- Max GND  -> ESP GND (38)

## (Optional) Notes about RST Momentary External Switch
- Switch Pin A -> ESP RESET/EN (2)
- Switch Pin B -> ESP GND (14)


## Notes about uploading prerecorded WAV files and contents of data subdirectory from parrotpi project to parrotesp project.
1. You only have about 1.2-1.3MB of room in the /data directory on the ESP32
1. Uploading this dir can be touchy, may have to try several times
1. You'll need the arduino-littlefs-upload-1.x.x.visix plugin installed for the Arduino IDE.
2. Put the .wav files under the ./data directory of the parrotesp folder.
3. (Important) Turn off the serial monitor window/view!
4. Set the file upload speed to the 115K option
5. Click Ctrl-Shift-P
4. Then find and use the littlefs, then the Upload option. Watch the LittleFS output tab.
5. When "COnnecting..." shows on the LittleFS output tab, click the BOOT button on the ESP and hold until it shows that files are being Written (e.g. `Writing at 0x0033333...`)

## Notes about Using Audacity to Create New Audio Clips
1. Click File, New
2. Click Record button
3. Speak, then click Stop button
4. Click on new track that was created
5. Right mouse click, choose Split Stereo to Mono
6. Click the X on one of the newly split tracks
7. Click Ctrl-A to select entire track
8. Click Effects, Change Pitch, Choose about 25% Frequency Change, then click Preview and adjust percentage to desired pitch
9. Click Apply
10. Click File, Export Audio..., Export to Computer
11. Enter file name and folder, choose Sample Rate 22050, Encoding Signed 16-bit PCM, click Export