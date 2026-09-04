#include "wii.h"

#include <stdlib.h>
#include <stdio.h>
#include <network.h>

// libogc's BSD socket() API requires the "soc:" device, which is only
// registered by socInit(). net_init() does NOT call it (only if_configex
// does), so we must register it ourselves or socket() fails with ENODEV.
extern int socInit(void);

void wii_net_init(void) {
  int rc = net_init();
  if (rc != 0) {
    printf("net_init failed: %d\n", rc);
  }
  else {
    printf("Network initialized\n");
    printf("socInit: %d\n", socInit());
    u32 ip = net_gethostip();
    if (ip) {
      struct in_addr a; a.s_addr = ip;
      struct in_addr b; b.s_addr = htonl(ip);
      printf("Wii IP: 0x%08x (net=%s host=%s)\n", ip, inet_ntoa(a), inet_ntoa(b));
    } else {
      printf("Wii IP: <none assigned>\n");
    }
    u8 mac[6];
    if (net_get_mac_address(mac) == 0) {
      printf("Wii MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
  }
}

void wii_net_shutdown(void) {
  net_deinit();
}
