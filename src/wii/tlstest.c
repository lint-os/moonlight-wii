// Startup TLS self-test. Connects to a local test server
// (pc-test/basic_server.py) twice: once without a client cert and once
// with, to isolate whether the client-cert path is what breaks on real
// hardware. Enabled via the "tls_test = ip:port" config option.
#include "wii.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <network.h>
#include <sys/iosupport.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#include "tls_rng.h"
#include "http.h"

// High-res timing: timebase ticks relative to probe start, so we can see
// exactly which TLS record (and whether the gap is client-side compute or
// network/server-side) accounts for the seconds a handshake takes.
static u64 probe_t0;
static int tsend_seq, trecv_seq;

static long long now_ms(void) {
  return (long long) ticks_to_millisecs(gettime() - probe_t0);
}

static int to_net_fd(int fd) {
  __handle* h = __get_handle(fd);
  if (h == NULL)
    return -1;
  if (strcmp(devoptab_list[h->device]->name, "soc") != 0)
    return -1;
  return *(s32*) h->fileStruct;
}

static int t_send(void* ctx, const unsigned char* buf, size_t len) {
  int nfd = to_net_fd((int)(intptr_t) ctx);
  if (nfd < 0)
    return -1;
  int n = net_write(nfd, (const void*) buf, (s32) len);
  printf("[tlstest] t_send #%d at +%lldms len=%d -> %d\n", ++tsend_seq, now_ms(), (int) len, n);
  return n;
}

static int t_recv(void* ctx, unsigned char* buf, size_t len) {
  int nfd = to_net_fd((int)(intptr_t) ctx);
  if (nfd < 0)
    return -1;
  int n = net_recv(nfd, (void*) buf, (s32) len, 0);
  if (n > 0)
    printf("[tlstest] t_recv #%d at +%lldms len=%d -> %d\n", ++trecv_seq, now_ms(), (int) len, n);
  if (n == 0)
    return -1;
  return n;
}

