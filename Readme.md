CD-ESP
=======================================

A CDBUS (RS-485) wireless bridge based on the ESP32-C3 / ESP32-C5, with BLE and Wi-Fi support
(dual-band 2.4 GHz + 5 GHz Wi-Fi on the ESP32-C5).


## Features

 - Transparent BLE and Wi-Fi (UDP) bridge to the RS-485 / CDBUS bus
 - Acts as a proxy to reach any node on the RS-485 bus, or the CD-ESP itself
 - Optional AES-256 encryption, independently switchable for BLE and UDP
 - mDNS discovery — reachable via the `cd-esp-xxxx.local` hostname
 - Fully configurable through a parameter table over BLE, RS-485, or UDP
 - Firmware upgrade via RS-485 IAP, BLE/UDP OTA, or the USB port


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

#### Advertising

The full device name is: `CD-ESP XXXX`

`XXXX` represents the two least-significant bytes of the device address in uppercase hex
(byte[1] followed by byte[0]).  
Example: `CD-ESP A1B2`

Manufacturer Specific Data: the full 6-byte device address.

#### Service & Characteristics

Service UUID: `b3340001-56ba-40b1-8ecb-8fe18dfffddd`

Characteristic RX:
 - UUID: `b3340002-56ba-40b1-8ecb-8fe18dfffddd`
 - Property: `write-no-response`

Characteristic TX:
 - UUID: `b3340003-56ba-40b1-8ecb-8fe18dfffddd`
 - Property: `notify`


### Wi-Fi Station

For initial setup, the device must be provisioned via BLE or RS-485.
Once the device has connected to the specified Wi-Fi access point,
its IP address can be queried via BLE or RS-485, or discovered through the local mDNS service;
the device can also be reached directly at the `cd-esp-xxxx.local` hostname (`xxxx`: same suffix as in the BLE name).

All subsequent communication uses UDP port 52685 (0xCDCD) on the device.


## Communication Diagram

<img src="doc/block_diagram.svg" width="100%">

 - (1): Any RS-485 node accesses the CD-ESP itself, for example to configure the network.
 - (2): The response to command (1), or a proactively reported data packet
        (e.g., the CD-ESP sending debug prints to the CDBUS GUI Tool).
 - (3)(5): Access to the CD-ESP itself via BLE or UDP, for example to configure the network or query status.
 - (4)(6): The responses to commands (3) and (5).
 - (7)(9): BLE or UDP access to any node on the RS-485 bus, with the CD-ESP acting as a proxy.
 - (8)(10): The responses to commands (7) and (9), or proactively reported data packets from any RS-485 node, forwarded by the CD-ESP.
 - (13)(14): The CD-ESP actively sends commands to any RS-485 node and receives the corresponding responses.


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
   * A-CNT: 2 bytes, AES-256 counter, optional.
   * T-MAC: 1 byte, target RS-485 node address, optional.

Recommendations:
 - BLE: maximum 244 or 495 bytes per transmission.
 - UDP: up to 5 CDNET packets per transmission.


#### Proxying

 - When bit5 of the CDNET temporary port is 0, the communication target is the CD-ESP itself.
 - When bit5 is 1, the packet is forwarded to the other end via the CD-ESP proxy.
 - Command/report packets from the RS-485 side are proxied by default when the target port is greater than 8.

When proxying is enabled:
 - If the target is the RS-485 node specified by the `p_mac` register, the T-MAC field is not included.
 - Otherwise, the T-MAC field is included to specify the target RS-485 node address.


### WHDR Definition

| FIELD   | DESCRIPTION                                 |
|-------- |---------------------------------------------|
| [7]     | Always 1 (indicates WHDR byte)              |
| [6]     | a_cnt_en                                    |
| [5]     | t_mac_en                                    |
| [4:3]   | frag_type (00: no fragment, 01: first, 10: continue, 11: last) |
| [2:0]   | frag_cnt (frag_type ≠ 0) or err_code (frag_type = 0)           |

For fragmented packets, the frag_cnt of the first fragment may be any value;
it is incremented by 1 for each subsequent fragment.


### Encryption and Fragmentation

<img src="doc/svg_out/_cdnet_wb_frag.svg" width="100%">

