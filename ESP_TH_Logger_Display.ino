/*---------------------------------------------------
HTTP 1.1 Temperature & Humidity Webserver for ESP8266 
for ESP8266 adapted Arduino IDE

Original by Stefan Thesen 05/2015 - free for anyone
Modded and extended by Felix Schuchmann 08/2017

Connect DHT21 / AMS2301 at D4 and Display at D1, D2
---------------------------------------------------*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "time_ntp.h"
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <ArduinoJson.h>

// Include the correct display library
// For a connection via I2C using Wire include
#include <Wire.h>  // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306.h" // alias for `#include "SSD1306Wire.h"`

SSD1306  display(0x3c, D1, D2);

// WiFi connection
const char* ssid = "...";
const char* password = "...";
const String apikey = "...";

// ntp timestamp
unsigned long ulSecs2000_timer=0;

// storage for Measurements; keep some mem free; allocate remainder
#define KEEP_MEM_FREE 10240
#define MEAS_SPAN_H 168         //168 = 1 week
unsigned long ulMeasCount=0;    // values already measured
unsigned long ulNoMeasValues=0; // size of array
unsigned long ulMeasDelta_ms;   // distance to next meas time
unsigned long ulNextMeas_ms;    // next meas time
unsigned long *pulTime;         // array for time points of measurements
float *pfTemp,*pfHum;           // array for temperature and humidity measurements

unsigned long ulReqcount;       // how often has a valid page been requested
unsigned long ulReconncount;    // how often did we connect to WiFi

// Create an instance of the server on Port 80
WiFiServer server(80);

//////////////////////////////
// DHT21 / AMS2301 is at D4
//////////////////////////////
#define DHTPIN D4

// Uncomment whatever type you're using!
//#define DHTTYPE DHT11   // DHT 11 
//#define DHTTYPE DHT22   // DHT 22  (AM2302)
#define DHTTYPE DHT21   // DHT 21 (AM2301)

// init DHT; 3rd parameter = 16 works for ESP8266@80MHz
DHT_Unified dht(DHTPIN, DHTTYPE, 16); 

// needed to avoid link error on ram check
extern "C" 
{
#include "user_interface.h"
}

/////////////////////
// the setup routine
/////////////////////
void setup() 
{
  // setup globals
  ulReqcount=0; 
  ulReconncount=0;

  // Initialising the UI will init the display too.
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.drawStringMaxWidth(0, 0, 128, "WLAN Temperatur und Feuchtigkeitslogger" );
  display.display();

  dht.begin();

  // start serial
  Serial.begin(115200);
  Serial.println("WLAN Temperatur und Feuchtigkeitslogger - S. Thesen 05/2015");
  
  // inital connect
  WiFi.mode(WIFI_STA);
  WiFi.hostname("temphum");
  WiFiStart();
  
  // allocate ram for data storage
  uint32_t free=system_get_free_heap_size() - KEEP_MEM_FREE;
  ulNoMeasValues = free / (sizeof(float)*2+sizeof(unsigned long));  // humidity & temp --> 2 + time
  pulTime = new unsigned long[ulNoMeasValues];
  pfTemp = new float[ulNoMeasValues];
  pfHum = new float[ulNoMeasValues];
  
  if (pulTime==NULL || pfTemp==NULL || pfHum==NULL)
  {
    ulNoMeasValues=0;
    Serial.println("Error in memory allocation!");
  }
  else
  {
    Serial.print("Allocated storage for ");
    Serial.print(ulNoMeasValues);
    Serial.println(" data points.");
    
    float fMeasDelta_sec = MEAS_SPAN_H*3600./ulNoMeasValues;
    ulMeasDelta_ms = ( (unsigned long)(fMeasDelta_sec+0.5) ) * 1000;  // round to full sec
    Serial.print("Measurements will happen each ");
    Serial.print(ulMeasDelta_ms);
    Serial.println(" ms.");
    
    ulNextMeas_ms = millis()+ulMeasDelta_ms;
  }
}


///////////////////
// (re-)start WiFi
///////////////////
void WiFiStart()
{
  ulReconncount++;
  
  // Connect to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  display.clear();
  display.drawString(0, 0, "Connecting to " + String(ssid) );
  display.display();
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  display.drawString(0, 12, "WiFi connected" );
  display.display();
  
  // Start the server
  server.begin();
  Serial.println("Server started");
  display.drawString(0, 24, "Server started" );

  // Print the IP address
  Serial.println(WiFi.localIP());
  
  display.drawString(0, 36, WiFi.localIP().toString() );
  display.display();
  
  ///////////////////////////////
  // connect to NTP and get time
  ///////////////////////////////
  ulSecs2000_timer=getNTPTimestamp();
  Serial.print("Current Time UTC from NTP server: " );
  Serial.println(epoch_to_string(ulSecs2000_timer).c_str());

  ulSecs2000_timer -= millis()/1000;  // keep distance to millis() counter
}


/////////////////////////////////////
// make html table for measured data
/////////////////////////////////////
unsigned long MakeTable (WiFiClient *pclient, bool bStream)
{
  unsigned long ulLength=0;
  
  // here we build a big table.
  // we cannot store this in a string as this will blow the memory   
  // thus we count first to get the number of bytes and later on 
  // we stream this out
  if (ulMeasCount==0) 
  {
    String sTable = "Noch keine Daten verf&uuml;gbar.<BR>";
    if (bStream)
    {
      pclient->print(sTable);
    }
    ulLength+=sTable.length();
  }
  else
  { 
    unsigned long ulEnd;
    if (ulMeasCount>ulNoMeasValues)
    {
      ulEnd=ulMeasCount-ulNoMeasValues;
    }
    else
    {
      ulEnd=0;
    }
    
    String sTable;
    sTable = "<table style=\"width:100%\"><tr><th>Zeit / UTC</th><th>T &deg;C</th><th>Hum &#037;</th></tr>";
    sTable += "<style>table, th, td {border: 2px solid black; border-collapse: collapse;} th, td {padding: 5px;} th {text-align: left;}</style>";
    for (unsigned long li=ulMeasCount;li>ulEnd;li--)
    {
      unsigned long ulIndex=(li-1)%ulNoMeasValues;
      sTable += "<tr><td>";
      sTable += epoch_to_string(pulTime[ulIndex]).c_str();
      sTable += "</td><td>";
      sTable += pfTemp[ulIndex];
      sTable += "</td><td>";
      sTable += pfHum[ulIndex];
      sTable += "</td></tr>";

      // play out in chunks of 1k
      if(sTable.length()>1024)
      {
        if(bStream)
        {
          pclient->print(sTable);
          //pclient->write(sTable.c_str(),sTable.length());
        }
        ulLength+=sTable.length();
        sTable="";
      }
    }
    
    // remaining chunk
    sTable+="</table>";
    ulLength+=sTable.length();
    if(bStream)
    {
      pclient->print(sTable);
      //pclient->write(sTable.c_str(),sTable.length());
    }   
  }
  
  return(ulLength);
}
  

////////////////////////////////////////////////////
// make google chart object table for measured data
////////////////////////////////////////////////////
unsigned long MakeList (WiFiClient *pclient, bool bStream)
{
  unsigned long ulLength=0;
  
  // here we build a big list.
  // we cannot store this in a string as this will blow the memory   
  // thus we count first to get the number of bytes and later on 
  // we stream this out
  if (ulMeasCount>0) 
  { 
    unsigned long ulBegin;
    if (ulMeasCount>ulNoMeasValues)
    {
      ulBegin=ulMeasCount-ulNoMeasValues;
    }
    else
    {
      ulBegin=0;
    }
    
    String sTable="";
    for (unsigned long li=ulBegin;li<ulMeasCount;li++)
    {
      // result shall be ['18:24:08 - 21.5.2015',21.10,49.00],
      unsigned long ulIndex=li%ulNoMeasValues;
      sTable += "['";
      sTable += epoch_to_string(pulTime[ulIndex]).c_str();
      sTable += "',";
      sTable += pfTemp[ulIndex];
      sTable += ",";
      sTable += pfHum[ulIndex];
      sTable += "],\n";

      // play out in chunks of 1k
      if(sTable.length()>1024)
      {
        if(bStream)
        {
          pclient->print(sTable);
          //pclient->write(sTable.c_str(),sTable.length());
        }
        ulLength+=sTable.length();
        sTable="";
      }
    }
    
    // remaining chunk
    if(bStream)
    {
      pclient->print(sTable);
      //pclient->write(sTable.c_str(),sTable.length());
    } 
    ulLength+=sTable.length();  
  }
  
  return(ulLength);
}


//////////////////////////
// create HTTP 1.1 header
//////////////////////////
String MakeHTTPHeader(unsigned long ulLength)
{
  String sHeader;
  
  sHeader  = F("HTTP/1.1 200 OK\r\nContent-Length: ");
  sHeader += ulLength;
  sHeader += F("\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
  
  return(sHeader);
}


////////////////////
// make html footer
////////////////////
String MakeHTTPFooter()
{
  String sResponse;
  
  sResponse  = F("<FONT SIZE=-2><BR>Aufrufz&auml;hler="); 
  sResponse += ulReqcount;
  sResponse += F(" - Verbindungsz&auml;hler="); 
  sResponse += ulReconncount;
  sResponse += F(" - Freies RAM=");
  sResponse += (uint32_t)system_get_free_heap_size();
  sResponse += F(" - Max. Datenpunkte=");
  sResponse += ulNoMeasValues;
  sResponse += F("<BR>Stefan Thesen 05/2015<BR></body></html>");
  
  return(sResponse);
}

/////////////
// main loop
/////////////
void loop() 
{
  //////////////////////////////
  // check if WLAN is connected
  //////////////////////////////
  if (WiFi.status() != WL_CONNECTED)
  {
    WiFiStart();
  }

  ///////////////////
  // do data logging
  ///////////////////
  if (ulMeasCount==0 || millis()>=ulNextMeas_ms) 
  {
    ulNextMeas_ms = millis()+ulMeasDelta_ms;
    int i = ulMeasCount%ulNoMeasValues;

    // Get temperature event and print its value.
    sensors_event_t event;  
    dht.temperature().getEvent(&event);
    if (isnan(event.temperature)) {
      Serial.println("Error reading temperature!");
    }
    else {
      pfTemp[i] = event.temperature;
    }
    // Get humidity event and print its value.
    dht.humidity().getEvent(&event);
    if (isnan(event.relative_humidity)) {
      Serial.println("Error reading humidity!");
    }
    else {
      pfHum[i] = event.relative_humidity;
    }

    pulTime[i] = millis()/1000+ulSecs2000_timer;
    
    Serial.print("Logging Temperature: "); 
    Serial.print(pfTemp[i]);
    Serial.print(" deg Celsius - Humidity: "); 
    Serial.print(pfHum[i]);
    Serial.print("% - Time: ");
    Serial.println(pulTime[i]);

    display.clear();
    
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER); // The coordinates define the center of the text
    display.drawString(64, 0, epoch_to_string( pulTime[i] + 3600 * 2 ).c_str());

    display.setFont(ArialMT_Plain_24);
    display.setTextAlignment(TEXT_ALIGN_LEFT); // The coordinates define the left starting point of the text
    display.drawString(0, 10, String(pfTemp[i], 0) + "°C");
    
    display.setTextAlignment(TEXT_ALIGN_RIGHT); // The coordinates define the right end of the text
    display.drawString(128, 10, String(pfHum[i], 0) + "%");
    
    // write the buffer to the display
    display.display();
    
    ulMeasCount++;

    //////////////////////////////
    // Request Weather API
    //////////////////////////////
    
    // Use WiFiClient class to create TCP connections
    char* host = "api.openweathermap.org";
    WiFiClient apiclient;
    const int httpPort = 80;
    if (!apiclient.connect(host, httpPort)) {
      Serial.println("connection failed");
      return;
    }
    
    // We now create a URI for the request
    String url = "/data/2.5/weather?zip=55126,de&units=metric&APPID=" + apikey;
    Serial.print("Requesting URL: ");
    Serial.println(url);
    
    // This will send the request to the server
    apiclient.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: " + host + "\r\n" + 
                 "Connection: close\r\n\r\n");
    unsigned long timeout = millis();
    while (apiclient.available() == 0) {
      if (millis() - timeout > 5000) {
        Serial.println(">>> Client Timeout !");
        apiclient.stop();
        return;
      }
    }
    
    // Read all the lines of the reply from server and print them to Serial
    String json = "";
    boolean httpBody = false;
    while (apiclient.available()) {
      String line = apiclient.readStringUntil('\r');
      if (!httpBody && line.charAt(1) == '{') {
        httpBody = true;
      }
      if (httpBody) {
        json += line;
      }
    }

    //https://bblanchon.github.io/ArduinoJson/assistant/
    const size_t BUFFER_SIZE =
      JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(1) + 2*JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(5) + JSON_OBJECT_SIZE(6) + JSON_OBJECT_SIZE(12);
  
    DynamicJsonBuffer jsonBuffer(BUFFER_SIZE);
    Serial.println("Got data:");
    Serial.println(json);
    JsonObject& root = jsonBuffer.parseObject(json);
    if (!root.success()) {
      Serial.println("JSON parsing failed!");
    } else {
      JsonObject& main = root["main"];
      float main_temp = main["temp"];
      //int main_pressure = main["pressure"];
      int main_humidity = main["humidity"];
      //float main_temp_min = main["temp_min"];
      //float main_temp_max = main["temp_max"];

      Serial.println("temp: " + String(main_temp));
      Serial.println("humidity: " + String(main_humidity));

      // Außen-Luftfeuchtigkeit - 3 * (Innentemperatur - Außentemperatur)
      float new_innen_hum = main_humidity - 3 * (pfTemp[i] - main_temp);   

      display.setFont(ArialMT_Plain_16);
      display.setTextAlignment(TEXT_ALIGN_LEFT); // The coordinates define the left starting point of the text
      display.drawString(0, 34, String(main_temp, 0) + "°C");
      
      display.setTextAlignment(TEXT_ALIGN_RIGHT); // The coordinates define the right end of the text
      display.drawString(128, 34, String(main_humidity) + "%");
   
      display.setTextAlignment(TEXT_ALIGN_CENTER); // The coordinates define the right end of the text
      if (pfHum[i] > new_innen_hum) {
        display.drawString(64, 34, "AUF");
      } else {
        display.drawString(64, 34, "ZU");
      }

      display.setTextAlignment(TEXT_ALIGN_RIGHT); // The coordinates define the right end of the text
      display.setFont(ArialMT_Plain_10);
      display.drawString(128, 52, "erwartet nach Lüften: " + String(new_innen_hum, 0) + "%");

      display.display();
    }
    Serial.println();
    Serial.println("closing connection");
    apiclient.stop();
  }
  
  
  ///////////////////////////////////
  // Check if a client has connected
  ///////////////////////////////////
  WiFiClient client = server.available();
  if (!client) 
  {
    return;
  }
  
  // Wait until the client sends some data
  Serial.println("new client");
  unsigned long ultimeout = millis()+250;
  while(!client.available() && (millis()<ultimeout) )
  {
    delay(1);
  }
  if(millis()>ultimeout) 
  { 
    Serial.println("client connection time-out!");
    return; 
  }
  
  /////////////////////////////////////
  // Read the first line of the request
  /////////////////////////////////////
  String sRequest = client.readStringUntil('\r');
  //Serial.println(sRequest);
  client.flush();
  
  // stop client, if request is empty
  if(sRequest=="")
  {
    Serial.println("empty request! - stopping client");
    client.stop();
    return;
  }
  
  // get path; end of path is either space or ?
  // Syntax is e.g. GET /?show=1234 HTTP/1.1
  String sPath="",sParam="", sCmd="";
  String sGetstart="GET ";
  int iStart,iEndSpace,iEndQuest;
  iStart = sRequest.indexOf(sGetstart);
  if (iStart>=0)
  {
    iStart+=+sGetstart.length();
    iEndSpace = sRequest.indexOf(" ",iStart);
    iEndQuest = sRequest.indexOf("?",iStart);
    
    // are there parameters?
    if(iEndSpace>0)
    {
      if(iEndQuest>0)
      {
        // there are parameters
        sPath  = sRequest.substring(iStart,iEndQuest);
        sParam = sRequest.substring(iEndQuest,iEndSpace);
      }
      else
      {
        // NO parameters
        sPath  = sRequest.substring(iStart,iEndSpace);
      }
    }
  }
    
  
  ///////////////////////////
  // format the html response
  ///////////////////////////
  String sResponse,sResponse2,sHeader;
  
  /////////////////////////////
  // format the html page for /
  /////////////////////////////
  if(sPath=="/") 
  {
    ulReqcount++;
    int iIndex= (ulMeasCount-1)%ulNoMeasValues;
    sResponse  = F("<html>\n<head>\n<title>WLAN Logger f&uuml;r Lufttemperatur und Feuchtigkeit</title>\n<script type=\"text/javascript\" src=\"https://www.google.com/jsapi?autoload={'modules':[{'name':'visualization','version':'1','packages':['gauge']}]}\"></script>\n<script type=\"text/javascript\">\nvar temp=");
    sResponse += pfTemp[iIndex];
    sResponse += F(",hum=");
    sResponse += pfHum[iIndex];
    sResponse += F(";\ngoogle.load('visualization', '1', {packages: ['gauge']});google.setOnLoadCallback(drawgaugetemp);google.setOnLoadCallback(drawgaugehum);\nvar gaugetempOptions = {min: -20, max: 50, yellowFrom: -20, yellowTo: 0,redFrom: 30, redTo: 50, minorTicks: 10, majorTicks: ['-20','-10','0','10','20','30','40','50']};\n");
    sResponse += F("var gaugehumOptions = {min: 0, max: 100, yellowFrom: 0, yellowTo: 25, redFrom: 75, redTo: 100, minorTicks: 10, majorTicks: ['0','10','20','30','40','50','60','70','80','90','100']};\nvar gaugetemp,gaugehum;\n\nfunction drawgaugetemp() {\ngaugetempData = new google.visualization.DataTable();\n");
    sResponse += F("gaugetempData.addColumn('number', '\260C');\ngaugetempData.addRows(1);\ngaugetempData.setCell(0, 0, temp);\ngaugetemp = new google.visualization.Gauge(document.getElementById('gaugetemp_div'));\ngaugetemp.draw(gaugetempData, gaugetempOptions);\n}\n\n");
    sResponse += F("function drawgaugehum() {\ngaugehumData = new google.visualization.DataTable();\ngaugehumData.addColumn('number', '%');\ngaugehumData.addRows(1);\ngaugehumData.setCell(0, 0, hum);\ngaugehum = new google.visualization.Gauge(document.getElementById('gaugehum_div'));\ngaugehum.draw(gaugehumData, gaugehumOptions);\n}\n");
    sResponse += F("</script>\n</head>\n<body>\n<font color=\"#000000\"><body bgcolor=\"#d0d0f0\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\"><h1>WLAN Logger f&uuml;r Lufttemperatur und Feuchtigkeit</h1>AMS2301 / DHT21 an ESP8266 <BR><BR><FONT SIZE=+1>Letzte Messung um ");
    sResponse += epoch_to_string(pulTime[iIndex]).c_str();
    sResponse += F(" UTC<BR>\n<div id=\"gaugetemp_div\" style=\"float:left; width:160px; height: 160px;\"></div> \n<div id=\"gaugehum_div\" style=\"float:left; width:160px; height: 160px;\"></div>\n<div style=\"clear:both;\"></div>");
    
    sResponse2 = F("<p>Temperatur & Feuchtigkeitsverlauf - Seiten laden l&auml;nger:<BR><a href=\"/grafik\">Grafik</a>     <a href=\"/tabelle\">Tabelle</a></p>");
    sResponse2 += MakeHTTPFooter().c_str();
    
    // Send the response to the client 
    client.print(MakeHTTPHeader(sResponse.length()+sResponse2.length()).c_str());
    client.print(sResponse);
    client.print(sResponse2);
  }
  else if(sPath=="/tabelle")
  ////////////////////////////////////
  // format the html page for /tabelle
  ////////////////////////////////////
  {
    ulReqcount++;
    unsigned long ulSizeList = MakeTable(&client,false); // get size of table first
    
    sResponse  = F("<html><head><title>WLAN Logger f&uuml;r Lufttemperatur und Feuchtigkeit</title></head><body>");
    sResponse += F("<font color=\"#000000\"><body bgcolor=\"#d0d0f0\">");
    sResponse += F("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\">");
    sResponse += F("<h1>WLAN Logger f&uuml;r Lufttemperatur und Feuchtigkeit</h1>");
    sResponse += F("<FONT SIZE=+1>");
    sResponse += F("<a href=\"/\">Startseite</a><BR><BR>Letzte Messungen im Abstand von ");
    sResponse += ulMeasDelta_ms;
    sResponse += F("ms<BR>");
    // here the big table will follow later - but let us prepare the end first
      
    // part 2 of response - after the big table
    sResponse2 = MakeHTTPFooter().c_str();
    
    // Send the response to the client - delete strings after use to keep mem low
    client.print(MakeHTTPHeader(sResponse.length()+sResponse2.length()+ulSizeList).c_str()); 
    client.print(sResponse); sResponse="";
    MakeTable(&client,true);
    client.print(sResponse2);
  }
  else if(sPath=="/grafik")
  ///////////////////////////////////
  // format the html page for /grafik
  ///////////////////////////////////
  {
    ulReqcount++;
    unsigned long ulSizeList = MakeList(&client,false); // get size of list first

    sResponse  = F("<html>\n<head>\n<title>WLAN Logger f&uuml;r Lufttemperatur und Feuchtigkeit</title>\n<script type=\"text/javascript\" src=\"https://www.google.com/jsapi?autoload={'modules':[{'name':'visualization','version':'1','packages':['corechart']}]}\"></script>\n");
    sResponse += F("<script type=\"text/javascript\"> google.setOnLoadCallback(drawChart);\nfunction drawChart() {var data = google.visualization.arrayToDataTable([\n['Zeit / UTC', 'Temperatur', 'Feuchtigkeit'],\n");    
    // here the big list will follow later - but let us prepare the end first
      
    // part 2 of response - after the big list
    sResponse2  = F("]);\nvar options = {title: 'Verlauf',vAxes:{0:{viewWindowMode:'explicit',gridlines:{color:'black'},format:\"##.##\260C\"},1: {gridlines:{color:'transparent'},format:\"##,##%\"},},series:{0:{targetAxisIndex:0},1:{targetAxisIndex:1},},curveType:'none',legend:{ position: 'bottom'}};");
    sResponse2 += F("var chart = new google.visualization.LineChart(document.getElementById('curve_chart'));chart.draw(data, options);}\n</script>\n</head>\n");
    sResponse2 += F("<body>\n<font color=\"#000000\"><body bgcolor=\"#d0d0f0\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\"><h1>WLAN Logger f&uuml;r Lufttemperatur und Feuchtigkeit</h1><a href=\"/\">Startseite</a><BR>");
    sResponse2 += F("<BR>\n<div id=\"curve_chart\" style=\"width: 600px; height: 400px\"></div>");
    sResponse2 += MakeHTTPFooter().c_str();
    
    // Send the response to the client - delete strings after use to keep mem low
    client.print(MakeHTTPHeader(sResponse.length()+sResponse2.length()+ulSizeList).c_str()); 
    client.print(sResponse); sResponse="";
    MakeList(&client,true);
    client.print(sResponse2);
  }
  else
  ////////////////////////////
  // 404 for non-matching path
  ////////////////////////////
  {
    sResponse="<html><head><title>404 Not Found</title></head><body><h1>Not Found</h1><p>The requested URL was not found on this server.</p></body></html>";
    
    sHeader  = F("HTTP/1.1 404 Not found\r\nContent-Length: ");
    sHeader += sResponse.length();
    sHeader += F("\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
    
    // Send the response to the client
    client.print(sHeader);
    client.print(sResponse);
  }
  
  // and stop the client
  client.stop();
  Serial.println("Client disconnected");
}

