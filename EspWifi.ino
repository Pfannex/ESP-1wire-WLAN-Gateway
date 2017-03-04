#include "Arduino.h"
#include "WiFiUDP.h"
#include "ESP8266WiFi.h"
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include "FS.h"
#include "detail/RequestHandlersImpl.h"

extern "C" {
#include "user_interface.h"
}

// source: http://esp8266-re.foogod.com/wiki/SPI_Flash_Format
typedef struct __attribute__((packed))
{
  uint8   magic;
  uint8   unknown;
  uint8   flash_mode;
  uint8   flash_size_speed;
  uint32  entry_addr;
} HeaderBootMode1;

// globals
String otaFileName;
File otaFile;
bool lastWiFiStatus = false;

// Multicast declarations
IPAddress ipMulti(239, 0, 0, 57);
unsigned int portMulti = 12345;      // local port to listen on

WiFiUDP WiFiUdp;
ESP8266WebServer server(80);

void setupEspWifi() {
//  WiFi.hostname("bla");

  Serial.print("Hostname: "); Serial.println(WiFi.hostname());
  Serial.print("MAC:      "); Serial.println(WiFi.macAddress());

  setupWifi();

  // init http server
  setupHttp();
}

void loopEspWifi() {
  statusWifi();

  unsigned int start = millis();
  // process http requests
  server.handleClient();
  if (httpRequestProcessed) {
    Serial.printf("%d ms\n", (millis() - start));
    httpRequestProcessed = false;
  }
}

void setupWifi() {
  Serial.println("starting WiFi");

  if (WiFi.getMode() != WIFI_STA)
    WiFi.mode(WIFI_STA);

  // force wait for connect
  lastWiFiStatus = !(WiFi.status() == WL_CONNECTED);
  statusWifi(true);
}

void statusWifi() {
  statusWifi(false);
}

void statusWifi(bool reconnect) {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected == lastWiFiStatus)
    return;
    
  if (!connected && reconnect) {
    byte retries = 20;
    while (retries > 0 && WiFi.status() != WL_CONNECTED) {
      retries--;
      delay(100);
    }
  }

  lastWiFiStatus = (WiFi.status() == WL_CONNECTED);
  if(lastWiFiStatus) {
    Serial.print("Wifi connected: ");
    Serial.print(WiFi.localIP());
    Serial.print("/");
    Serial.print(WiFi.subnetMask());
    Serial.print(" ");
    Serial.println(WiFi.gatewayIP());
    // trigger KVPUDP to reload config
    sendMultiCast("REFRESH CONFIG REQUEST");
  } else {
    Serial.println("Wifi not connected");
  }

  setupSoftAP();
}

void setupSoftAP() {
  bool run = (WiFi.status() != WL_CONNECTED);
  bool softAP = (ipString(WiFi.softAPIP()) != "0.0.0.0");
  
  if (run && !softAP) {
    Serial.print("starting SoftAP: ");
  
    String apSsid = PROGNAME;
    apSsid += "@" + getChipID();
    WiFi.softAP(apSsid.c_str(), getChipID().c_str());
    
    Serial.println("done (" + ipString(WiFi.softAPIP()) + ")");
  }

  if (!run && softAP) {
    Serial.print("stopping SoftAP: ");
  
    WiFi.softAPdisconnect(true);
    
    Serial.println("done");
  }
}

void configWifi(String action) {
  Serial.print("configWifi: " + action);

  if (action == "reset") {
    WiFi.disconnect();  // clear ssid and psk in EEPROM
    delay(1000);
    statusWifi();
  }
  if (action == "setup") {
    reconfigWifi(server.arg("ssid"), server.arg("password"));
  }

  httpRequestProcessed = true;
}

void reconfigWifi(String ssid, String password) {
  if (ssid != "" && (WiFi.SSID() != ssid || WiFi.psk() != password)) {
    Serial.print(" apply new config (" + ssid + ")");

    if (WiFi.status() == WL_CONNECTED) {
      WiFi.disconnect();
      delay(1000);
    }
    WiFi.begin(ssid.c_str(), password.c_str());
    statusWifi(true);
  }
}

void setupHttp() {
//  if (MDNS.begin(WiFi.hostname().c_str()))
//    Serial.println("MDNS responder started");

  Serial.print("starting WebServer");

  server.on("/", HTTP_GET, httpHandleRoot);

  server.on("/config", HTTP_GET, httpHandleConfig);
  server.on("/config", HTTP_POST, httpHandleConfig);
  server.onNotFound(httpHandleNotFound);
  server.addHandler(new FunctionRequestHandler(httpHandleOTA, httpHandleOTAData, ("/ota/" + getChipID() + ".bin").c_str(), HTTP_POST));

  server.begin();
  Serial.println();
}

