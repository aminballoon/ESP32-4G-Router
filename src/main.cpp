#include <Arduino.h>
#include <ETH.h>
#include <PPP.h>
#include <NetworkClient.h>
#include <esp_netif.h>
#include <dhcpserver/dhcpserver.h>
#include <dhcpserver/dhcpserver_options.h>
#include <ping/ping_sock.h>
extern "C" esp_err_t esp_netif_up(esp_netif_t *esp_netif);

// PIN Definitions for LilyGo T-Internet-COM Ethernet (LAN8720)
#undef ETH_CLK_MODE
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_OUT
#define ETH_POWER_PIN   4
#define ETH_TYPE        ETH_PHY_LAN8720
#define ETH_ADDR        0
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18
#define NRST            5

// PIN Definitions for LilyGo T-Internet-COM 4G/LTE Modem (T-PCIE)
#define PPP_MODEM_TX      33
#define PPP_MODEM_RX      35
#define PPP_MODEM_PWRKEY  32
#define PPP_MODEM_RST     PPP_MODEM_PWRKEY
#define PPP_MODEM_RST_LOW false // active HIGH
#define PPP_MODEM_RST_DELAY 200
#define PPP_MODEM_MODEL   PPP_MODEM_SIM7600 // SIM7600 is compatible with T-PCIE 4G modules

// Cellular APN (Edit this according to your cellular provider, e.g., "internet", "truemoveh", "www.dtac.co.th")
#define PPP_MODEM_APN "internet" 
#define PPP_MODEM_PIN NULL       // SIM PIN (usually NULL or "0000")

// Ping Callbacks
void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint8_t ttl;
    uint32_t elapsed_time, recv_len, seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    Serial.printf("[Ping] %d bytes from %s: icmp_seq=%d ttl=%d time=%d ms\n",
                  recv_len, ipaddr_ntoa(&target_addr), seqno, ttl, elapsed_time);
}

void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    uint32_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    Serial.printf("[Ping] Request timeout from %s: icmp_seq=%d\n", ipaddr_ntoa(&target_addr), seqno);
}

void on_ping_end(esp_ping_handle_t hdl, void *args) {
    uint32_t transmitted, received, total_time;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time, sizeof(total_time));
    uint32_t loss = transmitted ? ((transmitted - received) * 100 / transmitted) : 0;
    Serial.printf("[Ping] %d packets transmitted, %d received, %d%% packet loss, time %d ms\n",
                  transmitted, received, loss, total_time);
    
    // Delete session after it ends to prevent memory leak
    esp_ping_delete_session(hdl);
}

void trigger_ping(IPAddress target_ip) {
    Serial.printf("[Ping] Pinging %s...\n", target_ip.toString().c_str());
    
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    
    ip_addr_t target_addr;
    target_addr.type = IPADDR_TYPE_V4;
    target_addr.u_addr.ip4.addr = static_cast<uint32_t>(target_ip);
    ping_config.target_addr = target_addr;
    ping_config.count = 4; // Ping 4 times
    
    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = on_ping_success;
    callbacks.on_ping_timeout = on_ping_timeout;
    callbacks.on_ping_end = on_ping_end;
    callbacks.cb_args = NULL;
    
    esp_ping_handle_t ping_handle;
    esp_err_t err = esp_ping_new_session(&ping_config, &callbacks, &ping_handle);
    if (err == ESP_OK) {
        esp_ping_start(ping_handle);
    } else {
        Serial.printf("[Ping] Failed to create ping session: 0x%x\n", err);
    }
}

