#!/usr/bin/env python3

import socket
import subprocess
import os

# Get current script directory
dir_path = os.path.dirname(os.path.realpath(__file__))
switchctl_path = dir_path + "/switchctl.sh"

def trigger_rules(command ,subcommand = "", level=""):
    if len(level) > 0:
        subprocess.call([switchctl_path, command, subcommand, level])
    else:
        subprocess.call([switchctl_path, command, subcommand])

# Socket configuration
# to comunicate with host
DPU_IP = '192.168.100.2'
DPU_PORT = 5000
    
trigger_rules("host")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind((DPU_IP, DPU_PORT))
s.listen(1)
conn, addr = s.accept()
print('Got signal from: ', addr)
conn.close()
trigger_rules("dpu")
