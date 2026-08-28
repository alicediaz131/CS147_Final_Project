#include <Arduino.h>
#include "SPIFFS.h"

#include "Audio.h"
#include <driver/i2s.h>

//i2s amplifier + speaker pins
#define AMP_I2S_LRC 14
#define AMP_I2S_BCLK 27
#define AMP_I2S_DIN 26
#define AMP_I2S_PORT I2S_NUM_0

Audio audio(AMP_I2S_PORT); //used to play mp3 files

#include <queue> //used to queue up mp3 files

//microphone pins
#define MIC_I2S_SD   35
#define MIC_I2S_SCK  32
#define MIC_I2S_WS   12
#define MIC_I2S_PORT I2S_NUM_1 // Use I2S port 1

// Audio buffer configuration
#define bufferLen 256  // Increase buffer size to accommodate more audio data



//distance sensor pins and constants
#define ECHO 18
#define TRIG 5
#define SOUND_SPEED 0.034
#define CM_TO_INCH 0.393701

long duration;
float distanceCm;
float distanceInch;

//variables for checking distance
unsigned long distance_tick_millis = 5000;
unsigned long distance_tick_step = 0;
const unsigned long WAKEUP_DISTANCE = 3.0;

//time to track what day it is
#include "time.h"

struct tm timeinfo; //used to check the time


//various server, WiFi, and telemetry libraries
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

//for downloading mp3
#include <WebServer.h>
File rawFile;
WebServer server(80);


// WiFi network credentials
const char* ssid = "";           // Replace with your WiFi name
const char* password = "";   // Replace with your WiFi password

// UDP server settings (where audio data will be sent)
const char* host = ""; // IP address of the computer/server receiving audio
const int audioPort = 8888;                  // Port number the computer/server receiver is listening on
const int stringPort = 8889;				// Port number to receive transcriptions

//WiFi objects
WiFiUDP outAudioUDP;              // Create a UDP object for streaming audio
WiFiUDP stringUDP;              // Create a UDP object for receiving transcriptions



//variables/constants for playing audio
int16_t sBuffer[bufferLen]; // Buffer array to hold 16-bit audio samples
std::queue<char*> audioQueue; //used to queue up audio files

// variables/constants for getting current date and time over WiFi
// used for keeping track of what day it is
const char* ntpServer = "pool.ntp.org"; //time is in GMT
long  gmtOffset_sec = -28800 ; //PST is 8 hours behind GMT, so subtract 8 hours in seconds
const int   daylightOffset_sec = 3600;

#define SAS_TOKEN "place SAS token here"
// Azure IoT Hub configuration"
// Root CA certificate for Azure IoT Hub    
const char* root_ca = "-----BEGIN CERTIFICATE-----\n" \
"place ceterificate here\n" \
"-----END CERTIFICATE-----\n";


String iothubName = ""; //Your hub name (replace if needed)
String deviceName = ""; //Your device name (replace if needed)
String url = "https://" + iothubName + ".azure-devices.net/devices/" + deviceName + "/messages/events?api-version=2021-04-12";


//set up functions
void setup_wifi();
void i2s_install();
void i2s_setpin(); 

//for waking up and sleeping the device and other state control
void detect_distance();
void enter_sleep_state();
void queue_prompt();


//for receiving user commands via speech-to-text transcript
void listen_for_command();
void send_audio(const unsigned int flag);
bool receive_transcription(char *);

//various user commands and their submethods
void create_new_habit();
bool create_TTS();

void report_habit();

void list_habits();

void start_new_day();
void demo_next_day();


//functions related to saving/loading/trasmitting/resetting habits data
void send_telemetry();
void load_habits_from_JSON();
void generate_habits_JSON(ArduinoJson::JsonDocument &doc);
void print_habits();
void reset_habits();

//used to run HTTP server on the ESP32 for the python server to upload mp3 files to
void handleCreate();
void handleCreateProcess();
void handleNotFound();


//states the device can be in. 
//used for the finite state machine
enum DeviceState {SLEEPING, SPEAKING, PROMPT_USER, LISTEN_COMMAND, 
CREATE_NEW_HABIT, LISTEN_CONFIRMATION, REPORT_HABIT}; 

DeviceState deviceState; //current state device is in
DeviceState nextState; //next state device will change to when finished with work

float sleepTimer = 0.0; // used to know when to put the device to sleep.

bool transcriptRecv = false; //set to true once a transcription is fully received.
bool mp3Recv = false; //set to true once an MP3 file is fully received.
unsigned int nextHabitIndex; //used to remember the index of the next habit to write info to while downloading files


