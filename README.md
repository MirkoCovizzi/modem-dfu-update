# Simple full modem DFU

## Requirements

- Python 3
- pyserial (`pip install pyserial`)
- NCS 1.9.99 (tested on this version)
- 2 different and compatible modem FW packages. They will be placed in the `fw` subdirectory inside the `modem_update` directory.
- Tested on Ubuntu. Device UART0 is seen by the OS as /dev/ttyACM0. Modify as necessary in the script depending on your configuration.

## Execution

- First run python script, which will wait for device to boot
- Boot device
