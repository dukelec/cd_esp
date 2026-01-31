CD-ESP
=======================================

An ESP32C3-based CDBUS (RS-485) wireless bridge with BLE and Wi-Fi support.


## Communication Interfaces

### RS-485
 - Default baud rate: 115200 bps
 - Maximum speed: 50 Mbps
 - Default address: 0xfe

The underlying protocol is CDBUS, with the following frame format:  
`src, dst, len, [payload], crc_l, crc_h`

Each frame includes a 3-byte header, a variable-length payload, and a 2-byte CRC (identical to Modbus CRC).  
For more information on the CDBUS protocol, please refer to:
 - https://cdbus.org

The payload is encoded using the CDNET protocol. For detailed information, please refer to:
 - https://github.com/dukelec/cdnet
 - https://github.com/dukelec/cdnet/wiki/CDNET-Intro-and-Demo

### BLE

#### Advertising:

The full device name is: `CD-ESP XXXXXX`

`XXXXXX` represents the first 3 bytes of the device MAC address.  
Example: `CD-ESP dc1ed5`

Manufacturer Specific Data: 6-byte full device address.

#### Service & Characteristics:

Service UUID：`b3340001-56ba-40b1-8ecb-8fe18dfffddd`

Characteristic RX:
 - UUID: `b3340002-56ba-40b1-8ecb-8fe18dfffddd`
 - Property: `write-no-response`

Characteristic TX:
 - UUID: `b3340003-56ba-40b1-8ecb-8fe18dfffddd`
 - Property: `notify`


### WiFi Station

For initial setup, the device must be provisioned via BLE or RS-485.
After successfully connecting to the specified Wi-Fi access point,
the device IP address can be queried via BLE or RS-485, discovered through the local mDNS service,
or accessed directly using the `cd-esp.local` hostname.

Subsequent communication uses the device UDP port 52685 (0xCDCD) for data transmission and reception.


## Communication Diagram

<img src="doc/block_diagram.svg" width="100%">

 - (1): Any RS-485 node accesses the CD-ESP itself, for example to configure the network.
 - (2): The response packet corresponding to command (1), or a proactively reported data packet
        (e.g., CD-ESP sending debug print information to the CDBUS GUI Tool).
 - (3)(5): Accessing the CD-ESP itself via BLE or UDP, for example to configure the network or query status.
 - (4)(6): The response packets corresponding to commands (3) and (5).
 - (7)(9): BLE or UDP accesses any RS-485 bus node through the CD-ESP as a proxy.
 - (8)(10): The response packets corresponding to commands (7) and (9), or proactively reported data packets from any RS-485 node forwarded by the CD-ESP.
 - (13)(14): The CD-ESP actively sends commands to any RS-485 node and receives the corresponding response packets.


## Protocol

All interfaces are based on the CDNET L0 protocol. The CDNET packet encapsulation over BLE and UDP is as follows:

<img src="doc/svg_out/_cdnet_wb.svg" width="100%">

 - (1): The simplest mode — raw transmission of a single CDNET packet.  
        S-PORT is the source port and T-PORT is the destination port, each 1 byte.  
        Packet size ranges from 2 to 253 bytes.
 - (2): Concatenation of multiple CDNET packets.  
        Except for the last one, all CDNET packets must be exactly 253 bytes.
 - (3): Full format with a WHDR header.  
        (The same constraints as (2) apply when concatenating multiple CDNET packets.)
   * WHDR (Wireless Header): 1 byte, MSB is always 1.
   * A-CNT: 2 bytes, AES256 counter, optional.
   * T-MAC: 1 byte, target RS-485 node address, optional.

Recommendations:
 - BLE: maximum 244 or 495 bytes per transmission.
 - UDP: up to 5 CDNET packets per transmission.


#### Proxying:

 - When bit5 of the CDNET temporary port is 0, the communication target is the CD-ESP itself.
 - When bit5 is 1, the packet is forwarded to the other end via the CD-ESP proxy.
 - Command/report packets from RS485 are proxied by default when the target port is greater than 8.

When proxying is enabled:
 - If the target is the RS-485 node specified by the `p_mac` register, the T-MAC field is not included.
 - Otherwise, the T-MAC field is included to specify the target RS-485 node address.


### WHDR Definition

| FIELD   | DESCRIPTION                                 |
|-------- |---------------------------------------------|
| [7]     | Always 1 (indicates this byte is a WHDR)    |
| [6]     | a_cnt_en                        |
| [5]     | t_mac_en                        |
| [4:3]   | frag_type (00: no fragment, 01: first, 10: continue, 11: last) |
| [2:0]   | frag_cnt (frag_type ≠ 0) or err_code (frag_type = 0)           |

