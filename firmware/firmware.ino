#include <ESP8266WiFi.h>
#include <FS.h>
#include <Hash.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP8266mDNS.h>
#include <SPIFFSEditor.h>
#include <SPI.h>
#include <LoRa.h>

#define HEADERSIZE 4 
#define BUFFERSIZE 252

byte mac[6];
char macaddr[14];
char ssid[32] = "disaster.radio ";
const char * hostName = "disaster-node";

IPAddress local_IP(192, 162, 4, 1);
IPAddress gateway(0, 0, 0, 0);
IPAddress netmask(255, 255, 255, 0);
const char * url = "chat.disaster.radio";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

int loraInitialized = 0; // has the LoRa radio been initialized?


// for portable node (wemos d1 mini) use these settings:
const int csPin = 15;          // LoRa radio chip select, GPIO15 = D8 on WeMos D1 mini
const int resetPin = 5;       // LoRa radio reset, GPIO0 = D3 
const int irqPin = 4;        // interrupt pin for receive callback?, GPIO2 = D4

// for solar-powered module use these settings:
/*
const int csPin = 2;          // LoRa radio chip select, GPIO2
const int resetPin = 5;       // LoRa radio reset (hooked to LED, unused)
const int irqPin = 16;        // interrupt pin for receive callback?, GPIO16
*/

//TODO: switch to volatile byte for interrupt

byte localAddress;     // assigned to last byte of mac address in setup
byte destination = 0xFF;      // destination to send to default broadcast

bool echo_on = false;

byte routes = 0;

struct routeTableEntry{
    uint8_t address[6];
    uint8_t destination[256][6];
    uint8_t routes;
    uint8_t metric;
};

struct routeTableEntry route[17];

/*
  FORWARD-DEFINED FUNCTIONS
*/

/*char* stringToArray(const char* input, int length){
    char output[length]; 
    for( int i = 0 ; i < length ; i++ ){
        output[i] = input[i]; 
    }
    return output;
}
*/

void sendMessage(uint8_t* outgoing, int outgoing_length) {
    LoRa.beginPacket();
    for( int i = 0 ; i < outgoing_length ; i++){
        LoRa.write(outgoing[i]);
    }
    LoRa.endPacket();
}

void storeMessage(uint8_t* message, int message_length) {
    //store full message in log file
    File log = SPIFFS.open("/log.txt", "a");
    if(!log){
        Serial.printf("file open failed");
    }
    for(int i = 0 ; i <= message_length ; i++){
        log.printf("%c", message[i]);
    }
    log.printf("\n");
    log.close();
}

//TODO convert string to char array
String dumpLog() {
    String dump = "";
    File log = SPIFFS.open("/log.txt", "r");
    if(!log){
        Serial.printf("file open failed");
    }  
    Serial.print("reading log file: \r\n");
    while (log.available()){
        //TODO replace string with char array
        String s = log.readStringUntil('\n');
        dump += s;
        dump += "\r\n";
    }
    return dump;
}

void clearLog() {
  File log = SPIFFS.open("/log.txt", "w");
  log.close();
}

void parseRoute(uint8_t *buf, int length) {
   
    int next_entry = 1;
    for( int i = 0; i < 6; i++){
        route[next_entry].address[i] = buf[4+i]; 
    }

    int routes = (length-10)/6;
    for( int i = 0; i < routes; i++){
        for( int j = 0; j < 6; j++){
            route[next_entry].destination[i][j] = buf[(6*i)+j+10]; 
        }
    }

}

