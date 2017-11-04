//  BrewBrain, Thijs Nijveldt. Code used in motherships to handle measurements
//  Copyright (C) 2017  BrewBrain - Thijs Nijveldt
//  
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//  
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//  
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.


// https://arduino-esp8266.readthedocs.io/en/latest/

// TODO: add check for decoupling of sensors, bubble sensor can be plugged out for instance
// TODO: set pin on nodemcu high/low for debug in serial monitor (brewbrain only)

// Needed for communication with the ESP chip
#include <ESP8266WiFi.h> //https://github.com/esp8266/Arduino

//needed for library
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager

// include EEPROM library to store thumbprint in EEPROM
#include <EEPROM.h>
// use certain offset in EEPROM
// I think the EEPROM is also used to store network credentials
// store thumbprint at a safe distance from the network credentials
// chosen value can account for upto 1024 characters of stored network credentials
// is assumed to be enough... TODO: validate assumption
const uint16_t EEPROM_SIZE = 2048;
const uint16_t EEPROM_THUMBPRINT_ADDRESS = 1024;
// it is assumed ssl thumbprints are always 40 chars in length: TODO: validate
const uint8_t THUMBPRINT_ADDRESS_LENGTH = 40;

// Needed for the I2C temp/RH sensor in the motherhsip
#include <Wire.h>
#include <SI7021.h> // https://github.com/LowPowerLab/SI7021

// Needed for temperature sensor that is placed in the brew
#include <OneWire.h> // https://github.com/PaulStoffregen/OneWire
#include <DallasTemperature.h> // https://github.com/milesburton/Arduino-Temperature-Control-Library

/****************************************************************
 ************************* CONSTANTS ****************************
 ****************************************************************/
// Define SDA,SCL for TWI to temp/RH sensor in mothership
#define SDA D1 // GPIO5 on NodeMCU
#define SCL D2 // GPIO4 on NodeMCU

const uint16_t SEND_DATA_INTERVAL = 60000; // parameters for data transmitting [ms]
const uint8_t MINIMUM_TIME_BETWEEN_SENDS = 1; // seconds
const uint8_t ONE_WIRE_BUS = D6; // GPIO12
const char UUID[51] = "";
const char POST_STRING[] = "{\"data\": [{\"sensor_id\": 1, \"value\": %s}, {\"sensor_id\": 2, \"value\": %s}, {\"sensor_id\": 3, \"value\": %s}, {\"sensor_id\": 4, \"value\": %s}]}";
const char REGISTER_ACTIVITY_STRING[] = "{\"local_ip_address\": \"%d.%d.%d.%d\", \"chip_id\": %d, \"core_version\": \"%s\", \"sketch_size\": %d, \"sketch_md5\": \"%s\"}"; // post string of motherhsip
const char CREATE_COMMENT_STRING[] = "{\"comment\": \"%s\", \"status_text\": \"%s\"}"; // post string of motherhsip

const char HOST[] = "api.brewbrain.nl";

// the SSL thumbprint is retreived from the brewbrain server
char SSL_CERT_THUMBPRINT[THUMBPRINT_ADDRESS_LENGTH];

const char MEASUREMENT_URI[] = "/v1/Measurement";
const char ACTIVITY_URI[] = "/v1/APIKeyActivity";
const char COMMENT_URI[] = "/v1/APIKeyComment";

const uint16_t LED_OFF_TIME = 1000; // [ms]

uint8_t bubble_sensor_address[8];

// TODO: use SI7021 library struct instead of this self-created one...
struct si7021_readings {
  float temperature;
  float humidity;
};

/****************************************************************
 ************************* VARIABLES ****************************
 ****************************************************************/
SI7021 si7021; // instantiate temperature sensor on mothership
OneWire oneWire(ONE_WIRE_BUS); // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
DallasTemperature ds18b20(&oneWire); // Pass our oneWire reference to Dallas Temperature.
HTTPClient http;
uint32_t number_of_bubbles = 0;
uint32_t last_data_send = 0; // define parameters for data transmitting
char buffer[400];
char str_data_bubble[8];
char str_data_temp[8];
char str_data_humi[8];
char str_data_temp_brew[8];
unsigned long LEDturnedOff = 0;

// TODO: create struct for sensor status
boolean si7021_connected;
boolean ds18b20_connected;
boolean bubble_sensor_connected;

ESP8266WebServer server(80);