For fragmented packets, the frag_cnt of the first fragment may be any value;
it is incremented by 1 for each subsequent fragment.


### Encryption and Fragmentation

<img src="doc/svg_out/_cdnet_wb_frag.svg" width="100%">

This diagram illustrates encryption with fragmentation enabled. The actual transmitted and received packets are (3)(4)(5).
The number of fragments depends on the total payload size and the fragment size.  
For example, when the T-MAC field is not enabled, the WHDR values of (3)(4)(5) are: 0b11001000, 0b11010001, 0b11011010.

If encryption only is enabled, the transmitted and received packet is (2).  
For example, when the T-MAC field is not enabled, the WHDR of (2) is: 0b11000000.

If fragmentation only is enabled, the fragmented data is the unencrypted plaintext.  
In this case, the WHDR values of plaintext-carrying (3)(4)(5) are: 0b10001000, 0b10010001, 0b10011010.

When encryption or fragmentation is enabled:
 - On decryption failure, a single-byte WHDR packet is returned with err_code = 2 (bit[6:3] = 0).
 - On fragment reassembly failure, an error is reported upon receiving the last fragment as a single-byte WHDR packet with err_code = 1 (bit[6:3] = 0).

Recommendations and limitations:
 - Fragmentation is recommended only when encryption is enabled over BLE.
 - UDP does not support fragmentation, as UDP packets are sufficiently large and do not require it.


#### AES256 Encryption

When a packet requires encryption:
 - Enable encryption by setting `a_cnt_en` = 1. Then append 2 bytes of A-CNT after the WHDR
   (note: separate counters are maintained for send/receive and for BLE/Wi-Fi, totaling 4 counters).
 - Before communication, read the plaintext `k_random` (changes on each power-up).  
   For example, if k_random = 0xabcd1234 and the default password string is "123456",
   the AES256 key is derived by computing the SHA256 of the string: `cd_abcd1234_123456`. The IV is fixed to all zeros.
 - Also read `k_cnt_rx_ble/udp` (defaults to 0 at startup). Upon receiving an encrypted packet, CD-ESP checks this counter;  
   If it doesn’t match, an error is reported. Otherwise, the counter increments automatically.
 - For encrypted packets, only the 1-byte WHDR remains unencrypted; everything after WHDR is encrypted with AES256-CBC using PKCS#7 padding.
 - When encryption is enabled (`k_en` bit0 for BLE; bit1 for Wi-Fi), registers starting from `proxy_sel` can still be read in plaintext.
 - For BLE, when encryption is enabled, the first encrypted transaction must complete within 8 seconds after connection, or the link is terminated.


#### BLE Fragment Example:

Example 1:

 - BLE single transmission: 495 bytes (excluding 1-byte WHDR → 494 bytes payload)
 - Aggregate 8 transmissions for one large packet → encrypted data size: 494 × 8 = 3952 bytes
 - AES256 block size: 16 bytes → 3952 ÷ 16 = 247 blocks, fits exactly, no wasted bandwidth.
 - Due to PKCS#7 padding, the plaintext part is 1 byte smaller (if plaintext is a multiple of 16 bytes, padding adds 16 bytes).
 - With 2 bytes of A-CNT at the start, the plaintext size available for cdnet_pkt is:
   3952 - 1(WHDR) - 2(A-CNT) = 3949 bytes (If T-MAC is enabled, subtract 1 more byte.)
 - Each cdnet_pkt is up to 253 bytes: 3949 ÷ 253 = 15 × 253 + 154
 - Result: 15 full 253-byte packets + 1 final 154-byte packet → 16 cdnet_pkt in total.

Example 2:
 - BLE single transmission: 244 bytes, aggregate 16 transmissions for one large packet.
 - Encrypted size aligns with AES256 16-byte blocks.
 - Resulting cdnet_pkt division: 15 full 253-byte packets + 1 final 90-byte packet → 16 cdnet_pkt in total.


## Parameter Table

Parameter list:

<img src="doc/reg_list.avif" alt="Your browser may not support avif images!">

Note:
 - Parameters starting from ble_stop are not saved to flash.
 - Parameter table version: 0x0200


#### conf_ver

Address: 0x0002; Type: uint16_t

Description: Parameter table version information


#### do_reboot

Address: 0x0005; Type: uint8_t