//used to encapsulate habit data
class Habit{
    public:
    bool is_active = false;
    bool reported_today = false;
    unsigned int tally = 0;
    char name [bufferLen] = "";
    char mp3Path[13] = "";
    struct tm time_created;
    struct tm last_time_reported;
    
};
#define HABIT_COUNT 5
Habit habits[HABIT_COUNT]; //array used to access habits data


//paths for mp3 files
char mp3_prompt[] =  "/demo_prompt.mp3", mp3_welcome[] = "/demo_welcome.mp3", 
mp3_new_habit[] = "/demo_new_habit.mp3", mp3_todays_habits[] = "/todays_habits.mp3", mp3_good_job[] = "/good_job.mp3", mp3_report_habit[] = "/report_habit.mp3",
mp3_habit_added[] = "/habit_added.mp3", mp3_goodbye[] = "/goodbye.mp3", habitsJSON[] = "/habits.json";



void setup() {
    Serial.begin(115200);

    //set up the microphone
    Serial.println("Setting up I2S...");

    // Connect to WiFi network
    setup_wifi();

    delay(1000);
    i2s_install();   // Configure and install the I2S driver
    i2s_setpin();    // Set the I2S pins
    i2s_start(MIC_I2S_PORT); // Start the I2S receiver
    delay(500);
    stringUDP.begin(stringPort);
    
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    if(!getLocalTime(&timeinfo)){
        Serial.println("Failed to obtain time");
    }
    delay(500);
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");

    //set up the i2S amplifier + speaker
    if (!SPIFFS.begin(true)) {
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }
    Serial.println("SPIFFS mounted successfully");
	
	//setup amplifier pinout
    audio.setPinout(AMP_I2S_BCLK, AMP_I2S_LRC, AMP_I2S_DIN);
    audio.setVolume(20);


    //set up the echo sensor pinmodes
    pinMode(TRIG, OUTPUT); // Sets the trigPin as an Output
    pinMode(ECHO, INPUT); // Sets the echoPin as an Input

    //write mp3 paths to habit array
    for (int i = 0; i < HABIT_COUNT; ++i){
        String tempstr =  "/habit_" + String(i) + ".mp3";
        strcpy(habits[i].mp3Path, tempstr.c_str());
    }
    
	//initialize the on board HTTP server
    server.on("/data", HTTP_PUT, handleCreate, handleCreateProcess);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started");

	//start in sleep state
    enter_sleep_state();
	
	//try to load previously saved habits data
    load_habits_from_JSON();
	
    Serial.println("setup complete");
}




//the loop contains the finite state machine logic and runs the appropriate functions
//it uses deviceState to track what the device should currently be doing and nextState
//to track what state to enter once it completes its current job
void loop() {
    server.handleClient(); //for on board HTTP server 
    switch(deviceState){
    //run this when detecting for a person nearby
        case SLEEPING:
            detect_distance();
            
            struct tm temptimeinfo;
            
            if(!getLocalTime(&temptimeinfo)){
                Serial.println("Failed to obtain time");
            }else if (temptimeinfo.tm_yday != timeinfo.tm_yday){
                start_new_day();
            }
            timeinfo = temptimeinfo;
            break;
        //run this when playing queued audio
        case SPEAKING:
            audio.loop();
            if (audio.isRunning() == false){
                if(audioQueue.empty()){
                    deviceState = nextState;
                    if(nextState == SLEEPING)
                        enter_sleep_state();
                } else {
                    const char* mp3path = audioQueue.front();
                    audioQueue.pop();
                    audio.connecttoFS(SPIFFS, mp3path);

                }
            }
            break;
        //run this to listen to a person speak
        case PROMPT_USER:
            audio.loop();
            if (audio.isRunning() == false){
                deviceState = LISTEN_COMMAND;
                sleepTimer = millis() + 300000.0;
            }
            break;
        case LISTEN_COMMAND:
            if (sleepTimer < millis()){
                deviceState = SPEAKING;
                audioQueue.push(mp3_goodbye);
                nextState = SLEEPING;
                break;
            }
            listen_for_command();
            break;
        case CREATE_NEW_HABIT:
            create_new_habit();
            break;
        case REPORT_HABIT:
            report_habit();
            break;
        }
}





// Function to handle WiFi connection
void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password); // Initiate connection

    // Wait for connection to establish
    while (WiFi.status() != WL_CONNECTED) {
        delay(600);
        Serial.print("-"); // Print a dash every 600ms while connecting
    }

    // Connection successful
    Serial.println("\nWiFi connected");
    Serial.println("IP address assigned: ");
    Serial.println(WiFi.localIP()); // Print the ESP32's IP address
}

