import socket
import json
import time
from gtts import gTTS #used for text-to-speech generation
import os
from vosk import Model, KaldiRecognizer #used to interpret speech
import requests

esp32ip = "" #place esp32 IP address here
udpPort = 8889
RATE = 16000
model = Model("vosk-model-en-us-0.42-gigaspeech") #download at https://alphacephei.com/vosk/models and place into .py directory
recognizer = KaldiRecognizer(model, 16000)

recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
recv_sock.bind(("0.0.0.0", 8888))
outgoing_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) # UDP


prev_flag = None #used to detect a change in the flag 

while True:
    data, addr = recv_sock.recvfrom(4096)
    flag = data[0] #first integer is the flag
    audio_data = data[1:]#audio data stream comes after first integer

    if flag != prev_flag:
        recognizer.Reset() #clear vosk in case old audio was still being interpretted
        prev_flag = flag

            
    if recognizer.AcceptWaveform(audio_data):
        result = json.loads(recognizer.Result())

        inferredText = result.get("text", "")

        if inferredText:
            print("Recognized:", inferredText)
            print(f'{outgoing_sock.sendto(bytes(inferredText, "utf-8"), (esp32ip, udpPort))}') 
            
            if(flag == 1): #this means that we need to also upload an mp3 file onto the ESP32
                print("Recognized mp3:", inferredText)
                language = 'en'
                myobj = gTTS(text=inferredText, lang=language, tld='us', slow=False)
                file_name = "inferredText.mp3"
                myobj.save(file_name)
                
                url = 'http://' + esp32ip + '/data'

                with open(file_name, "rb") as mp3:
                    response = requests.put(url, data=mp3)
                
                print("HTTP status:", response.status_code)
                print("Response:", response.text)

                print("mp3 sent")
                