This diagram illustrates encryption with fragmentation enabled. The actual transmitted and received packets are (3)(4)(5).
The number of fragments depends on the total payload size and the fragment size.  
For example, when the T-MAC field is not enabled, the WHDR values of (3)(4)(5) are: 0b11001000, 0b11010001, 0b11011010.

If only encryption is enabled, the transmitted and received packet is (2).  
For example, when the T-MAC field is not enabled, the WHDR of (2) is: 0b11000000.

If only fragmentation is enabled, the fragmented data is unencrypted plaintext.  
In this case, the WHDR values of plaintext-carrying (3)(4)(5) are: 0b10001000, 0b10010001, 0b10011010.

When encryption or fragmentation is enabled:
 - On decryption failure, a single-byte WHDR packet is returned with err_code = 2 (bit[6:3] = 0).
 - On fragment reassembly failure, an error is reported after the last fragment is received, as a single-byte WHDR packet with err_code = 1 (bit[6:3] = 0).

Recommendations and limitations:
 - Fragmentation is recommended only when encryption is enabled over BLE.
 - UDP does not support fragmentation, as UDP packets are sufficiently large and do not require it.


#### AES-256 Encryption

When a packet requires encryption:
 - Enable encryption by setting `a_cnt_en` = 1. Then append 2 bytes of A-CNT after the WHDR
   (note: separate counters are maintained for send/receive and for BLE/Wi-Fi, totaling 4 counters).
 - Before communication, read the plaintext `k_random` (which changes on each power-up).  
   For example, if k_random = 0xabcd1234 and the default password string is "123456",
   the AES-256 key is derived by computing the SHA-256 of the string: `cd_abcd1234_123456`. The IV is fixed to all zeros.
 - Also read `k_cnt_rx_ble/udp` (defaults to 0 at startup). Upon receiving an encrypted packet, CD-ESP checks this counter;  
   if it doesn’t match, an error is reported; otherwise, the counter increments automatically.
 - For encrypted packets, only the 1-byte WHDR remains unencrypted; everything after WHDR is encrypted with AES-256-CBC using PKCS#7 padding.
 - When encryption is enabled (`k_en` bit0 for BLE; bit1 for Wi-Fi), registers starting from `proxy_sel` can still be read in plaintext.
 - For BLE, when encryption is enabled, the first encrypted transaction must complete within 8 seconds after connection, or the link is terminated.


#### BLE Fragment Examples

Example 1:

 - BLE single transmission: 495 bytes (excluding 1-byte WHDR → 494 bytes payload)
 - Aggregate 8 transmissions for one large packet → encrypted data size: 494 × 8 = 3952 bytes
 - AES-256 block size: 16 bytes → 3952 ÷ 16 = 247 blocks, fits exactly, no wasted bandwidth.
 - Due to PKCS#7 padding, the plaintext part is 1 byte smaller (if plaintext is a multiple of 16 bytes, padding adds 16 bytes).
 - With 2 bytes of A-CNT at the start, the plaintext size available for cdnet_pkt is:
   3952 - 1(pad) - 2(A-CNT) = 3949 bytes (If T-MAC is enabled, subtract 1 more byte.)
 - Each cdnet_pkt is up to 253 bytes: 3949 ÷ 253 = 15 × 253 + 154
 - Result: 15 full 253-byte packets + 1 final 154-byte packet → 16 cdnet_pkt in total.

Example 2:
 - BLE single transmission: 244 bytes, aggregate 16 transmissions for one large packet.
 - Encrypted size aligns with AES-256 16-byte blocks.
 - Resulting cdnet_pkt division: 15 full 253-byte packets + 1 final 90-byte packet → 16 cdnet_pkt in total.


## Parameter Table

