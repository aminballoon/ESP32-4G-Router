# ESP32 4G/LTE to Ethernet NAT Router

Turn a **LilyGo T-Internet-COM** (ESP32 + SIM7600 4G/LTE modem + LAN8720 Ethernet PHY)
into a cellular **NAT router**: the board connects to the internet over 4G/LTE (PPP) and
shares that connection to LAN devices over Ethernet, acting as a DHCP server and gateway.

```
   ┌─────────────┐   4G/LTE (PPP)    ┌──────────────────┐   Ethernet (LAN)   ┌──────────┐
   │  AIS / SIM  │ ◄──────────────►  │  ESP32 (this fw) │ ◄───────────────►  │  Client  │
   │   Carrier   │   WAN: 10.x/32    │  NAT + DHCP + DNS │  192.168.4.1/24    │  PC/IoT  │
   └─────────────┘                   └──────────────────┘                    └──────────┘
```

---

## Hardware

| Component        | Detail                                              |
|------------------|-----------------------------------------------------|
| Board            | LilyGo T-Internet-COM (ESP32-D0WD)                   |
| Ethernet PHY     | LAN8720 (RMII)                                       |
| Cellular Modem   | SIM7600 (T-PCIE slot), PPP over UART1                |
| SIM              | Any data-enabled SIM (tested with AIS / Thailand)    |

### Pin map (defined in `src/main.cpp`)

| Function          | GPIO | | Function          | GPIO |
|-------------------|------|-|-------------------|------|
| ETH CLK (GPIO0)   | 0    | | Modem TX          | 33   |
| ETH Power         | 4    | | Modem RX          | 35   |
| ETH MDC           | 23   | | Modem PWRKEY/RST  | 32   |
| ETH MDIO          | 18   | | —                 | —    |
| ETH PHY Reset     | 5    | | —                 | —    |

---

## Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (CLI)
  or the PlatformIO IDE extension.
- USB cable to the board's serial/UART port.

This project uses the **pioarduino** ESP32 platform (Arduino core 3.x / ESP-IDF 5.x),
pinned in `platformio.ini`. PlatformIO downloads it automatically on first build.

### Configure your SIM (optional)

Edit the APN at the top of `src/main.cpp` to match your carrier:

```cpp
#define PPP_MODEM_APN "internet"   // e.g. "internet", "truemoveh", "www.dtac.co.th"
#define PPP_MODEM_PIN NULL         // SIM PIN, or NULL if none
```

---

## Build

Compile the firmware (no board required):

```bash
pio run
```

Output binaries land in `.pio/build/esp32dev/`.

## Burn (Flash / Upload)

Connect the board, then upload. Replace the port with your own
(find it with `pio device list` or `ls /dev/tty.usb*`):

```bash
pio run -t upload --upload-port /dev/tty.usbserial-576A0005291
```

If you omit `--upload-port`, PlatformIO auto-detects the board.

### Combined build + upload + monitor

```bash
pio run -t upload -t monitor --upload-port /dev/tty.usbserial-576A0005291
```

## Monitor (Serial debug)

Open the serial console at **115200 baud** to watch boot, modem diagnostics,
and the connectivity test:

```bash
pio device monitor --port /dev/tty.usbserial-576A0005291 --baud 115200
```

Expected healthy output:

```
[Diag] CPIN   : +CPIN: READY          <- SIM detected
[Diag] COPS   : +COPS: 0,0,"AIS",7     <- attached to carrier (LTE)
[PPP] Modem Got IP Address!  inet 10.x.x.x  gateway 10.64.64.64
[Route] Set PPP as default netif: OK
[NAT] NAPT successfully enabled on Ethernet interface!
[Net] Default route is PPP (correct)
[Ping] 8.8.8.8: 4 received, 0% packet loss   <- internet works
```

---

## Architecture

All logic lives in `src/main.cpp`. It is event-driven on top of the Arduino-ESP32
network stack (`ETH`, `PPP`, and the underlying `esp_netif` / lwIP).

### Boot sequence — `setup()`

1. **Ethernet PHY bring-up** — powers and resets the LAN8720, then `ETH.begin()`.
2. **Modem presence check** — reads the modem UART RX line and sends `AT` to detect
   whether the SIM7600 is already powered. Skips the power toggle if it is.
3. **Modem power & init** — pulses `PWRKEY` if needed, sets APN/PIN/pins, then
   `PPP.begin(SIM7600)`.
4. **`diagnose_modem()`** — queries the modem (still in command mode) and prints
   SIM status (`CPIN`, `ICCID`, `IMSI`), signal (`RSSI`), and carrier registration
   (`CREG`/`CEREG`/`CGATT`/`COPS`). This isolates **SIM** problems from **carrier** problems.
5. **Network attach** — waits up to 30 s for `PPP.attached()`, then switches the link
   to **CMUX** mode so data and AT commands share the UART.

### Event handler — `onEvent()`

Reacts to Arduino network events. The key one is **`ARDUINO_EVENT_PPP_GOT_IP`**, which:

1. **`esp_netif_set_default_netif(PPP)`** — forces the device's own egress traffic out
   the PPP (WAN) interface. *Without this, the Ethernet interface can win the default
   route and send internet-bound packets to a dead end — even `ping` to the cellular
   gateway fails. This is the fix that makes routing work.*
2. **`esp_netif_napt_enable(ETH)`** — enables NAPT on the LAN interface so client
   traffic is translated out over PPP.
3. Spawns **`connectivity_test()`** — a layered self-test: ping PPP gateway → raw TCP
   to `8.8.8.8:53` → DNS resolve → TCP via hostname → ICMP to `8.8.8.8`. This pinpoints
   *which* layer breaks if connectivity fails.

### LAN gateway — `start_dhcp_server()` + `loop()`

- The Ethernet interface is configured as a static gateway (`192.168.4.1/24`) running a
  **DHCP server** (lease range `192.168.4.10`–`192.168.4.99`, offering Google DNS).
- `loop()` acts as a **supervisor**: every 5 s it checks the DHCP server status and
  restarts it if the interface came up but the server isn't running.

### Why a ping can fail even when the link is up

| Symptom in logs                              | Likely cause                          |
|----------------------------------------------|---------------------------------------|
| `CPIN` not `READY` / empty `IMSI`            | SIM not seated / wrong orientation    |
| `RSSI: 99` or `-1`                           | No signal — check antenna             |
| `CEREG` stuck at `,2` (searching)            | Can't attach to carrier / bad APN     |
| Got IP but `Default route is NOT PPP`        | Routing/default-netif bug (fixed here)|
| TCP works but only ICMP to gateway fails     | Carrier blocks ICMP — harmless        |

---

## Project layout

```
.
├── platformio.ini        # Board, platform, build flags (CORE_DEBUG_LEVEL=4)
├── src/main.cpp          # All firmware logic
└── lib/Ethernet/         # Local ESP32 ETH library (LAN8720, DHCP-server capable)
```

## License

Add your license of choice here.
