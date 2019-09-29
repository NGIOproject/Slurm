#!/bin/bash --login

ifup usb0
sleep 1

curl -k -u XXXXX:YYYYYYYYYY -H "Accept: application/json" -H "Content-type: application/json" -L -X POST -i https://169.254.0.2/rest/v1/Oem/eLCM/ProfileManagement/set?inhibit_reboot -d "{\"Server\": {\"SystemConfig\": {\"BiosConfig\": {\"MemoryConfig\": {\"VolatileMemoryMode\": \"2LM\"}, \"@Version\": \"1.04\"}}, \"@Version\": \"1.01\", \"@Processing\": \"execute\"}}"

