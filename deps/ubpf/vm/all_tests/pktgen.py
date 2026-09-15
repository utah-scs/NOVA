#!/usr/bin/env python3

import scapy.all as scapy
import time
from ctypes import CDLL
import os

# Script directory
script_dir = os.path.dirname(os.path.realpath(__file__))

libubpf_so = "{}/../libubpf.so".format(script_dir)

VM_STATE_SIZE = CDLL(libubpf_so).ubpf_vm_size()
DMA_REQ_SIZE = CDLL(libubpf_so).dma_req_size()

HEADER_SIZE = 42
PAD_AFTER_HDR = 6

# Craft a packet with the specified IP addresses
def gen_packet(proto, src_port, dst_port, src_ip, dst_ip, payload):
    eth = scapy.Ether(src='02:1e:67:9f:4d:ae', dst='06:16:3e:1b:72:32')
    ip = scapy.IP(src=src_ip, dst=dst_ip)
    udp = proto(sport=src_port, dport=dst_port)

    data = 0 

    # Add padding after header
    vm_state = data.to_bytes(PAD_AFTER_HDR, 'little')
    vm_state += data.to_bytes(VM_STATE_SIZE, 'little')
    vm_state += data.to_bytes(DMA_REQ_SIZE, 'little')
    payload  = vm_state + payload

    pkt = eth/ip/udp/payload
    return bytes(pkt)

# Generate payload for set operation
# cmd field for set operation is 1
def gen_set_payload(offset, data):
    # Parameter length
    # 2b parameter + 8b timestamp + 8b sequence number
    # + 1b server type + 1b command + 8b offset + 8b size + len(Actual Data)
    param_size = 36 + len(data)
    timestamp = int(time.time())  # Timestamp
    timestamp2 = int(time.time()) # Timestamp
    nseq = 0                      # Sequence number
    server_type = 0               # Server type: 0 DPU, 1 HOST
    function_id = 0               # Function ID
    cmd = 1                       # Opcode
    size = len(data)              # Actual Data
    end = 'little'                # Endianness
    
    return param_size.to_bytes(2, end) + timestamp.to_bytes(8, end) + timestamp2.to_bytes(8, end) + nseq.to_bytes(8, end) + server_type.to_bytes(1, end) + function_id.to_bytes(1, end) + cmd.to_bytes(1, end) + offset.to_bytes(8, end) + size.to_bytes(8, end) + bytes(data, 'utf-8')

# Generate payload for get operation
# cmd field for get operation is 0
def gen_get_payload(offset, size):
    # Parameter length
    # 2b parameter + 8b timestamp + 8b sequence number
    # + 1b server_type + 1b command + 8b offset + 8b size
    param_size = 36
    timestamp = int(time.time())  # timestamp
    timestamp2 = int(time.time())  # timestamp2
    nseq = 0                      # Sequence number
    server_type = 0               # Server type: 0 DPU, 1 HOST
    function_id = 0               # Function ID
    cmd = 0                       # Opcode
    end = 'little'                # Endianness

    return param_size.to_bytes(2, end) + timestamp.to_bytes(8, end) + timestamp2.to_bytes(8, end) + nseq.to_bytes(8, end) + server_type.to_bytes(1, end) + function_id.to_bytes(1, end) + cmd.to_bytes(1, end) + offset.to_bytes(8, end) + size.to_bytes(8, end)

# Generate linked list payload
def gen_linked_list_payload(node_size):
    # Parameter length
    # 2b parameter + 8b timestamp + 8b sequence number
    # 1b server type + 4b node size
    param_size = 23
    timestamp = int(time.time())  # timestamp
    timestamp2 = int(time.time())  # timestamp
    nseq = 0                      # Sequence number
    server_type = 0               # Server type: 0 DPU, 1 HOST
    function_id = 0               # Function ID
    end = 'little'                # Endianness
    
    return param_size.to_bytes(2, end) + timestamp.to_bytes(8, end) + timestamp2.to_bytes(8, end) + nseq.to_bytes(8, end) + server_type.to_bytes(1, end) + function_id.to_bytes(1, end) + node_size.to_bytes(4, end)