Description: Write 2 to reboot the device


#### save_conf

Address: 0x0007; Type: uint8_t

Description: Write 1 to save parameters to flash; settings persist after reboot


#### k_en

Address: 0x0092; Type: uint8_t

Bits:
 - bit0: Enable BLE password protection when set
 - bit1: Enable UDP password protection when set

Note: This protocol does not use the built-in Bluetooth pairing or encryption; it relies on custom AES256 encryption.


#### k_pwd

Address: 0x0094; Type: char[24]

Description:  
Stores the password string, default "123456".  
Modify this register to change the password.


#### k_st_ble

Address: 0x0118; Type: uint8_t

Values:
 - 0: Password not verified
 - 1: Password verified (always 1 if password protection is disabled)


#### ble_itvl_min/max

Address: 0x00A6 / 0x00A7; Type: uint8_t

Description:

BLE connection parameters (connection interval).
 - Default: 6–12
 - Android: Larger intervals improve file transfer speed (recommended 6–36)
 - iOS: Smaller intervals improve file transfer speed (recommended 6–12)

Rules: Transmission window ≤ connection interval

Explanation:
 - On Android, longer intervals allow larger transmission windows, letting more data packets be sent per interval, improving throughput.
 - On iOS, the transmission window is fixed and small; reducing the connection interval allows more windows per unit time, compensating for the small window size.

Effect: Changes take effect without reconnection. Saving to flash sets the values as defaults for subsequent power-ups.


#### ble_stop

Address: 0x010A; Type: uint8_t

Values:
 - 1: Stop BLE advertising (improves Wi-Fi transmission speed when BLE is not connected)
 - 0: Resume BLE advertising


#### proxy_sel

Address: 0x0109; Type: uint8_t

Select the current proxy interface:
 - 1: BLE (default)
 - 2: Wi-Fi


#### wifi_ssid/pwd

Target Wi-Fi network information.  
Maximum length: SSID 32 bytes, Password 64 bytes.


#### wifi_conf

Address: 0x0108; Type: uint8_t

Values:
 - 0: Disconnect Wi-Fi (BLE transmission speed improves when Wi-Fi is disconnected)
 - 1: Connect to the specified Wi-Fi SSID in station mode


#### wifi_state

Address: 0x0148; Type: uint8_t

Bits:
 - bit0 = 1: Scanning SSID list
 - bit1 = 1: Wi-Fi connected
 - bit4 = 1: Attempting Wi-Fi connection
 - bit7 = 1: Wi-Fi disabled (power-up initialization state, normally ignored)
 - Other bits reserved (0)


#### remote_ip/port

 - remote_ip: 16-byte uint8_t[] array (raw IP data, not string)
 - remote_port: uint16_t port number

Default: remote_ip = ff...ff, first two bytes ff indicate invalid ip; remote_port = 0xFFFF indicates invalid port.

Behavior:
 - If remote_ip or remote_port is invalid:
   * When encryption is enabled, they are updated to the client’s IP and port after one encrypted communication.
   * When encryption is disabled, they are updated after one plaintext UDP communication.
 - Proxy responses are sent to the updated remote_ip/port.
 - Clients should check remote_ip/port before communication:
   * If invalid, or equal to the client’s own ip and port, CD-ESP is idle and ready to communicate.
   * After communication, the client can reset remote_ip/port to invalid to allow other clients to connect.
   * If remote_ip/port remain occupied by another client and the plaintext k_cnt_rx_udp register does not change for an extended period,
     it is reasonable to assume the connection has been terminated; in this case, remote_ip/port can be forcibly updated.


#### local_ip

IP addresses assigned to CD-ESP after connecting to the router:
 - First entry: IPv4 address
 - Second entry: IPv6 link-local address
 - Subsequent entries: other IPv6 addresses


#### scan_start

Write 1 to start Wi-Fi scanning


#### scan_auth/rssi/ssid

Results of Wi-Fi scanning


## Build Instructions

Based on IDF v6.0-beta1, run `source esp-idf/export.sh`, then execute `src/idf_patchs/patch_all.sh` once.  
After that, enter the `src` directory, run `idf.py set-target esp32c3` (only required the first time), and then execute `idf.py build`.

Firmware can be upgraded by:
 - Using the CDBUS GUI tool to perform RS-485 IAP with the HEX file in the build directory.
 - Running `tests/ble_ota.py` for OTA upgrade (or OTA via UDP).
 - Via the USB debug port.

After reboot, CD-ESP will automatically switch to the new firmware.