// Function to install and configure the I2S driver
void i2s_install() {
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), // Set as master receiver
        .sample_rate = 16000,              // Audio sample rate (16kHz)
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // 16-bit per sample
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // Use right channel only (mono)
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S), // Standard I2S format
        .intr_alloc_flags = 0,             // No interrupt flags
        .dma_buf_count = 8,                // Number of DMA buffers
        .dma_buf_len = bufferLen,          // Size of each DMA buffer
        .use_apll = false                  // Do not use APLL clock
    };

    i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL); // Install the driver

    }

// Function to set the I2S pinout
void i2s_setpin() {
    const i2s_pin_config_t pin_config = {
        .bck_io_num = MIC_I2S_SCK,   // Bit clock pin
        .ws_io_num = MIC_I2S_WS,     // Word select pin
        .data_out_num = I2S_PIN_NO_CHANGE, // No data output needed (RX only)
        .data_in_num = MIC_I2S_SD    // Data input pin (from microphone)
    };

    i2s_set_pin(MIC_I2S_PORT, &pin_config); // Apply the pin configuration
}



//used to wake up the device when an object comes within WAKEUP_DISTANCE
void detect_distance(){
    switch(distance_tick_step){
        case 0:
            if (distance_tick_millis <= (millis()+50.0) && distance_tick_millis >= (millis()-50.0)){
                digitalWrite(TRIG, LOW);
                ++distance_tick_step;
            }
            break;
        case 1:
                digitalWrite(TRIG, HIGH);
                ++distance_tick_step;
            //}
            break;
        case 2:
                digitalWrite(TRIG, LOW);
                duration = pulseIn(ECHO, HIGH);

                // Calculate the distance
                distanceCm = duration * SOUND_SPEED/2;

                // Convert to inches
                distanceInch = distanceCm * CM_TO_INCH;

                // Prints the distance in the Serial Monitor
                Serial.print("Distance (cm): ");
                Serial.println(distanceCm);
                Serial.print("Distance (inch): ");
                Serial.println(distanceInch);
                if (distanceInch < WAKEUP_DISTANCE){
                    audioQueue.push(mp3_welcome);
                    stringUDP.flush();
                    queue_prompt();
                }
                distance_tick_step = 0;
                distance_tick_millis = millis() + 1000.0;    
            //}
            break;
    }
}

//used when the device needs to begin sleeping
void enter_sleep_state(){
    distance_tick_millis = 5000.0 + millis();
    distance_tick_step = 0;
    deviceState = SLEEPING;
    nextState = SLEEPING;
    stringUDP.flush();
}

// used when the device needs to prompt the user.
void queue_prompt(){
    audioQueue.push(mp3_prompt);
    deviceState = SPEAKING;
    nextState = LISTEN_COMMAND;
    sleepTimer = millis() + 30000.0;
}


//sends audio, and once a transcription is receiving, matches the transcription to a command, 
//triggering a new state and running new functions.
void listen_for_command(){
    send_audio(0);
    char command[bufferLen] = {0};
    if (receive_transcription(command)){
        if ((strstr(command, "create") || strstr(command, "new")) && strstr(command, "habit")){
            audioQueue.push(mp3_new_habit);
            deviceState = SPEAKING;
            nextState = CREATE_NEW_HABIT;
            transcriptRecv = false;
            mp3Recv = false;


            for(int i = 0; i < HABIT_COUNT; ++i){
                if(!habits[i].is_active){
                    nextHabitIndex = i;
                    break;
                }
            }
        }
        else if (strstr(command, "report") && strstr(command, "habit")){
            audioQueue.push(mp3_report_habit);            
            deviceState = SPEAKING;
            nextState = REPORT_HABIT;
        }
        else if (strstr(command, "list") && strstr(command, "habit"))
        {
            list_habits();
        }
        else if (strstr(command, "next") && strstr(command, "day")){
            demo_next_day();
            deviceState = SPEAKING;
            audioQueue.push(mp3_goodbye);
            nextState = SLEEPING;
        }
        else if (strstr(command, "reset") && strstr(command, "habits")){
            reset_habits();
            deviceState = SPEAKING;
            audioQueue.push(mp3_goodbye);
            nextState = SLEEPING;
        }
    }
}