/*
  CALLBACK FUNCTIONS
*/
void onReceive(int packetSize) {
    Serial.printf("GOT PACKET!\r\n");
    if (packetSize == 0) return;          // if there's no packet, return



    uint8_t incoming[BUFFERSIZE];                 // payload of packet

    int incomingLength = 0;
    while (LoRa.available()) { 
        incoming[incomingLength] = (char)LoRa.read(); 
        incomingLength++;
    }

    // read packet header bytes:
    uint8_t seq1 = incoming[0];   // open byte one 
    uint8_t seq2 = incoming[1];   // open byte two 
    uint8_t type = incoming[2];   // identifies message type 
    uint8_t pipe = incoming[3];   // breaks header for error check 

    
    if (pipe != '|') return;

    
    switch(type){
        case 'b': 
            parseRoute(incoming, incomingLength);
            break;
        case 'c': 
            //parseChat();
            break;
        case 'm': 
            //parseMap();
            break;

    }

    //TODO refresh on case statements

    /* TODO: fix error check once garbling solved
    if (incomingLength != i) {   // check length for error
      Serial.printf("error: message length does not match length\r\n");
      return;                             // skip rest of function
    }*/

    // if the recipient isn't this device or broadcast,
    /*if (recipient != localAddress && recipient != 0xFF) {
        Serial.printf("This message is not for me.\r\n");
        return;                             // skip rest of function
    }*/

    Serial.printf("RSSI: %f\r\n", LoRa.packetRssi());
    Serial.printf("Snr: %f\r\n", LoRa.packetSnr());

    for(int i = 0 ; i < incomingLength ; i++){
        Serial.printf("%c", incoming[i]);
    }
    Serial.printf("\r\n");

    storeMessage(incoming, incomingLength);
    
    ws.binaryAll(incoming, incomingLength);
   
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
    if(type == WS_EVT_CONNECT){
        Serial.printf("ws[%s][%u] connect\r\n", server->url(), client->id());
        client->ping();
    } else if(type == WS_EVT_DISCONNECT){
        Serial.printf("ws[%s][%u] disconnect: %u\r\n", server->url(), client->id());
    } else if(type == WS_EVT_ERROR){
        Serial.printf("ws[%s][%u] error(%u): %s\r\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
    } else if(type == WS_EVT_PONG){
        Serial.printf("ws[%s][%u] pong[%u]: %s\r\n", server->url(), client->id(), len, (len)?(char*)data:"");
    } else if(type == WS_EVT_DATA){

        AwsFrameInfo * info = (AwsFrameInfo*)arg;
        char msg_id[4];
        char usr_id[32];
        char msg[256];
        int msg_length;
        int usr_id_length = 0;
        int usr_id_stop = 0;

        if(info->final && info->index == 0 && info->len == len){
            //the whole message is in a single frame and we got all of it's data

            Serial.printf("ws[%s][%u] %s-message[%llu]: \r\n", server->url(), client->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);
            //cast data to char array
            for(size_t i=0; i < info->len; i++) {
                //TODO check if info length is bigger than allocated memory
                msg[i] = (char) data[i];
                msg_length = i; 
                    
                if(msg[i] == '$'){
                    echo_on = !echo_on;
                }

                // check for stop char of usr_id
                if(msg[i] == '>'){
                    usr_id_stop = i;  
                }
            }
            msg_length++;
            msg[msg_length] = '\0';

	    
            //parse message id 
            memcpy( msg_id, msg, 2 );
            msg_id[2] = '!';
            msg_id[3] = '\0';   

            //parse username
            for( int i = 5 ; i < usr_id_stop ; i++){
                usr_id[i-5] = msg[i];
            }
            usr_id_length = usr_id_stop - 5;

            //print message info to serial
            /*Serial.printf("Message Length: %d\r\n", msg_length);
            Serial.printf("Message ID: %02d%02d %c\r\n", msg_id[0], msg_id[1], msg_id[2]);
            Serial.printf("Message:");
            for( int i = 0 ; i <= msg_length ; i++){
                Serial.printf("%c", msg[i]);
            }
            Serial.printf("\r\n");
	    */
	    
	    //store full message in log file
	    File log = SPIFFS.open("/log.txt", "a");
	    if(!log){
	      Serial.printf("file open failed");
	    }
	    //TODO msg_id is hex bytes not chars? adapt storeMessage function
	    log.printf("%02d%02d", msg_id[0], msg_id[1]);
	    for(int i = 2 ; i <= msg_length ; i++){
	      log.printf("%c", msg[i]);
	    }
	    log.printf("\n");
	    log.close();

	    //TODO delay ack based on estimated transmit time
            //send ack to websocket
            ws.binary(client->id(), msg_id, 3);

//	    Serial.print(dumpLog());
	    
            //transmit message over LoRa
            /*if(loraInitialized) {
              sendMessage(msg, msg_length);
            }*/

            //echoing message to ws
            if(echo_on){
                char echo[256]; 
                char prepend[7] = "<echo>";
                int prepend_length= 6;
                memcpy(echo, msg, 4);
                for( int i = 0 ; i < prepend_length ; i++){
                    echo[4+i] = prepend[i];
                }
                for( int i = 0 ; i < msg_length-usr_id_stop ; i++){
                    echo[4+prepend_length+i] = msg[i+usr_id_stop+1];
                }
                int echo_length = prepend_length - usr_id_length + msg_length - 1;
                ws.binaryAll(echo, echo_length);
            }

            //set LoRa back into receive mode
            if(loraInitialized) {
              LoRa.receive();
            }           
        } 
        else {

            //TODO message is comprised of multiple frames or the frame is split into multiple packets

        }
    }
}


/*
  SETUP FUNCTIONS
*/
void wifiSetup(){
    WiFi.macAddress(mac);
    sprintf(macaddr, "%02x%02x%02x%02x%02x%02x", mac[5], mac[4], mac[3], mac[2], mac[1], mac [0]);
    strcat(ssid, macaddr);
    WiFi.hostname(hostName);
    WiFi.mode(WIFI_AP);
    //WiFi.softAPConfig(local_IP, gateway, netmask);
    WiFi.softAP(ssid);
}

void spiffsSetup(){
    if (SPIFFS.begin()) {
        Serial.print("ok\r\n");
        if (SPIFFS.exists("/index.html")) {
            Serial.printf("The file exists!\r\n");
            File f = SPIFFS.open("/index.html", "r");
            if (!f) {
                Serial.printf("Some thing went wrong trying to open the file...\r\n");
            }
            else {
                int s = f.size();
                Serial.printf("Size=%d\r\n", s);
                String data = f.readString();
                Serial.printf("%s\r\n", data.c_str());
                f.close();
            }
        }
        else {
            Serial.printf("No such file found.\r\n");
        }
    }
}

void mdnsSetup(){
    if(!MDNS.begin("disaster")){
        Serial.printf("Error setting up mDNS\r\n");
        while(1) {
            delay(1000);
        }
    }
    Serial.printf("mDNS responder started\r\n");
      
    MDNS.addService("http", "tcp", 80);
}

void webServerSetup(){
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    events.onConnect([](AsyncEventSourceClient *client){
        client->send("hello!",NULL,millis(),1000);
    });
    server.addHandler(&events);

    server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", String(ESP.getFreeHeap()));
    });

    //server.serveStatic("/dump", SPIFFS, "/log.txt");

    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest *request){
        Serial.printf("NOT_FOUND: ");
        if(request->method() == HTTP_GET)
        Serial.printf("GET");
        else if(request->method() == HTTP_POST)
        Serial.printf("POST");
        else if(request->method() == HTTP_DELETE)
        Serial.printf("DELETE");
        else if(request->method() == HTTP_PUT)
        Serial.printf("PUT");
        else if(request->method() == HTTP_PATCH)
        Serial.printf("PATCH");
        else if(request->method() == HTTP_HEAD)
        Serial.printf("HEAD");
        else if(request->method() == HTTP_OPTIONS)
        Serial.printf("OPTIONS");
        else
        Serial.printf("UNKNOWN");
        Serial.printf(" http://%s%s\r\n", request->host().c_str(), request->url().c_str());

        if(request->contentLength()){
            Serial.printf("_CONTENT_TYPE: %s\r\n", request->contentType().c_str());
            Serial.printf("_CONTENT_LENGTH: %u\r\n", request->contentLength());
        }

        int headers = request->headers();
        int i;
        for(i=0;i<headers;i++){
            AsyncWebHeader* h = request->getHeader(i);
            Serial.printf("_HEADER[%s]: %s\r\n", h->name().c_str(), h->value().c_str());
        }

        int params = request->params();
        for(i=0;i<params;i++){
            AsyncWebParameter* p = request->getParam(i);
            if(p->isFile()){
                Serial.printf("_FILE[%s]: %s, size: %u\r\n", p->name().c_str(), p->value().c_str(), p->size());
            } else if(p->isPost()){
                Serial.printf("_POST[%s]: %s\r\n", p->name().c_str(), p->value().c_str());
            } else {
                Serial.printf("_GET[%s]: %s\r\n", p->name().c_str(), p->value().c_str());
            }
        }

        request->send(404);
    });

    server.onFileUpload([](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index)
        Serial.printf("UploadStart: %s\r\n", filename.c_str());
        Serial.printf("%s", (const char*)data);
        if(final)
        Serial.printf("UploadEnd: %s (%u)\r\n", filename.c_str(), index+len);
    });

    server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if(!index)
        Serial.printf("BodyStart: %u\r\n", total);
        Serial.printf("%s", (const char*)data);
        if(index + len == total)
        Serial.printf("BodyEnd: %u\r\n", total);
    });

    server.begin();
}