void httpHandleRoot() {
  Serial.print("httpHandleRoot: ");
  String message = "Hostname: ";
  message += WiFi.hostname();
  message += "\r\nChip:     " + getChipID();
  message += "\r\nMAC:      ";
  message += WiFi.macAddress();
  message += "\r\nClient:   " + ipString(server.client().remoteIP());

  server.client().setNoDelay(true);
  server.send(200, "text/html", htmlBody(message));
  httpRequestProcessed = true;
}

void httpHandleConfig() {
  Serial.print("httpHandleConfig: ");
  String message = "", separator = "";

  if (server.method() == HTTP_GET) {
    for (uint8_t i=0; i<server.args(); i++) {
      if (message.length() > 0)
        separator = ",";
        
      if (server.argName(i) == "Version") {
          message += separator + "Version:";
          message += PROGNAME;
          message += ".";
          message += PROGVERS;
      }
      if (server.argName(i) == "ChipID") {
          message += separator + "ChipID:" + getChipID();
      }
      if (server.argName(i) == "Dictionary") {
          message += separator + "Dictionary:" + getDictionary();
      }
    }
  }
  
  if (server.method() == HTTP_POST) {
    // check required parameter ChipID
    if (server.args() == 0 || server.argName(0) != "ChipID" || server.arg(0) != getChipID()) {
      server.client().setNoDelay(true);
      server.send(403, "text/plain", "Forbidden");
      httpRequestProcessed = true;
    }
    
    if (server.arg("wifi") == "submit") {
      configWifi(server.arg("action"));
      server.client().setNoDelay(true);
      server.sendHeader("Location", "/");
      server.send(303, "text/plain", "See Other");
      return;
    }
      
    for (uint8_t i=1; i<server.args(); i++) {
      int value, value2;
      bool hasValue = false, hasValue2 = false;

      char r = server.argName(i)[0];
      String values = server.arg(i);

      hasValue=(values.length() > 0);
      if (hasValue) {
        int idx=values.indexOf(',', 0);
        value=atoi(values.substring(0, (idx == -1 ? values.length() : idx)).c_str());

        hasValue2=(idx > 0);
        if (hasValue2)
          value2=atoi(values.substring(idx + 1).c_str());
      }
      handleInput(r, hasValue, value, hasValue2, value2);
    }
  }
  
  server.client().setNoDelay(true);
  server.send(200, "text/plain", message);
  httpRequestProcessed = true;
}

void httpHandleNotFound() {
  Serial.print("httpHandleNotFound: ");
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.client().setNoDelay(true);
  server.send(404, "text/plain", message);
  httpRequestProcessed = true;
}

bool initOtaFile(String filename, String mode) {
  SPIFFS.begin();
  otaFile = SPIFFS.open(filename, mode.c_str());

  if (otaFile)
    otaFileName = filename;

  return otaFile;
}

void clearOtaFile() {
  if (otaFile)
    otaFile.close();
  if (SPIFFS.exists(otaFileName))
    SPIFFS.remove(otaFileName);
  otaFileName = "";
}

void httpHandleOTA() {
  String message = "httpHandleOTA: ";
  bool doUpdate = false;
  
  if (SPIFFS.exists(otaFileName) && initOtaFile(otaFileName, "r")) {
    message += otaFile.name();
    message += + " (";
    message += otaFile.size();
    message += " Bytes) received!";
    if ((doUpdate = Update.begin(otaFile.size()))) {
      message += "\nstarting upgrade!";
    } else
      clearOtaFile();
  }

  Serial.println(message);

  server.client().setNoDelay(true);
//  server.sendHeader("Location", "/");
//  server.send(303, "text/plain", "See Other");
  server.send(200, "text/plain", message);

  if (doUpdate) {
    Serial.print("starting Update: ");
    size_t written = Update.write(otaFile);
    clearOtaFile();

    if (!Update.end() || Update.hasError()) {
      Serial.println("failed!");
      Update.printError(Serial);
      Update.end(true);
    } else {
      Serial.println("ok, md5 is " + Update.md5String());
      Serial.println("restarting");
      delay(1000);
      ESP.reset();
    }
  }
  httpRequestProcessed = true;
}