//this function sends audio from the microphone to the python server.
//the flag will be sent as the first in the audio stream packet
//this helps the server know if it should send transcribed audio or an mp3 file back.
void send_audio(const unsigned int flag){

    size_t bytesIn = 0;
    // Read audio data from the I2S buffer
    esp_err_t result = i2s_read(MIC_I2S_PORT, &sBuffer, bufferLen * sizeof(int16_t), &bytesIn, portMAX_DELAY);

    // If data was read successfully and the buffer isn't empty
    if (result == ESP_OK && bytesIn > 0) {
        // Send the audio data via UDP to the specified host and port

        //packet with 1 byte added to the front for commands for the remote server
        uint8_t packet[1 + bufferLen * sizeof(int16_t)];


        packet[0] = flag;

        memcpy(packet + 1, sBuffer, bytesIn);
        
        outAudioUDP.beginPacket(host, audioPort);
        outAudioUDP.write(packet, 1 + bytesIn);
        outAudioUDP.endPacket();
    }
}

//this function listens to the microphone for words. 
//if any words were heard by the speech-to-text program, the ESP32 will receive a transcription string from a UDP packet
//and place it in the buffer
bool receive_transcription(char* buffer){
    //run this to wait for a return message
    int incPacketSize = stringUDP.parsePacket();
    if (incPacketSize > 0){
        Serial.print("Incoming pack with bytes of length ");
        Serial.println(incPacketSize);

        int len = stringUDP.read(buffer, bufferLen - 1);
        stringUDP.flush();
        if (len > 0){
            buffer[len] = '\0';
            Serial.println(buffer);
        }   else   {
            buffer[0] = '\0';
            return false;
        }
        return true;
    }
    return false;
}


void create_new_habit(){
    if (create_TTS()){
        habits[nextHabitIndex].is_active = true;
        getLocalTime(&habits[nextHabitIndex].time_created);
        send_telemetry();
        audioQueue.push(habits[nextHabitIndex].mp3Path);
        audioQueue.push(mp3_habit_added);
        queue_prompt();
    }
}


bool create_TTS(){
    //try to download the transcription
    if (transcriptRecv == false){
        send_audio(1);
        char buffer [bufferLen];
        if (receive_transcription(buffer)){
            strcpy(habits[nextHabitIndex].name, buffer);
            transcriptRecv = true;
        }
    }
 
	//mp3 file is uploaded via HTTP server, which in the main loop function (server.handleClient();)

    //once both are received, return true
    return mp3Recv && transcriptRecv;
}



//this function plays the audio for all user created habits
//skipping the ones that have already been completed today.
void list_habits(){
    audioQueue.push(mp3_todays_habits);
    for(int i = 0; i < HABIT_COUNT; ++i){
        if(habits[i].is_active && !habits[i].reported_today)
            audioQueue.push(habits[i].mp3Path);
    }
    queue_prompt();
}


//this function finds the matching habit reported by a user
//and sets reported_today to true
void report_habit(){
    send_audio(0);
    char habitName[bufferLen] = {0};
    if (receive_transcription(habitName)){
        for (int i = 0; i < HABIT_COUNT; ++i){
            if (strstr(habitName, habits[i].name)){
                habits[i].reported_today = true;
                ++habits[i].tally;
                getLocalTime(&habits[i].last_time_reported);
                audioQueue.push(habits[i].mp3Path);
                audioQueue.push(mp3_good_job);
                queue_prompt();
                send_telemetry();
                return;
            } 
        }    
    }
}




//used when the day was detected to have changed 
void start_new_day(){
    Serial.println("The day has been reset");
    for(int i = 0; i < HABIT_COUNT; ++i){
        habits[i].reported_today = false;
    }
    send_telemetry();
}

//this function adds a day to the clock for demonstration purposes
void demo_next_day(){
    gmtOffset_sec += 86400; // add 24 hours to the clock offset
    
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); //reconfigure the time
}





//this function places habit data into a JSON, saves it to flash and sends it to Azure cloud,
void send_telemetry(){
    //Create JSON payload
    ArduinoJson::JsonDocument doc;
    generate_habits_JSON(doc);

    char buffer[2048];
    ArduinoJson::serializeJson(doc, buffer, sizeof(buffer));

    // Send telemetry via HTTPS
    WiFiClientSecure client;
    client.setCACert(root_ca); // Set root CA certificate
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", SAS_TOKEN);
    int httpCode = http.POST(buffer);


    if (httpCode == 204) { // IoT Hub returns 204 No Content for successful telemetry
      Serial.println("Telemetry sent: " + String(buffer));
    } else { //print if a message didn't successfully send to cloud
         
      Serial.println("Failed to send telemetry. HTTP code: " + String(httpCode) + ": " + http.errorToString((httpCode)));
    }
    http.end();

}


