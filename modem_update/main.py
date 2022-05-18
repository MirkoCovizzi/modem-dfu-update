import os
import time
from threading import Thread
from enum import IntEnum

import serial
from zipfile import ZipFile

DEV_NAME = '/dev/ttyACM0'
BAUDRATE = 115200

dbg = False

dirname = os.path.dirname(__file__)
fw_dirname = os.path.join(dirname, "fw")
temp_dirname = os.path.join(dirname, "temp")
fw_filename = os.path.join(fw_dirname, os.listdir(fw_dirname)[0])

ser = serial.Serial(DEV_NAME, BAUDRATE)

bootloader = None
certificate = None
firmware = None

dfu_start = False
wait_for_boot = False
started = False


class Segment(IntEnum):
    BL = 1
    CERT = 2
    FW = 3


def term_out():
    while True:
        line = ser.readline()

        if b'DFU start' in line:
            global dfu_start
            dfu_start = True

        global started
        if b'Modem FW UUID' in line and not started:
            # Get UUID from device
            uuid = line.decode('utf-8').split(' ')[-1]
            # Cleanup uuid string
            uuid = uuid.replace('\r\n', '')
            global fw_filename
            if uuid in fw_filename:
                # Choose the other firmware with different UUID
                fw_filename = os.path.join(fw_dirname, os.listdir(fw_dirname)[1])

            global wait_for_boot
            wait_for_boot = True
            started = True

        if dbg:
            print(line)
        else:
            print(line.decode('utf-8'), end='')


def parser(line):
    line = line[1:]
    len_val = line[0:2]
    len_int = int(line[0:2], 16)
    addr = line[2:6]
    type_val = line[6:8]
    end = 8 + len_int * 2
    data = line[8:end]
    check = line[end:-1]
    len_hex = bytes.fromhex(len_val)
    addr_hex = bytes.fromhex(addr)
    type_hex = bytes.fromhex(type_val)
    data_hex = bytes.fromhex(data)
    check_hex = bytes.fromhex(check)
    return len_hex, addr_hex, type_hex, data_hex, check_hex


def serialize(f_name, segment):
    with open(f_name, "rt") as bl:
        for line in bl.readlines():
            len_hex, addr_hex, type_hex, data_hex, check_hex = parser(line)
            seg_hex = int(segment).to_bytes(1, byteorder='little')
            ser.write(len_hex)
            ser.write(addr_hex)
            ser.write(type_hex)
            ser.write(data_hex)
            ser.write(check_hex)
            ser.write(seg_hex)
            if dbg:
                print(len_hex, addr_hex, type_hex, data_hex, check_hex, seg_hex)
                time.sleep(0.1)


if __name__ == '__main__':
    t = Thread(target=term_out)
    t.start()

    while not wait_for_boot:
        pass

    print("Flashing fw: ", fw_filename)

    with ZipFile(fw_filename, 'r') as zip_file:
        for name in zip_file.namelist():
            if 'signed.ihex' in name:
                zip_file.extract(name, path=temp_dirname)
                bootloader = name
            elif 'segments.0' in name:
                zip_file.extract(name, path=temp_dirname)
                certificate = name
            elif 'segments.1' in name:
                zip_file.extract(name, path=temp_dirname)
                firmware = name

    while not dfu_start:
        pass

    s_time = time.time()

    bl_filename = os.path.join(temp_dirname, bootloader)
    serialize(bl_filename, segment=Segment.BL)

    cert_filename = os.path.join(temp_dirname, certificate)
    serialize(cert_filename, segment=Segment.CERT)

    fw_filename = os.path.join(temp_dirname, firmware)
    serialize(fw_filename, segment=Segment.FW)

    e_time = time.time() - s_time

    print("Flashed in: ", e_time)

    t.join()