Parameter table (read/write via CDNET port #05; `F`: retained after power-off, `!`: takes effect after reboot):

<table>
<tr> <th>Addr</th>   <th>Name</th>              <th>Attr</th>   <th>Type</th>   <th>Default</th>
     <th>Description</th>
</tr>
<tr> <td>0x0000</td> <td>magic_code</td>        <td>R/W</td>    <td>u16</td>    <td>0xcdcd</td>
     <td>Fixed value used to check whether the flash contains a valid register table</td>
</tr>
<tr> <td>0x0002</td> <td>conf_ver</td>          <td>R/W</td>    <td>u16</td>    <td>0x0201</td>
     <td>Register table version: high byte: major, low byte: minor</td>
</tr>
<tr> <td>0x0004</td> <td>conf_from</td>         <td>R</td>      <td>u8</td>     <td>0</td>
     <td>
        0: Default<br>
        1: Flash-stored table<br>
        2: Default from p_mac onwards (major version match only)
     </td>
</tr>
<tr> <td>0x0005</td> <td>do_reboot</td>         <td>R/W</td>    <td>u8</td>     <td>0</td>
     <td>Write 2: Normal reboot</td>
</tr>
<tr> <td>0x0007</td> <td>save_conf</td>         <td>R/W</td>    <td>u8</td>     <td>0</td>
     <td>Write 1: Save config to flash</td>
</tr>
<tr> <td>0x0008</td> <td>dbg_en</td>            <td>R/W/F</td>  <td>u8</td>     <td>0</td>
     <td>
        0: No debug print<br>
        1: Report debug print
     </td>
</tr>
<tr> <td>0x000c</td> <td>bus_mac</td>           <td>R/W/F!</td> <td>u8</td>     <td>0xfe</td>
     <td>Default serial address</td>
</tr>
<tr> <td>0x0010</td> <td>bus_baud_l</td>        <td>R/W/F!</td> <td>u32</td>    <td>115200</td>
     <td>Default baud rate for the first byte</td>
</tr>
<tr> <td>0x0014</td> <td>bus_baud_h</td>        <td>R/W/F!</td> <td>u32</td>    <td>115200</td>
     <td>Default baud rate for the following bytes</td>
</tr>
<tr> <td>0x0018</td> <td>bus_filter_m</td>      <td>R/W/F!</td> <td>u8[2]</td>  <td>0xff 0xff</td>
     <td>Multicast address filter</td>
</tr>
<tr> <td>0x001a</td> <td>bus_mode</td>          <td>R/W/F!</td> <td>u8</td>     <td>1</td>
     <td>
        0: Traditional half-duplex mode<br>
        1: Arbitration mode<br>
        2: BS mode
     </td>
</tr>
<tr> <td>0x001c</td> <td>bus_tx_permit_len</td> <td>R/W/F!</td> <td>u16</td>    <td>20</td>
     <td>
        Wait time before transmission is permitted (10 bits)<br>
        (Time unit: 1 bit duration)
     </td>
</tr>
<tr> <td>0x001e</td> <td>bus_max_idle_len</td>  <td>R/W/F!</td> <td>u16</td>    <td>200</td>
     <td>Maximum idle wait time in BS mode (10 bits)</td>
</tr>
<tr> <td>0x0020</td> <td>bus_tx_pre_len</td>    <td>R/W/F!</td> <td>u8</td>     <td>1</td>
     <td>
        Duration to assert TX_EN before TX output (2 bits)<br>
        (Ignored in Arbitration mode)
     </td>
</tr>
<tr> <td>0x0080</td> <td>p_mac</td>             <td>R/W/F</td>  <td>u8</td>     <td>0x10</td>
     <td>Predefined target MAC</td>
</tr>
<tr> <td>0x008d</td> <td>k_en</td>              <td>R/W/F</td>  <td>u8</td>     <td>0x02</td>
     <td>
        - bit0: 1 = Enable BLE encryption<br>
        - bit1: 1 = Enable UDP encryption
     </td>
</tr>
<tr> <td>0x008e</td> <td>k_pwd</td>             <td>R/W/F!</td> <td>c8[24]</td> <td>"123456"</td>
     <td>Password string</td>
</tr>
<tr> <td>0x00a6</td> <td>ble_itvl_min</td>      <td>R/W/F</td>  <td>u8</td>     <td>6</td>
     <td>BLE connection interval min</td>
</tr>
<tr> <td>0x00a7</td> <td>ble_itvl_max</td>      <td>R/W/F</td>  <td>u8</td>     <td>12</td>
     <td>BLE connection interval max</td>
</tr>
<tr> <td>0x00a8</td> <td>wifi_ssid</td>         <td>R/W/F</td>  <td>c8[32]</td> <td>""</td>
     <td>Target Wi-Fi SSID</td>
</tr>
<tr> <td>0x00c8</td> <td>wifi_pwd</td>          <td>R/W/F</td>  <td>c8[64]</td> <td>""</td>
     <td>Target Wi-Fi password</td>
</tr>
<tr> <td>0x0108</td> <td>wifi_conf</td>         <td>R/W/F</td>  <td>u8</td>     <td>0</td>
     <td>
        bit[3:0]:<br>
        - 0: Disconnect (improves BLE speed)<br>
        - 1: Station mode<br>
        bit[7:6] (band, dual-band chips like ESP32-C5 only):<br>
        - 0: Auto (2.4 GHz + 5 GHz)<br>
        - 1: 2.4 GHz only<br>
        - 2: 5 GHz only
     </td>
</tr>
<tr> <td>0x0109</td> <td>proxy_sel</td>         <td>R/W</td>    <td>u8</td>     <td>1</td>
     <td>1: BLE, 2: UDP</td>
</tr>
<tr> <td>0x010a</td> <td>ble_stop</td>          <td>R/W</td>    <td>u8</td>     <td>0</td>
     <td>1: Stop BLE advertising (improves Wi-Fi speed)</td>
</tr>
<tr> <td>0x0118</td> <td>k_st_ble</td>          <td>R</td>      <td>u8</td>     <td>0</td>
     <td>1: Password verified (always 1 if BLE encryption disabled)</td>
</tr>
<tr> <td>0x011c</td> <td>k_random</td>          <td>R</td>      <td>u32</td>    <td>--</td>
     <td>Used by AES-256 encryption</td>
</tr>
<tr> <td>0x0120</td> <td>k_cnt_rx_ble</td>      <td>R</td>      <td>u16</td>    <td>0</td>
     <td>Counter for BLE RX encryption</td>
</tr>
<tr> <td>0x0122</td> <td>k_cnt_tx_ble</td>      <td>R</td>      <td>u16</td>    <td>0</td>
     <td>Counter for BLE TX encryption</td>
</tr>
<tr> <td>0x0124</td> <td>k_cnt_rx_udp</td>      <td>R</td>      <td>u16</td>    <td>0</td>
     <td>Counter for UDP RX encryption</td>
</tr>
<tr> <td>0x0126</td> <td>k_cnt_tx_udp</td>      <td>R</td>      <td>u16</td>    <td>0</td>
     <td>Counter for UDP TX encryption</td>
</tr>
<tr> <td>0x0132</td> <td>ble_mtu_cur</td>       <td>R</td>      <td>u16</td>    <td>--</td>
     <td>BLE current connection MTU</td>
</tr>
<tr> <td>0x0134</td> <td>ble_itvl_cur</td>      <td>R</td>      <td>u8</td>     <td>--</td>
     <td>BLE current connection interval</td>
</tr>
<tr> <td>0x0148</td> <td>wifi_state</td>        <td>R</td>      <td>u8</td>     <td>0</td>
     <td>
        - bit0 = 1: Wi-Fi scan in progress<br>
        - bit1 = 1: Wi-Fi connected<br>
        - bit4 = 1: Attempting to connect to Wi-Fi
     </td>
</tr>
<tr> <td>0x0149</td> <td>remote_ip</td>         <td>R/W</td>    <td>u8[16]</td>  <td>ff...ff</td>
     <td>
        UDP client IP (starts with ffff = invalid)<br>
        Raw IP data (not string)
     </td>
</tr>
<tr> <td>0x015a</td> <td>remote_port</td>       <td>R/W</td>    <td>u16</td>     <td>0xffff</td>
     <td>UDP client port (0xffff = invalid)</td>
</tr>
<tr> <td>0x015c</td> <td>local_ip0</td>         <td>R</td>      <td>u8[16]</td>  <td>ff...ff</td>
     <td>IPv4 address</td>
</tr>
<tr> <td>0x016c</td> <td>local_ip1</td>         <td>R</td>      <td>u8[16]</td>  <td>ff...ff</td>
     <td>IPv6 link-local</td>
</tr>
<tr> <td>0x017c</td> <td>local_ip2</td>         <td>R</td>      <td>u8[16]</td>  <td>ff...ff</td>
     <td>IPv6 global / other</td>
</tr>
<tr> <td>0x018c</td> <td>local_ip3</td>         <td>R</td>      <td>u8[16]</td>  <td>ff...ff</td>
     <td>IPv6 global / other</td>
</tr>
<tr> <td>0x019c</td> <td>scan_start</td>        <td>R/W</td>    <td>u8</td>      <td>0</td>
     <td>1: Start Wi-Fi scanning</td>
</tr>
<tr> <td>0x019d</td> <td>scan_auth</td>         <td>R</td>      <td>u8[20]</td>  <td>00...00</td>
     <td>
        Scan result auth mode:<br>
        0: Open<br>
        1: WEP<br>
        2: WPA_PSK<br>
        3: WPA2_PSK<br>
        4: WPA_WPA2_PSK<br>
        ...
     </td>
</tr>
<tr> <td>0x01b1</td> <td>scan_rssi</td>         <td>R</td>      <td>i8[20]</td>  <td>7f...7f</td>
     <td>Scan result RSSI</td>
</tr>
<tr> <td>0x01c5</td> <td>scan_ssid0</td>        <td>R</td>      <td>c8[32]</td>  <td>""</td>
     <td>Scan result SSID</td>
</tr>
<tr> <td>0x01e5</td> <td>scan_ssid1</td>        <td>R</td>      <td>c8[32]</td>  <td>""</td>
     <td>Scan result SSID</td>
</tr>
<tr> <td>...</td>    <td>...</td>               <td>...</td>    <td>...</td>     <td>...</td>
     <td>...</td>
</tr>
<tr> <td>0x0425</td> <td>scan_ssid19</td>       <td>R</td>      <td>c8[32]</td>  <td>""</td>
     <td>Scan result SSID</td>
</tr>
</table>



#### BLE Connection Interval Suggestions

BLE connection interval range:
 - Default: 6–12
 - Android: Larger intervals allow higher throughput (recommended 6–36)
 - iOS: Smaller intervals allow higher throughput (recommended 6–12)

Rule: transmission window ≤ connection interval

Explanation:
 - On Android, longer intervals allow larger transmission windows, letting more data packets be sent per interval, improving throughput.
 - On iOS, the transmission window is fixed and small; reducing the connection interval allows more windows per unit time, compensating for the small window size.

Note: changes take effect immediately, without reconnecting.



#### Remote IP / Port

 - If remote_ip or remote_port is invalid:
   * When encryption is enabled, they are updated to the client’s IP and port upon the next encrypted communication.
   * When encryption is disabled, they are updated upon the next plaintext UDP communication.
 - Proxy responses are sent to the updated remote_ip/port.
 - Clients should check remote_ip/port before communication:
   * If invalid, or equal to the client’s own IP and port, the CD-ESP is idle and ready to communicate.
   * After communication, the client can reset remote_ip/port to invalid to allow other clients to connect.
   * If remote_ip/port remain occupied by another client and the plaintext k_cnt_rx_udp register does not change for an extended period,
     it is reasonable to assume the connection has been terminated; in this case, remote_ip/port can be forcibly updated.



## CDBUS GUI Tool

<img src="doc/reg_list.avif" alt="Your browser may not support avif images!">



## Build Instructions

This project is based on ESP-IDF v6.0.2. Run `source esp-idf/export.sh`, then execute `src/idf_patchs/patch_all.sh` once.  
After that, enter the `src` directory, run `idf.py set-target esp32c3` (or `esp32c5`; only required the first time),
and then run `idf.py build`. The firmware automatically adapts to the selected target;
the wifi_conf band bits take effect only on dual-band chips (ESP32-C5).

The firmware can be upgraded in any of the following ways:
 - RS-485 IAP using the CDBUS GUI Tool with the HEX file from the build directory.
 - OTA via BLE (run `tests/ble_ota.py`) or via UDP.
 - Through the USB debug port.

After rebooting, the CD-ESP automatically switches to the new firmware.