# Generate payload for hashtable set operation
def gen_ht_set_payload(key, value):
    # Parameter length
    # 2b parameter + 8b timestamp + 8b sequence number
    # + 1b server type + 1b command + 8b offset + 8b size
    param_size = 36
    timestamp = int(time.time())  # Timestamp
    timestamp2 = int(time.time())  # Timestamp
    nseq = 0                      # Sequence number
    server_type = 1               # Server type: 0 DPU, 1 HOST
    function_id = 0               # Function ID
    cmd = 0                       # Opcode. Don't care for ht get/set
    offset = 0                    # Offset to data. Don't care for ht get/set
    size = 0                      # Size of data. Don't care for ht get/set
    end = 'little'                # Endianness

    # Hash table specific parameters (Part of Data)
    ht_command = 1                # HT command. 0 - GET, 1 - SET
                                  # Can be also used to flag status of the operation. e.g: Successful/Failed
    
    return param_size.to_bytes(2, end) + timestamp.to_bytes(8, end) + timestamp2.to_bytes(8, end) + nseq.to_bytes(8, end) + server_type.to_bytes(1, end) + function_id.to_bytes(1, end) + cmd.to_bytes(1, end) + offset.to_bytes(8, end) + size.to_bytes(8, end) + ht_command.to_bytes(1, end) + key.to_bytes(8, end) + value.to_bytes(8, end)

# Generate payload for hashtable set operation
def gen_ht_get_payload(key):
    # Parameter length
    # 2b parameter + 8b timestamp + 8b sequence number
    # + 1b server type + 1b command + 8b offset + 8b size
    param_size = 36
    timestamp = int(time.time())  # Timestamp
    timestamp2 = int(time.time())  # Timestamp
    nseq = 0                      # Sequence number
    server_type = 1               # Server type: 0 DPU, 1 HOST
    function_id = 0               # Function ID
    cmd = 0                       # Opcode. Don't care for ht get/set
    offset = 0                    # Offset to data. Don't care for ht get/set
    size = 0                      # Size of data. Don't care for ht get/set
    end = 'little'                # Endianness

    # Hash table specific parameters (Part of Data)
    ht_command = 0                # HT command. 0 - GET, 1 - SET.
                                  # Can be also used to flag status of the operation. e.g: Successful/Failed

    value = 0                     # Placeholder for the GET operation
    
    return param_size.to_bytes(2, end) + timestamp.to_bytes(8, end) + timestamp2.to_bytes(8, end) + nseq.to_bytes(8, end) + server_type.to_bytes(1, end) + function_id.to_bytes(1, end) + cmd.to_bytes(1, end) + offset.to_bytes(8, end) + size.to_bytes(8, end) + ht_command.to_bytes(1, end) + key.to_bytes(8, end) + value.to_bytes(8, end)

get_pkt = gen_packet(scapy.UDP, 10003, 10004, '172.12.55.99', '12.34.56.78', gen_get_payload(16, len('helloworld')))
set_pkt = gen_packet(scapy.UDP, 10001, 10002, '172.16.100.1', '10.0.0.1', gen_set_payload(16, 'helloworld'))
list_pkt = gen_packet(scapy.UDP, 10001, 10002, '172.16.100.1', '10.0.0.1', gen_linked_list_payload(8))
ht_get_pkt = gen_packet(scapy.UDP, 10003, 10004, '172.12.55.99', '12.34.56.78', gen_ht_get_payload(47))
ht_set_pkt = gen_packet(scapy.UDP, 10003, 10004, '172.12.55.99', '12.34.56.78', gen_ht_set_payload(47, 666))


print("VM state size: {}".format(VM_STATE_SIZE))
print("DMA state size: {}".format(DMA_REQ_SIZE))
print("Total pkt state size: {}".format(VM_STATE_SIZE + DMA_REQ_SIZE))
print("Packet header length [eth/ip/udp]: " + str(HEADER_SIZE))
print("Padding after header: " + str(PAD_AFTER_HDR))
print("Application defined data offset: " + str(HEADER_SIZE + PAD_AFTER_HDR + VM_STATE_SIZE + DMA_REQ_SIZE))
print("Set packet length: Total " + str(len(set_pkt)) + " Payload " + str(len(set_pkt) - HEADER_SIZE))
print("Get packet length: Total " + str(len(get_pkt)) + " Payload " + str(len(get_pkt) - HEADER_SIZE))
print("List packet length: Total " + str(len(list_pkt)) + " Payload " + str(len(list_pkt) - HEADER_SIZE))
print("HT set packet length: Total " + str(len(ht_set_pkt)) + " Payload " + str(len(ht_set_pkt) - HEADER_SIZE))
print("HT get packet length: Total " + str(len(ht_get_pkt)) + " Payload " + str(len(ht_get_pkt) - HEADER_SIZE))

all_packets = {"read": get_pkt, "write": set_pkt, "list": list_pkt, "ht_get": ht_get_pkt, "ht_set": ht_set_pkt}

# Write packets to file
for key in all_packets.keys():
    fname = script_dir + "/" + key + ".pkt"

    with open(fname, "wb") as bin_file:
        bin_file.write(all_packets[key])