// Test connectivity layer-by-layer so we can tell WHERE it breaks:
// cellular L3 (gateway ping) -> raw TCP uplink -> DNS -> TCP via hostname -> ICMP to 8.8.8.8.
// Many carriers block ICMP to public IPs even when TCP/UDP data works perfectly,
// so a ping timeout alone does NOT mean the internet is down.
void connectivity_test() {
    Serial.println("---------- CONNECTIVITY TEST ----------");
    Serial.print("[Net] PPP IP     : "); Serial.println(PPP.localIP());
    Serial.print("[Net] PPP Gateway: "); Serial.println(PPP.gatewayIP());
    Serial.print("[Net] PPP DNS    : "); Serial.println(PPP.dnsIP());

    // Confirm which interface is the default route for device traffic
    {
        esp_netif_t *def = esp_netif_get_default_netif();
        esp_netif_t *ppp_netif = PPP.netif();
        Serial.printf("[Net] Default route is %s\n",
                      (def == ppp_netif) ? "PPP (correct)" : "NOT PPP (wrong!)");
    }

    // 1. Ping the PPP gateway: confirms the cellular link itself carries ICMP at all.
    Serial.println("[Test] (1/4) Pinging PPP gateway...");
    trigger_ping(PPP.gatewayIP());
    delay(6000);

    // 2. Raw TCP to 8.8.8.8:53 (no DNS needed): pure internet uplink over TCP.
    {
        NetworkClient client;
        Serial.println("[Test] (2/4) TCP connect 8.8.8.8:53 (no DNS)...");
        if (client.connect(IPAddress(8, 8, 8, 8), 53, 8000)) {
            Serial.println("[TCP]  -> SUCCESS: internet uplink works over TCP!");
            client.stop();
        } else {
            Serial.println("[TCP]  -> FAILED: no TCP uplink. Check APN / data plan.");
        }
    }

    // 3. DNS resolution: tests UDP uplink + the carrier DNS server.
    Serial.println("[Test] (3/4) DNS resolve google.com...");
    {
        IPAddress resolved;
        if (Network.hostByName("google.com", resolved)) {
            Serial.print("[DNS]  -> SUCCESS: google.com = "); Serial.println(resolved);
        } else {
            Serial.println("[DNS]  -> FAILED: DNS not working (UDP blocked or bad DNS).");
        }
    }

    // 4. TCP via hostname: DNS + TCP end-to-end, the real-world path.
    {
        NetworkClient client;
        Serial.println("[Test] (4/4) TCP connect google.com:80...");
        if (client.connect("google.com", 80, 8000)) {
            Serial.println("[TCP]  -> SUCCESS: full internet path (DNS+TCP) works!");
            client.stop();
        } else {
            Serial.println("[TCP]  -> FAILED: DNS+TCP path broken.");
        }
    }

    // 5. ICMP to public IP: likely blocked by carrier even if the above succeed.
    Serial.println("[Test] ICMP ping 8.8.8.8 (may be blocked by carrier)...");
    trigger_ping(IPAddress(8, 8, 8, 8));
    Serial.println("---------------------------------------");
}

void onEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        // Ethernet Events
        case ARDUINO_EVENT_ETH_START:
            Serial.println("[ETH] Ethernet Started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] Ethernet Link Connected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.print("[ETH] Ethernet Got IP: ");
            Serial.println(ETH.localIP());
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("[ETH] Ethernet Link Disconnected");
            break;
        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("[ETH] Ethernet Stopped");
            break;

        // PPP (Cellular) Events
        case ARDUINO_EVENT_PPP_START:
            Serial.println("[PPP] Modem Interface Started");
            break;
        case ARDUINO_EVENT_PPP_CONNECTED:
            Serial.println("[PPP] Modem Connected (Link Up)");
            break;
        case ARDUINO_EVENT_PPP_GOT_IP:
            Serial.println("[PPP] Modem Got IP Address!");
            Serial.println(PPP);

            // Force PPP to be the default route for the device's OWN egress traffic.
            // With both ETH (LAN, 192.168.4.1) and PPP up, ETH can win the default
            // route and send internet-bound packets to a dead end. This was why even
            // pinging the PPP gateway failed despite the link being up.
            {
                esp_netif_t *ppp_netif = PPP.netif();
                esp_netif_t *eth_netif = ETH.netif();
                if (ppp_netif != NULL && eth_netif != NULL) {
                    esp_err_t rerr = esp_netif_set_default_netif(ppp_netif);
                    Serial.printf("[Route] Set PPP as default netif: %s\n",
                                  rerr == ESP_OK ? "OK" : "FAILED");
                    
                    // Enable NAPT on the LAN (Ethernet) interface to share the PPP internet to LAN
                    esp_err_t err = esp_netif_napt_enable(eth_netif);
                    if (err == ESP_OK) {
                        Serial.println("[NAT] NAPT successfully enabled on Ethernet interface!");
                    } else {
                        Serial.printf("[NAT] Failed to enable NAPT: %d\n", err);
                    }
                } else {
                    Serial.println("[Route/NAT] Error: PPP or Ethernet netif handle is NULL!");
                }
            }
            
            // Run the layered connectivity test after 3s to let routing stabilize
            xTaskCreate([](void *arg) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                connectivity_test();
                vTaskDelete(NULL);
            }, "conn_test", 8192, NULL, 5, NULL);
            break;
        case ARDUINO_EVENT_PPP_LOST_IP:
            Serial.println("[PPP] Modem Lost IP Address!");
            break;
        case ARDUINO_EVENT_PPP_DISCONNECTED:
            Serial.println("[PPP] Modem Disconnected");
            break;
        case ARDUINO_EVENT_PPP_STOP:
            Serial.println("[PPP] Modem Interface Stopped");
            break;

        default:
            break;
    }
}

bool start_dhcp_server(esp_netif_t *eth_netif, IPAddress local_ip, IPAddress gateway, IPAddress subnet) {
    if (eth_netif == NULL) {
        return false;
    }
    
    // Stop DHCP client
    esp_netif_dhcpc_stop(eth_netif);
    
    // Stop DHCP server
    esp_netif_dhcps_stop(eth_netif);
    
    // Set static IP info
    esp_netif_ip_info_t info;
    info.ip.addr = static_cast<uint32_t>(local_ip);
    info.gw.addr = static_cast<uint32_t>(gateway);
    info.netmask.addr = static_cast<uint32_t>(subnet);
    
    esp_err_t err = esp_netif_set_ip_info(eth_netif, &info);
    if (err != ESP_OK) {
        Serial.printf("[DHCP] esp_netif_set_ip_info failed: 0x%x\n", err);
        return false;
    }
    
    // Bring the network interface UP to prevent ESP_ERR_ESP_NETIF_IF_NOT_READY (0x5002)
    err = esp_netif_up(eth_netif);
    if (err != ESP_OK) {
        Serial.printf("[DHCP] esp_netif_up warning/failed: 0x%x\n", err);
    }
    
    // Configure DHCP server lease options
    dhcps_lease_t lease;
    lease.enable = true;
    
    // Set lease range from local_ip + 10 to local_ip + 100
    uint8_t ip_bytes[4];
    ip_bytes[0] = local_ip[0];
    ip_bytes[1] = local_ip[1];
    ip_bytes[2] = local_ip[2];
    
    IPAddress start_ip(ip_bytes[0], ip_bytes[1], ip_bytes[2], 10);
    IPAddress end_ip(ip_bytes[0], ip_bytes[1], ip_bytes[2], 99);
    
    lease.start_ip.addr = static_cast<uint32_t>(start_ip);
    lease.end_ip.addr = static_cast<uint32_t>(end_ip);
    
    Serial.printf("[DHCP] Setting lease range: %s - %s\n", start_ip.toString().c_str(), end_ip.toString().c_str());
    
    err = esp_netif_dhcps_option(eth_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(dhcps_lease_t));
    if (err != ESP_OK) {
        Serial.printf("[DHCP] esp_netif_dhcps_option failed: 0x%x\n", err);
        return false;
    }
    
    // Offer DNS to clients
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    err = esp_netif_dhcps_option(eth_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));
    if (err != ESP_OK) {
        Serial.printf("[DHCP] Set OFFER_DNS option failed: 0x%x\n", err);
    }
    
    esp_netif_dns_info_t dns_info;
    dns_info.ip.type = IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(IPAddress(8, 8, 8, 8)); // Offer Google DNS
    err = esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (err != ESP_OK) {
        Serial.printf("[DHCP] esp_netif_set_dns_info failed: 0x%x\n", err);
    }
    
    // Start DHCP server
    err = esp_netif_dhcps_start(eth_netif);
    if (err != ESP_OK) {
        Serial.printf("[DHCP] esp_netif_dhcps_start failed: 0x%x\n", err);
        return false;
    }
    
    Serial.println("[DHCP] DHCP Server started successfully.");
    return true;
}