void blink_LED(uint8_t times)
{
  return blink_LED(times, 500);
}
void blink_LED(uint8_t times, uint16_t wait)
{
  uint8_t i = 0;

  for (i = 0; i < times; i++)
  {
    digitalWrite(LED_BUILTIN, LOW); // Turn the LED on
    delay(wait);
    digitalWrite(LED_BUILTIN, HIGH); // Turn the LED off
    delay(wait);
  }
}

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);

  // set pinmode for internal led for feedback purposes
  pinMode(LED_BUILTIN, OUTPUT);

  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // reset settings - for testing
  //  wifiManager.resetSettings();

  // disable debug output
  wifiManager.setDebugOutput(false);

  // fetches ssid and pass from eeprom and tries to connect
  // if it does not connect it starts an access point with the specified name
  // and goes into a blocking loop awaiting configuration
  Serial.println(); // prepend with empty line to get rid of board gibberish
  Serial.println("Trying to connected to WiFi, LED will blink 4 times in 2 seconds.");
  Serial.println("If it doesn't blink 4 times in one second, no WiFI was found.");
  // blink 4 times
  blink_LED(4);
  wifiManager.autoConnect("BrewBrainAP");

  //if you get here you have connected to the WiFi
  Serial.println("Succesfully connected to WiFi, LED will bilnk 4 times in 1 second.");
  // blink 4 times, fast after connect
  blink_LED(4, 250);

  EEPROM.begin(EEPROM_SIZE);
  read_thumbprint_from_EEPROM();

  // turn LED on by default
  digitalWrite(LED_BUILTIN, LOW);   // Turn the LED on

  // start webserver to receive http requests on local wifi
  server.on("/", render_information_page);
  server.onNotFound(handle_not_found);
  server.begin();
  Serial.print("Web server started.");
  Serial.print(" Reachable at: ");
  Serial.println(WiFi.localIP());

  // initialize si7021 temperature sensor
  if (si7021.begin(SDA, SCL))
  {
    si7021_connected = true;
  }
  else
  {
    si7021_connected = false;
    String message = "No temperature and humidity sensor detected.";
    create_comment(message, "BB002 SI7021 not found");
    Serial.println(message);
  }

  // Start up the ds18b20 library
  ds18b20.begin();
  // set to true by default, will be switched when reading sensor
  ds18b20_connected = true;

  setBubbleSensorAddress(bubble_sensor_address);

  Serial.println("Contacting BrewBrain website to register mothership.");
  registerAcitivty();

  Serial.println("Device ready for measurement!");
}

void registerAcitivty()
{
  IPAddress ip = WiFi.localIP();

  sprintf(buffer, REGISTER_ACTIVITY_STRING, ip[0], ip[1], ip[2], ip[3], ESP.getChipId(), ESP.getCoreVersion().c_str(), ESP.getSketchSize(), ESP.getSketchMD5().c_str());

  http_post_request(ACTIVITY_URI, buffer);
}

void create_comment(String comment, String status_code)
{
  sprintf(buffer, CREATE_COMMENT_STRING, comment.c_str(), status_code.c_str());

  Serial.print("Creating comment: '");
  Serial.print(comment);
  Serial.println("'");
  http_post_request(COMMENT_URI, buffer);
}

void loop()
{
  server.handleClient();

  if (millis() > (last_data_send + SEND_DATA_INTERVAL))
  {
    int temperature;
    int humidity;

    if (si7021_connected)
    {
      temperature = si7021.getCelsiusHundredths();
      humidity = si7021.getHumidityBasisPoints();
    }

    double brew_temp = get_ds18b20_reading();

    number_of_bubbles = getBubbleCount();

    if (bubble_sensor_connected)
    {
      dtostrf(number_of_bubbles / (SEND_DATA_INTERVAL / 1000.0), 5, 2 , str_data_bubble);
    }
    else
    {
      strcpy(str_data_bubble, "null");
    }
    if (si7021_connected)
    {
      dtostrf(temperature / 100.0, 5, 2 , str_data_temp);
      dtostrf(humidity / 100.0, 5, 2 , str_data_humi);
    }
    else
    {
      strcpy(str_data_temp, "null");
      strcpy(str_data_humi, "null");
    }
    if (ds18b20_connected)
    {
      dtostrf(brew_temp / 1.0, 5, 2 , str_data_temp_brew);
    }
    else
    {
      strcpy(str_data_temp_brew, "null");
    }

    sprintf(buffer, POST_STRING, str_data_temp, str_data_humi, str_data_temp_brew, str_data_bubble);

    http_post_request(MEASUREMENT_URI, buffer);

    LEDturnedOff = millis();
    digitalWrite(LED_BUILTIN, HIGH);  // Turn the LED off
  }

  if (millis() > (LEDturnedOff + LED_OFF_TIME))
  {
    digitalWrite(LED_BUILTIN, LOW);   // Turn the LED on
  }
}