//this function loads a previously saved JSON file from SPIFFS memory and saves the data
//into the habits[] array. if there is no previously created JSON, it makes a blank one by calling generate_habits_JSON
void load_habits_from_JSON(){
    ArduinoJson::JsonDocument doc;
    File jsonFile;
    if (!SPIFFS.exists(habitsJSON)) {
        Serial.println("No JSON exists, generating new JSON with blank data");
        generate_habits_JSON(doc);
    } else{
        Serial.println("JSON found!, creating JSON with previously saved data");
        jsonFile = SPIFFS.open(habitsJSON, FILE_READ);
        ArduinoJson::deserializeJson(doc, jsonFile);
        jsonFile.close();
    }
    

    ArduinoJson::JsonArray jsonArr = doc["habits"];
    for(int i = 0; i < HABIT_COUNT; ++i){
        const char* tempStr = jsonArr[i]["name"];

        strncpy(habits[i].name, jsonArr[i]["name"], bufferLen-1);
        habits[i].name[bufferLen-1] = '\0';
        habits[i].tally = jsonArr[i]["tally"];
        habits[i].is_active = jsonArr[i]["is_active"];
        habits[i].reported_today = jsonArr[i]["reported_today"];
        
        time_t tempTime = jsonArr[i]["time_created"];
        localtime_r(&tempTime, &habits[i].time_created);

        tempTime = jsonArr[i]["last_time_reported"];
        localtime_r(&tempTime, &habits[i].last_time_reported);

    }
    print_habits();
}

//this function creates a new habits json document and saves the newly created version
//in SPIFFS
void generate_habits_JSON(ArduinoJson::JsonDocument &doc){
    ArduinoJson::JsonArray jsonArr = doc["habits"].to<JsonArray>();

    for (int i = 0; i < HABIT_COUNT; ++i){
        ArduinoJson::JsonObject habitObj = jsonArr.add<JsonObject>();
        habitObj["name"] = habits[i].name;
        habitObj["tally"] = habits[i].tally;
        habitObj["is_active"] = habits[i].is_active;
        habitObj["reported_today"] = habits[i].reported_today;

        //convert time to time_t so you can use strings.
        time_t tempTime = mktime(&habits[i].time_created);
        habitObj["time_created"] = String(tempTime);
        tempTime = mktime(&habits[i].last_time_reported);
        habitObj["last_time_reported"] = String(tempTime);
        
    }
    File jsonFile = SPIFFS.open(habitsJSON, FILE_WRITE);
    ArduinoJson::serializeJson(doc, jsonFile);
    jsonFile.close();
}

//prints the current habit array for debugging
void print_habits(){
    for(int i = 0; i < HABIT_COUNT; ++i){
        Serial.print("Habit #: ");
        Serial.println(i);
        Serial.print("name: ");
        Serial.println(String(habits[i].name));
        Serial.print("tally: ");
        Serial.println(habits[i].tally);
        Serial.print("is_active: ");
        Serial.println(habits[i].is_active);
        Serial.print("reported_today: ");
        Serial.println(habits[i].reported_today);

        //convert time to time_t so you can use strings.
        time_t tempTime = mktime(&habits[i].time_created);
        Serial.println("time_created: " + String(tempTime));
        tempTime = mktime(&habits[i].last_time_reported);
        Serial.println("last_time_reported: " + String(tempTime));
    }
}

//completely resets the habits array for demo purposes
void reset_habits(){
    for(int i = 0; i < HABIT_COUNT; ++i){
        strcpy(habits[i].name, "");
        habits[i].is_active = false;
        habits[i].reported_today = false;
        habits[i].tally = 0;
    }
    ArduinoJson::JsonDocument doc;
    generate_habits_JSON(doc); //make a new blank doc
}



//the next three functions are used for running an HTTP server and uploading
//an mp3 file to SPIFFS.
void handleCreate() {
  server.send(200, "text/plain", "");
}


void handleCreateProcess() {
  
  String path = String(habits[nextHabitIndex].mp3Path);
  
  HTTPRaw &raw = server.raw();
  if (raw.status == RAW_START) {
    if (SPIFFS.exists(path)) {
      SPIFFS.remove(path);
    }

    rawFile = SPIFFS.open(path.c_str(), FILE_WRITE);
    Serial.print("Upload: START, filename: ");
    Serial.println(path);
  } else if (raw.status == RAW_WRITE) {
    if (rawFile) {
      rawFile.write(raw.buf, raw.currentSize);
    }
    Serial.print("Upload: WRITE, Bytes: ");
    Serial.println(raw.currentSize);
  } else if (raw.status == RAW_END) {
    if (rawFile) {
      rawFile.close();
    }
    Serial.print("Upload: END, Size: ");
    Serial.println(raw.totalSize);
    mp3Recv = true; //used to tell device that mp3 download was successful
  }
}


void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (int i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}