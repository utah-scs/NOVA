#!/usr/bin/env python3

import time 
import socket

# Time units
MICROSEC = 1/1000000
MILLISEC = 1/1000

# Inital wait
INITIAL_WAIT = 5

# Interval for collecting stats
DISC_INTERVAL = 10 * MILLISEC

# Interval for waiting after load shifting
WAIT_INTERVAL = 1

# Socket configuration
# to comunicate with host
DPU_IP = '192.168.100.2'
DPU_PORT = 5000
        
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

time.sleep(INITIAL_WAIT)

# Read file /tmp/interference
while True:
    with open('/tmp/interference', 'r') as f:
        interference = f.read().strip()

        if int(interference) == 1:
            print("Interference detected")
            try:
                s.connect((DPU_IP, DPU_PORT))
            except:
                print("Host monitor script is not running")
            s.close()
            time.sleep(WAIT_INTERVAL)

    time.sleep(DISC_INTERVAL)