static int connect_tcp(const char* host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short) port);
  if (inet_aton(host, &addr.sin_addr) == 0) {
    struct hostent* he = gethostbyname(host);
    if (he == NULL || he->h_addr_list[0] == NULL) {
      close(fd);
      return -1;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
  }

  // Plain blocking connect: it does the 3-way handshake internally and
  // returns as soon as it's done. The non-blocking + select() pattern above
  // is what burns a full 10s on the first real-HW connect (net_select does
  // not return promptly when the connect completes), so don't use it.
  u64 c0 = gettime();
  int ret = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
  u64 c1 = gettime();
  printf("[tlstest] connect() took %lldms ret=%d\n", (long long) ticks_to_millisecs(c1 - c0), ret);
  if (ret < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static int probe(const char* host, int port, int with_cert, const char* keydir) {
  char errbuf[128];
  char path[4096];
  int ret = -1;

  probe_t0 = gettime();
  tsend_seq = trecv_seq = 0;

  int fd = connect_tcp(host, port);
  if (fd < 0) {
    printf("[tlstest] TCP connect failed\n");
    return -1;
  }
  printf("[tlstest] TCP connected at +%lldms\n", now_ms());

  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt cert;
  mbedtls_pk_context pkey;

  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_x509_crt_init(&cert);
  mbedtls_pk_init(&pkey);

  if (mbedtls_ctr_drbg_seed(&drbg, tls_rng, NULL, NULL, 0) != 0) {
    printf("[tlstest] drbg seed failed\n");
    goto finish;
  }

  if (with_cert) {
    snprintf(path, sizeof(path), "%s/%s", keydir, CERTIFICATE_FILE_NAME);
    if (mbedtls_x509_crt_parse_file(&cert, path) != 0) {
      printf("[tlstest] cert load failed: %s\n", path);
      goto finish;
    }
    snprintf(path, sizeof(path), "%s/%s", keydir, KEY_FILE_NAME);
    if (mbedtls_pk_parse_keyfile(&pkey, path, NULL, mbedtls_ctr_drbg_random, &drbg) != 0) {
      printf("[tlstest] key load failed: %s\n", path);
      goto finish;
    }
  }

  mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  if (with_cert)
    mbedtls_ssl_conf_own_cert(&conf, &cert, &pkey);
  mbedtls_ssl_conf_max_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  static const int ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_DHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_DHE_RSA_WITH_AES_256_GCM_SHA384,
    0
  };
  mbedtls_ssl_conf_ciphersuites(&conf, ciphersuites);

  if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
    printf("[tlstest] ssl_setup failed\n");
    goto finish;
  }
  mbedtls_ssl_set_bio(&ssl, (void*) (intptr_t) fd, t_send, t_recv, NULL);

  int hs;
  do {
    hs = mbedtls_ssl_handshake(&ssl);
  } while (hs == MBEDTLS_ERR_SSL_WANT_READ || hs == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (hs != 0) {
    mbedtls_strerror(hs, errbuf, sizeof(errbuf));
    printf("[tlstest] handshake FAILED at +%lldms: -0x%04x (%s)\n", now_ms(), (unsigned)(-hs), errbuf);
    goto finish;
  }
  printf("[tlstest] handshake OK at +%lldms, cipher=%s\n", now_ms(), mbedtls_ssl_get_ciphersuite(&ssl));

  const char* req = "GET /tlstest HTTP/1.1\r\nHost: moonlight-test\r\nConnection: close\r\n\r\n";
  int wr = mbedtls_ssl_write(&ssl, (const unsigned char*) req, strlen(req));
  printf("[tlstest] sent request at +%lldms (%d bytes)\n", now_ms(), wr);

  fd_set rset;
  FD_ZERO(&rset);
  FD_SET(fd, &rset);
  struct timeval tv = { 10, 0 };
  if (select(fd + 1, &rset, NULL, NULL, &tv) <= 0) {
    printf("[tlstest] no response within 10s\n");
    goto finish;
  }

  char rbuf[1024];
  int rr = mbedtls_ssl_read(&ssl, (unsigned char*) rbuf, sizeof(rbuf) - 1);
  if (rr > 0) {
    rbuf[rr] = 0;
    printf("[tlstest] server response at +%lldms (%d bytes): %s\n", now_ms(), rr, rbuf);
    ret = 0;
  } else {
    mbedtls_strerror(rr, errbuf, sizeof(errbuf));
    printf("[tlstest] read failed at +%lldms: -0x%04x (%s)\n", now_ms(), (unsigned)(-rr), errbuf);
  }

  mbedtls_ssl_close_notify(&ssl);

finish:
  close(fd);
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_x509_crt_free(&cert);
  mbedtls_pk_free(&pkey);
  return ret;
}

void wii_tls_test(const char* target, const char* keydir) {
  char host[64];
  int port = 47984;
  const char* colon = strchr(target, ':');
  size_t hlen = colon ? (size_t)(colon - target) : strlen(target);
  if (hlen >= sizeof(host))
    hlen = sizeof(host) - 1;
  memcpy(host, target, hlen);
  host[hlen] = 0;
  if (colon)
    port = atoi(colon + 1);
  if (port <= 0)
    port = 47984;

  printf("[tlstest] === startup TLS test vs %s:%d ===\n", host, port);

  Font_Clear();
  Font_SetColor(255, 255, 255, 255);
  Font_SetSize(24);
  Font_Print(8, 20, "TLS test (no client cert)...");
  Font_Draw_TVDRC();
  int r1 = probe(host, port, 0, keydir);

  Font_Clear();
  Font_SetColor(255, 255, 255, 255);
  Font_SetSize(24);
  Font_Print(8, 20, "TLS test (with client cert)...");
  Font_Draw_TVDRC();
  int r2 = probe(host, port, 1, keydir);

  char msg[256];
  snprintf(msg, sizeof(msg), "TLS test vs %s:%d\nno-cert:   %s\nwith-cert: %s",
           host, port, r1 == 0 ? "OK" : "FAIL", r2 == 0 ? "OK" : "FAIL");
  printf("[tlstest] %s\n", msg);

  Font_Clear();
  if (r1 == 0 && r2 == 0)
    Font_SetColor(120, 255, 120, 255);
  else
    Font_SetColor(255, 120, 120, 255);
  Font_SetSize(24);
  Font_Print(8, 20, msg);
  Font_SetColor(150, 150, 150, 255);
  Font_SetSize(18);
  Font_Print(8, 140, "A: continue");
  Font_Draw_TVDRC();

  while (wii_proc_running()) {
    if (wii_input_buttons_triggered() & (WPAD_BUTTON_A | WPAD_BUTTON_B))
      break;
    if (wii_proc_want_main_menu())
      break;
    usleep(10000);
  }
}