void httpHandleOTAData() {
  static HeaderBootMode1 otaHeader;
 
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.print("httpHandleOTAData: " + upload.filename);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // first block with data
    if (upload.totalSize == 0) {
      memcpy(&otaHeader, &upload.buf[0], sizeof(HeaderBootMode1));
      Serial.printf(", magic: 0x%0x, size: 0x%0x, speed: 0x%0x\n", otaHeader.magic, ((otaHeader.flash_size_speed & 0xf0) >> 4), (otaHeader.flash_size_speed & 0x0f));

      if (otaHeader.magic == 0xe9)
        initOtaFile("/ota/" + upload.filename, "w");
    }
    Serial.print(".");
    if ((upload.totalSize % HTTP_UPLOAD_BUFLEN) == 20)
      Serial.println("\n");

    if (otaFile && otaFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.println("\nwriting file " + otaFileName + " failed!");
      clearOtaFile();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaFile) {
      bool uploadComplete = (otaFile.size() == upload.totalSize);
      
      Serial.printf("\nend: %s (%d Bytes)\n", otaFile.name(), otaFile.size());
      otaFile.close();
      
      if (!uploadComplete)     
        clearOtaFile();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.printf("\naborted\n");
    clearOtaFile();
  }
}

boolean sendMultiCast(String msg) {
  boolean result = false;

  if (WiFi.status() != WL_CONNECTED)
    return result;

  if (WiFiUdp.beginPacketMulticast(ipMulti, portMulti, WiFi.localIP()) == 1) {
    WiFiUdp.write(msg.c_str());
    WiFiUdp.endPacket();
    yield();  // force ESP8266 background tasks (wifi); multicast requires approx. 600 µs vs. delay 1ms
    result = true;
  }

  return result;
}

String ipString(IPAddress ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

String getChipID() {
  char buf[10];
  sprintf(buf, "%08d", (unsigned int)ESP.getChipId());
  return String(buf);
}

void handleInput(char r, bool hasValue, unsigned long value, bool hasValue2, unsigned long value2) {
  switch (r) {
    case 'v':
      // Version info
      handleCommandV();
      print_config();
      break;
    case ' ':
    case '\n':
    case '\r':
      break;
    default:
      handleCommandV();
/*
#ifndef NOHELP
      Help::Show();
#endif
*/      break;
    }
}

void handleSerialPort(char c) {
  static long value, value2;
  bool hasValue, hasValue2;
  char r = c;

  // reset variables
  value = 0; hasValue = false;
  value2 = 0; hasValue2 = false;
  
  byte sign = 0;
  // char is a number
  if ((r >= '0' && r <= '9') || r == '-'){
    byte delays = 2;
    while ((r >= '0' && r <= '9') || r == ',' || r == '-') {
      if (r == '-') {
        sign = 1;
      } else {
        // check value separator
        if (r == ',') {
          if (!hasValue || hasValue2) {
            print_warning(2, "format");
            return;
          }
          
          hasValue2 = true;
          if (sign == 0) {
            value = value * -1;
            sign = 0;
          }
        } else {
          if (!hasValue || !hasValue2) {
            value = value * 10 + (r - '0');
            hasValue = true;
          } else {
            value2 = value2 * 10 + (r - '0');
            hasValue2 = true;
          }
        }
      }
            
      // wait a little bit for more input
      while (Serial.available() == 0 && delays > 0) {
        delay(20);
        delays--;
      }

      // more input available
      if (delays == 0 && Serial.available() == 0) {
        return;
      }

      r = Serial.read();
    }
  }

  // Vorzeichen
  if (sign == 1) {
    if (hasValue && !hasValue2)
      value = value * -1;
    if (hasValue && hasValue2)
      value2 = value2 * -1;
  }

  handleInput(r, hasValue, value, hasValue2, value2);
}

void handleCommandV() {
  Serial.print("[");
  Serial.print(PROGNAME);
  Serial.print('.');
  Serial.print(PROGVERS);

  Serial.print("] ");
}

void bmpDataCallback(float temperature, int pressure) {
  sendMessage((String)SensorDataHeader("BMP180", BMP_ID) + SensorDataValue(Temperature, temperature) + SensorDataValue(Pressure, pressure));
}

void sendMessage(String message) {
  Serial.println(message);  
  sendMultiCast(message);
}

// helper
void print_config() {
  String blank = F(" ");
  
  Serial.print(F("config:"));
}

void print_warning(byte type, String msg) {
  return;
  Serial.print(F("\nwarning: "));
  if (type == 1)
    Serial.print(F("skipped incomplete command "));
  if (type == 2)
    Serial.print(F("wrong parameter "));
  if (type == 3)
    Serial.print(F("failed: "));
  Serial.println(msg);
}