struct si7021_readings get_si7021_readings()
{
  float temp;
  float humi;

  // also check when reading, else program freezes
  if (si7021_connected)
  {
    temp = si7021.getCelsiusHundredths() / 100.0;
    humi = si7021.getHumidityBasisPoints() / 100.0;
  }

  return {temp, humi};
}

double get_ds18b20_reading()
{
  ds18b20.requestTemperatures();
  double temp = ds18b20.getTempCByIndex(0);

  if (temp != DEVICE_DISCONNECTED_C)
  {
    ds18b20_connected = true;
    return temp;
  }
  else
  {
    ds18b20_connected = false;
  }
}

uint16_t getBubbleCount()
{
  // return 0 if no bubble sensor is selected
  // if this is not done, the mothership crashes...
  if (bubble_sensor_connected)
  {
    return 0;
  }

  oneWire.reset();
  oneWire.select(bubble_sensor_address);
  oneWire.write(0x44, 1);

  //  delay(1000);     // maybe 750ms is enough, maybe not
  // we might do a oneWire.depower() here, but the reset will take care of it.

  byte present = oneWire.reset();
  oneWire.select(bubble_sensor_address);
  oneWire.write(0xBE); // Read Scratchpad

  byte i;
  byte data[12];

  //  oneWire.read_bytes(data, 6);

  for ( i = 0; i < 6; i++)
  {
    data[i] = oneWire.read();
    delayMicroseconds(100);
  }

  int16_t bubble_count = (data[2] << 8) | data[1];

  return bubble_count;
}

void setBubbleSensorAddress(uint8_t pbubble_sensor_address[])
{
  uint8_t address[8];

  if (oneWire.search(address))
  {
    do
    {
      if (address[0] == 0xE2)
      {
        for (char i = 0; i < 8; i++)
        {
          pbubble_sensor_address[i] = address[i];
        }

        Serial.print("Bubble sensor detected at address: ");
        for (char i = 0; i < 8; i++)
        {
          Serial.print("0x");
          Serial.print(address[i], HEX);
          if (i < 7) Serial.print(",");
        }
        Serial.println();
        bubble_sensor_connected = true;
        return;
      }
    } while (oneWire.search(address));
  }

  bubble_sensor_connected = false;

  String message = "No bubble sensor detected, power down mothership to connect one.";
  create_comment(message, "BB001 Bubble sensor not found");
  Serial.println(message);
}

void http_post_request(String URI, String message)
{
  while (millis() < (last_data_send + MINIMUM_TIME_BETWEEN_SENDS * 1000));

  //  Serial.print(millis());
  //  Serial.print(" - Sending data: ");
  //  Serial.println(buffer);
  //  Serial.print("To: ");
  //  Serial.println(HOST + URI);

  http.begin(HOST, 443, URI, SSL_CERT_THUMBPRINT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + String(UUID));

  int http_client_send_request_return = http.POST(message);

  if (http_client_send_request_return == HTTPC_ERROR_CONNECTION_REFUSED)
  {
    http.end();
    handle_outdated_certificate_thumbprint();
    return;
  }

  last_data_send = millis();

  Serial.print(millis());
  Serial.print(" - API response: ");
  // TODO: handle timeout, print something like: "server HOST+URI not reachable..."
  http.writeToStream(&Serial);
  http.end();

  // add this to create line break
  Serial.println();
  Serial.println("------------------------------------------");
}