// Interpret +CREG/+CEREG/+CGREG <stat> code into a readable string
const char *reg_status_str(int stat) {
    switch (stat) {
        case 0: return "Not registered, NOT searching (radio off / no SIM?)";
        case 1: return "Registered on HOME network (OK)";
        case 2: return "Not registered, SEARCHING for carrier...";
        case 3: return "Registration DENIED by carrier";
        case 4: return "Unknown";
        case 5: return "Registered, ROAMING (OK)";
        default: return "Unexpected status";
    }
}

// Parse the <stat> field out of a "+CxREG: <n>,<stat>..." response and print it
void print_reg_stat(const String &resp, const char *tag, const char *label) {
    int idx = resp.indexOf(tag);
    if (idx == -1) return;
    int comma = resp.indexOf(',', idx);
    if (comma == -1) return;
    int stat = resp.substring(comma + 1, comma + 2).toInt();
    Serial.printf("[Diag]   -> %s: %s\n", label, reg_status_str(stat));
}

// Query the modem (in command mode, after PPP.begin()) and print SIM + carrier status.
// Use this to tell apart "SIM not detected" from "cannot attach to carrier".
void diagnose_modem() {
    Serial.println("---------- MODEM DIAGNOSTICS ----------");

    // 1. Does the modem respond to AT at all?
    if (!PPP.sync()) {
        Serial.println("[Diag] Modem does NOT respond to AT. Check wiring/power/baud rate!");
        Serial.println("---------------------------------------");
        return;
    }
    Serial.println("[Diag] Modem responds to AT (sync OK).");

    // 2. Modem identity
    Serial.printf("[Diag] Module : %s\n", PPP.moduleName().c_str());
    Serial.printf("[Diag] IMEI   : %s\n", PPP.IMEI().c_str());

    // 3. SIM card presence / status
    String cpin;
    PPP.cmd("AT+CPIN?", cpin, 3000);
    cpin.trim();
    Serial.printf("[Diag] CPIN   : %s\n", cpin.c_str());
    if (cpin.indexOf("READY") != -1) {
        Serial.println("[Diag]   -> SIM detected and READY.");
    } else if (cpin.indexOf("SIM PIN") != -1) {
        Serial.println("[Diag]   -> SIM is LOCKED, PIN required! Set PPP_MODEM_PIN.");
    } else {
        Serial.println("[Diag]   -> SIM NOT detected / error. Check SIM seating & orientation.");
    }

    String iccid;
    PPP.cmd("AT+CICCID", iccid, 3000);  // SIM7600 reports ICCID with +CICCID
    iccid.trim();
    Serial.printf("[Diag] ICCID  : %s\n", iccid.c_str());
    Serial.printf("[Diag] IMSI   : %s  (empty = SIM not readable)\n", PPP.IMSI().c_str());

    // 4. Signal quality
    int rssi = PPP.RSSI();
    Serial.printf("[Diag] RSSI   : %d  (0-31 valid, 99/-1 = no signal)\n", rssi);
    if (rssi == 99 || rssi < 0) {
        Serial.println("[Diag]   -> NO signal. Check antenna connection!");
    } else if (rssi < 10) {
        Serial.println("[Diag]   -> WEAK signal.");
    }

    // 5. Carrier registration (the part that fails when you can't reach the carrier)
    String creg, cereg, cgatt, cops;
    PPP.cmd("AT+CREG?",  creg,  3000); creg.trim();
    PPP.cmd("AT+CEREG?", cereg, 3000); cereg.trim();  // LTE/4G (EPS) registration
    PPP.cmd("AT+CGATT?", cgatt, 3000); cgatt.trim();
    PPP.cmd("AT+COPS?",  cops,  3000); cops.trim();
    Serial.printf("[Diag] CREG   : %s\n", creg.c_str());
    Serial.printf("[Diag] CEREG  : %s\n", cereg.c_str());
    Serial.printf("[Diag] CGATT  : %s  (1 = packet domain attached)\n", cgatt.c_str());
    Serial.printf("[Diag] COPS   : %s\n", cops.c_str());
    print_reg_stat(creg,  "+CREG:",  "2G/3G registration");
    print_reg_stat(cereg, "+CEREG:", "LTE registration");

    Serial.printf("[Diag] Operator: %s\n", PPP.operatorName().c_str());
    Serial.printf("[Diag] NetMode : %d  (radioState=%d)\n", PPP.networkMode(), PPP.radioState());
    Serial.printf("[Diag] Attached: %s\n", PPP.attached() ? "YES" : "NO");
    Serial.println("---------------------------------------");
}