void loraSetup(){
    localAddress = mac[0];

    // override the default CS, reset, and IRQ pins (optional)
    LoRa.setPins(csPin, resetPin, irqPin); // set CS, reset, IRQ pin

    if (!LoRa.begin(915E6)) {             // initialize ratio at 915 MHz
        Serial.printf("LoRa init failed. Check your connections.\r\n");
//        while (true);                       // if failed, do nothing
          return;
    }

    LoRa.setSPIFrequency(100E3);
    LoRa.setSpreadingFactor(9);           // ranges from 6-12,default 7 see API docs
    LoRa.onReceive(onReceive);
    LoRa.receive();

    loraInitialized = 1;

    Serial.printf("LoRa init succeeded.\r\n");
    Serial.printf("local address: %02x\r\n", localAddress);
    Serial.printf("%s\r\n", macaddr);
}


void broadcastRoute(uint8_t sequence, uint8_t entry){

    // initalize message with header
    uint8_t route_message[256] = { sequence, entry, 'b', '|' };
    int message_length = 4;

    //char route_message[256] = "01b|";
    
    Serial.printf("entry header");
    Serial.printf("\r\n");

    // append node's mac address
    for( int i = 5 ; i >= 0  ; i--){
        route_message[message_length] = mac[i];
        message_length++;
    }

    // append next hop mac address
    for( int i = 0 ; i < 6 ; i++){
        route_message[message_length] = route[entry].address[i];
        message_length++;
    }

    /*for( int i = 0 ; i < 4 ; i++){
        Serial.printf(" %c ", route_message[i]);
    }
    */

    // append destination mac addresses
    for( int i = 0 ; i < route[entry].routes; i++){
        for( int j = 0 ; j < 6 ; j++){
            route_message[message_length] = route[entry].destination[i][j];
            message_length++;
        }
    }

    for( int i = 0 ; i < message_length; i++){
        Serial.printf(" %02x ", route_message[i]);
    }
    Serial.printf("\r\n");

/*
    Serial.printf("sending routing table entry of length %d", message_length);
    Serial.printf("\r\n");

    sendMessage(route_message, message_length);

    for( int i = 0 ; i <= message_length ; i++){ 
        Serial.printf("%c", route_message[i]);
    }
    Serial.printf("\r\n");
    */

}