// prepare a web page to be send to a client (web browser)
void render_information_page()
{
  struct si7021_readings si7021_reading = get_si7021_readings();
  double brew_temp = get_ds18b20_reading();

  float temp = si7021_reading.temperature;
  float humi = si7021_reading.humidity;

  String temperature_part;
  String brew_temperature_part;
  String bubble_sensor_part;

  if (si7021_connected)
  {
    temperature_part = "Temperature: "
                       + String(temp)
                       + " &deg;C"
                       + "<br />"
                       + "Humidity: " + String(humi) + " %RH"
                       + "<br />";
  }
  else
  {
    temperature_part = "<i>Temperature and humidity sensor offline.</i><br />";
  }

  if (ds18b20_connected)
  {
    brew_temperature_part = "<b>Brew Temperature: "
                            + String(brew_temp)
                            + " &deg;C</b>"
                            + "<br />";
  }
  else
  {
    brew_temperature_part = "<b><i>Brew temperature sensor is offline.</i></b><br />";
  }

  if (bubble_sensor_connected)
  {
    bubble_sensor_part = "Yeast activity: "
                         + String(number_of_bubbles / (SEND_DATA_INTERVAL / 1000.0))
                         + " [-]";
  }
  else
  {
    bubble_sensor_part = "<i>Bubble sensor is offline.</i><br />";
  }

  String htmlPage =
    String("<!DOCTYPE HTML>") +
    "<html>" +
    "<head>" +
    "<title>Mothership Page</title>" +
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">" +
    "<meta http-equiv=\"refresh\" content=\"5\">" +
    "</head><body>" +
    "<div style='width: 100%; font-family: \"Avant Garde\", \"Century Gothic\", Arial, sans-serif'>" +
    "<h1>Mothership measurements</h1>" +
    "<i>Auto-refreshes every 5 seconds.</i>" +
    "<br />" +
    "Mothership uptime: " + String(millis() / 1000) + " s" +
    "<br />" +
    temperature_part +
    brew_temperature_part +
    bubble_sensor_part +
    "</div>" +
    "</body>" +
    "</html>" +
    "\r\n";

  server.send(200, "text/html", htmlPage);
}

void handle_not_found()
{
  String htmlPage =
    String("<!DOCTYPE HTML>") +
    "<html>" +
    "<head>" +
    "<title>Mothership Page Not Found</title>" +
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">" +
    "<meta http-equiv=\"refresh\" content=\"5\">" +
    "</head><body>" +
    "<div style='width: 100%; font-family: \"Avant Garde\", \"Century Gothic\", Arial, sans-serif'>" +
    "<h1 style='color: red'>Page Not Found</h1>" +
    "</div>" +
    "</body>" +
    "</html>" +
    "\r\n";

  server.send(404, "text/html", htmlPage);
}

void handle_outdated_certificate_thumbprint()
{
  Serial.println("Certificate thumbprint outdated!");

  String URL = String("http://") + HOST + String("/v1/SSL/getCertificateThumbprint");

  Serial.print("Getting thumbprint from: ");
  Serial.println(URL);

  http.begin(URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  http.GET();

  Serial.print("API response: ");

  String new_payload = http.getString();

  // TODO: handle timeout, print something like: "server HOST+URI not reachable..."
  http.writeToStream(&Serial);
  http.end();

  Serial.print("New thumbprint: '");
  Serial.print(new_payload);
  Serial.println("'");

  if (new_payload.length() != 40)
  {
    Serial.println("Thumbprint received incorrect length...");
    Serial.println(new_payload.length());
    return;
  }

  // load thumbprint to EEPROM
  write_thumbprint_to_EEPROM((char *) new_payload.c_str());
}

void read_thumbprint_from_EEPROM()
{
  Serial.print("Reading thumbprint from EEPROM!");
  for (int i = EEPROM_THUMBPRINT_ADDRESS; i < EEPROM_THUMBPRINT_ADDRESS + THUMBPRINT_ADDRESS_LENGTH; i++)
  {
    //    Serial.print(i);
    //    Serial.print(": ");
    //    Serial.print((char) EEPROM.read(i));
    //    Serial.print(" (");
    //    Serial.print((int) EEPROM.read(i));
    //    Serial.println(")");

    SSL_CERT_THUMBPRINT[i - EEPROM_THUMBPRINT_ADDRESS] = (char) EEPROM.read(i);
  }
  //  Serial.print("Read print: '");
  //  Serial.print(SSL_CERT_THUMBPRINT);
  //  Serial.println("'");
  Serial.println(" Succes!");
}

void write_thumbprint_to_EEPROM(char * new_thumbprint)
{
  Serial.println("Writing new thumbprint to EEPROM");
  for (int i = EEPROM_THUMBPRINT_ADDRESS; i < EEPROM_THUMBPRINT_ADDRESS + THUMBPRINT_ADDRESS_LENGTH; i++)
  {
    //    Serial.print(i);
    //    Serial.print(": ");
    //    Serial.print(new_thumbprint[i - EEPROM_THUMBPRINT_ADDRESS]);
    //    Serial.print(" (");
    //    Serial.print(int(new_thumbprint[i - EEPROM_THUMBPRINT_ADDRESS]));
    //    Serial.println(")");
    EEPROM.write(i, int(new_thumbprint[i - EEPROM_THUMBPRINT_ADDRESS]));
  }

  // commit new values to EEPROM:
  EEPROM.commit();

  read_thumbprint_from_EEPROM();
}