#include <lwip/opt.h>

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    delay(1000);
    Serial.println("============================================");
    Serial.println("   ESP32 4G/LTE to Ethernet NAT Router      ");
    Serial.println("============================================");
    
#ifndef IP_FORWARD
#define IP_FORWARD -1
#endif
#ifndef IP_NAPT
#define IP_NAPT -1
#endif
    Serial.printf("[LwIP] IP_FORWARD compile-time macro: %d\n", IP_FORWARD);
    Serial.printf("[LwIP] IP_NAPT compile-time macro: %d\n", IP_NAPT);


    // Register Network Events Listener
    Network.onEvent(onEvent);

    // 1. Initialize, Power up and Reset Ethernet PHY
    Serial.println("[ETH] Powering up and resetting Ethernet PHY...");
    pinMode(ETH_POWER_PIN, OUTPUT);
    digitalWrite(ETH_POWER_PIN, HIGH);
    delay(100);

    pinMode(NRST, OUTPUT);
    digitalWrite(NRST, LOW);
    delay(200);
    digitalWrite(NRST, HIGH);
    delay(200);

    // 2. Start Ethernet (passing NRST for official reset management)
    Serial.println("[ETH] Starting Ethernet interface...");
    if (!ETH.begin(ETH_TYPE, ETH_ADDR, ETH_MDC_PIN, ETH_MDIO_PIN, NRST, ETH_CLK_MODE)) {
        Serial.println("[ETH] ETH.begin failed!");
    }
    delay(2000);

    // 3. Configure Ethernet as Gateway (Static IP & DHCP Server flags)
    // The Ethernet netif is now natively created as a DHCP Server by our local Ethernet library.
    esp_netif_t *eth_netif = ETH.netif();
    if (eth_netif == NULL) {
        Serial.println("[DHCP] Error: Cannot find Ethernet network interface!");
    }

    // Test if modem is already on
    Serial.println("[Modem] Checking if modem is already online...");
    pinMode(PPP_MODEM_RX, INPUT);
    delay(100);
    int rx_val = digitalRead(PPP_MODEM_RX);
    Serial.printf("[Modem] RX pin digital level: %d\n", rx_val);
    
    bool modem_on = false;
    if (rx_val == HIGH) {
        Serial.println("[Modem] RX pin is HIGH. Modem might be on. Checking UART response...");
        Serial1.begin(115200, SERIAL_8N1, PPP_MODEM_RX, PPP_MODEM_TX);
        // Send escape sequence in case the modem is in data mode
        delay(500);
        Serial1.print("+++");
        delay(500);
        while (Serial1.available()) {
            Serial1.read();
        }
        
        for (int i = 0; i < 3; i++) {
            Serial1.println("AT");
            delay(200);
            if (Serial1.available()) {
                String resp = Serial1.readString();
                if (resp.indexOf("OK") != -1) {
                    modem_on = true;
                    break;
                }
            }
        }
        Serial1.end();
    } else {
        Serial.println("[Modem] RX pin is LOW. Modem is powered off.");
    }

    // 4. Configure and Start 4G/LTE Modem
    Serial.println("[Modem] Setting up pins and APN...");
    PPP.setApn(PPP_MODEM_APN);
    PPP.setPin(PPP_MODEM_PIN);
    // Call PPP.setPins AFTER Serial1.end() to prevent detaching pins
    PPP.setPins(PPP_MODEM_TX, PPP_MODEM_RX, -1, -1, ESP_MODEM_FLOW_CONTROL_NONE);
    
    if (modem_on) {
        Serial.println("[Modem] Modem is already online. Skipping power toggle.");
        PPP.setResetPin(-1); 
    } else {
        Serial.println("[Modem] Modem is offline. Powering it up manually...");
        pinMode(PPP_MODEM_PWRKEY, OUTPUT);
        digitalWrite(PPP_MODEM_PWRKEY, LOW);
        delay(100);
        digitalWrite(PPP_MODEM_PWRKEY, HIGH);
        delay(1000); // Pulse HIGH for 1 second
        digitalWrite(PPP_MODEM_PWRKEY, LOW);
        Serial.println("[Modem] Waiting 10 seconds for modem to boot up...");
        delay(10000); // Wait 10 seconds for SIM7600 to boot up
        PPP.setResetPin(-1); // PPP doesn't need to reset it again
    }

    Serial.println("[Modem] Initializing PPP modem connection...");
    PPP.begin(PPP_MODEM_MODEL);

    // Diagnose SIM + carrier state right after init, while still in command mode
    diagnose_modem();

    // Wait for network attach
    bool attached = PPP.attached();
    int retry = 0;
    while (!attached && retry++ < 30) {
        Serial.print(".");
        delay(1000);
        attached = PPP.attached();
    }
    Serial.println("");

    if (attached) {
        Serial.printf("[Modem] Attached to network. Operator: %s, RSSI: %d\n", PPP.operatorName().c_str(), PPP.RSSI());
        Serial.println("[Modem] Switching to Mixed Data & Command Mode (CMUX)...");
        PPP.mode(ESP_MODEM_MODE_CMUX);
    } else {
        Serial.println("[Modem] Failed to attach to network. Check SIM card and antenna!");
        // Re-run diagnostics to capture the final SIM/carrier state after the retries
        Serial.println("[Modem] Re-running diagnostics to identify the cause...");
        diagnose_modem();
    }
}

void loop() {
    // DHCP Server Supervisor
    esp_netif_t *eth_netif = ETH.netif();
    if (eth_netif != NULL) {
        esp_netif_dhcp_status_t status;
        esp_err_t err = esp_netif_dhcps_get_status(eth_netif, &status);
        if (err == ESP_OK && status != ESP_NETIF_DHCP_STARTED) {
            if (esp_netif_is_netif_up(eth_netif)) {
                Serial.println("[DHCP] Netif is UP but DHCP Server is not running. Starting DHCPS...");
                if (start_dhcp_server(eth_netif, IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0))) {
                    Serial.println("[DHCP] DHCP Server supervisor started DHCPS successfully.");
                } else {
                    Serial.println("[DHCP] DHCP Server supervisor failed to start DHCPS!");
                }
            }
        }
    }
    delay(5000);
}