/*
  START MAIN
*/
void setup(){
    Serial.begin(115200);
    Serial.setDebugOutput(true);

    pinMode(csPin, OUTPUT);
    pinMode(irqPin, INPUT);

    wifiSetup();

    spiffsSetup();

    mdnsSetup();

    webServerSetup();

    loraSetup();

    // this is a terrible way of initializing this, but 
    route[0].address[0] = 0x01; 
    route[0].address[1] = 0xC1; 
    route[0].address[2] = 0x44; 
    route[0].address[3] = 0x67; 
    route[0].address[4] = 0xB1; 
    route[0].address[5] = 0x87; 

    route[0].metric = 10;
    route[0].routes = 3;

    route[0].destination[0][0] = 0x10; 
    route[0].destination[0][5] = 0x11;

    route[0].destination[1][0] = 0x20, 
    route[0].destination[1][5] = 0x12;

    route[0].destination[2][0] = 0x30, 
    route[0].destination[2][5] = 0x13;

}

int interval = 1000;          // interval between sends
long lastSendTime = 0; // time of last packet send

void loop(){

    int packetSize;

    // broadcast routing table every 3 seconds
    if (millis() - lastSendTime > interval) {
        
        // Print current routing table
        /*
        for( int i = 0 ; i < route[0].routes ; i++){
            for( int j = 0 ; j < 12 ; j++){
                Serial.printf("%c", route[0].destination[i][j]);
                Serial.printf(" via ");
                for( int k = 0 ; k < 12 ; k++){
                    Serial.printf("%c", route[0].address[k]);
                }
            }
        }
        Serial.printf("\r\n");
        */
	
        // Send routing entry
        broadcastRoute(0, 0);

        lastSendTime = millis();            // timestamp the message
        interval = random(2000) + 4000;    // 2-3 seconds
    }

}
