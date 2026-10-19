# net

The bridge's Wi-Fi (bridge spec 9): its home network, its setup network,
the setup network's DNS answers, the DNS-SD advertisement and the clock.

| File | What it does | Portable |
|------|--------------|----------|
| `include/net_machine.h`, `net_machine.c` | The Wi-Fi states and their rules: when to join, when to show the setup network and when to turn it off, and when a join has failed. | yes |
| `include/captive_dns.h`, `captive_dns.c` | Answers every DNS question on the setup network with the bridge, so phones open the setup page. | yes |
| `include/net.h`, `net.c` | Applies the state machine to the Wi-Fi driver on ESP-IDF's event task, keeps the home network's credentials, runs mDNS, DNS-SD and SNTP. | no |

Public API: `net_start()`, `net_advertise()`, `net_get_status()`,
`net_set_hostname()`, `net_scan()`, `net_join()`, `net_finish_setup()`,
`net_socket_is_on_setup_network()`, `net_setup_address()`; the machine's
`net_machine_*()` functions; `captive_dns_answer()`.

Tests: `test/host/test_net.c`.
