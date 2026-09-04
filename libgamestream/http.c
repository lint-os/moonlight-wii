/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

// Minimal HTTP/HTTPS client using libogc sockets and mbedtls,
// replacing libcurl which is not available on the toolchain.
// All GameStream requests are GETs with query strings.

#include "http.h"
#include "errors.h"
#include "set_error.h"
#include "tls_rng.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <network.h>
#include <sys/iosupport.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>

#ifdef __WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

struct HTTP_T {
    char certPath[4096];
    char keyPath[4096];
    int timeout;
};

// Convert a public socket handle (from socket()) to the internal net index
// that net_write/net_recv/net_connect expect. Same logic as libogc's
// soc_get_fd and libwiisocket's __wiisocket_get_native_fd.
static int to_net_fd(int fd) {
    __handle* h = __get_handle(fd);
    if (h == NULL)
        return -1;
    if (strcmp(devoptab_list[h->device]->name, "soc") != 0)
        return -1;
    return *(s32*) h->fileStruct;
}

static long tlsSendBytes = 0, tlsRecvBytes = 0;
static int tlsSendSeq = 0, tlsRecvSeq = 0;

// Hex-dump the first 32 bytes of a TLS record so we can compare the handshake
// byte-for-byte between Dolphin (works) and real Wii (fails).
static void tls_hexdump(const char* tag, const unsigned char* buf, int n) {
    int i;
    printf("[tls] %s #%d len=%d: ", tag, (tag[0] == 's' ? ++tlsSendSeq : ++tlsRecvSeq), n);
    int show = n < 32 ? n : 32;
    for (i = 0; i < show; i++)
        printf("%02x", buf[i]);
    if (n > 32)
        printf("...");
    printf("\n");
}

// BIO callbacks call net_write/net_recv directly (not send/recv). libogc's
// send() wraps net_send() which takes a flags arg and behaves differently on
// real HW; net_write() is the path the known-working xkcd-wii TLS uses.
static int fd_send(void* ctx, const unsigned char* buf, size_t len) {
    int nfd = to_net_fd((int)(intptr_t) ctx);
    if (nfd < 0)
        return -1;
    tls_hexdump("send", buf, (int) len);
    int n = net_write(nfd, (const void*) buf, (s32) len);
    if (n > 0)
        tlsSendBytes += n;
    else
        printf("[tls] send n=%d errno=%d total_sent=%ld\n", n, errno, tlsSendBytes);
    return n;
}

static int fd_recv(void* ctx, unsigned char* buf, size_t len) {
    int nfd = to_net_fd((int)(intptr_t) ctx);
    if (nfd < 0)
        return -1;
    int n = net_recv(nfd, (void*) buf, (s32) len, 0);
    if (n > 0) {
        tlsRecvBytes += n;
        tls_hexdump("recv", buf, n);
        return n;
    }
    printf("[tls] recv n=%d errno=%d total_recv=%ld\n", n, errno, tlsRecvBytes);
    if (n == 0)
        return -1;
    return n;
}

static int append_data(HTTP_DATA* data, const char* buf, size_t len) {
    void* allocated = realloc(data->memory, data->size + len + 1);
    if (allocated == NULL)
        return -1;
    data->memory = allocated;
    memcpy(data->memory + data->size, buf, len);
    data->size += len;
    data->memory[data->size] = 0;
    return 0;
}

static void strip_headers(HTTP_DATA* data) {
    if (data->memory == NULL)
        return;
    char* mem = (char*) data->memory;
    char* sep = strstr(mem, "\r\n\r\n");
    size_t skip;
    if (sep != NULL)
        skip = (size_t)(sep + 4 - mem);
    else {
        sep = strstr(mem, "\n\n");
        if (sep == NULL)
            return;
        skip = (size_t)(sep + 2 - mem);
    }
    size_t bodyLen = data->size - skip;
    memmove(mem, mem + skip, bodyLen + 1);
    data->size = bodyLen;
}

static int connect_host(const char* host, int port, int timeout) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("[http] socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);

    // libogc's gethostbyname only does DNS (no dotted-quad), so resolve an IP
    // literal with inet_aton first; fall back to DNS for hostnames.
    if (inet_aton(host, &addr.sin_addr) == 0) {
        struct hostent* he = gethostbyname(host);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            printf("[http] DNS resolution failed for \"%s\"\n", host);
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    char ipbuf[64];
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    printf("[http] connecting to %s (%s) port %d\n", host, ipbuf, port);

    // Plain blocking connect: it does the 3-way handshake internally and
    // returns as soon as it's done (a few ms on a LAN). The non-blocking +
    // select() pattern is what burned a full 10s on the first real-HW
    // connect -- net_select does not return promptly when the connect
    // completes, it rides the whole timeout -- so don't use it. (An
    // unreachable host now relies on the stack's own connect timeout rather
    // than the `timeout` arg.)
    if (connect(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        printf("[http] connect failed (errno=%d: %s)\n", errno, strerror(errno));
        close(fd);
        return -1;
    }

    printf("[http] TCP connected\n");
    return fd;
}

static int parse_url(const char* url, int* isHttps, char* host, size_t hostSize, int* port, char* path, size_t pathSize) {
    const char* p = url;

    if (strncmp(p, "https://", 8) == 0) {
        *isHttps = 1;
        p += 8;
    }
    else if (strncmp(p, "http://", 7) == 0) {
        *isHttps = 0;
        p += 7;
    }
    else {
        return -1;
    }

    const char* hostEnd = strchr(p, '/');
    const char* colon = strchr(p, ':');
    size_t hostLen;

    if (colon != NULL && (hostEnd == NULL || colon < hostEnd)) {
        hostLen = colon - p;
        *port = atoi(colon + 1);
    }
    else {
        hostLen = hostEnd != NULL ? hostEnd - p : strlen(p);
        *port = *isHttps ? 443 : 80;
    }

    if (hostLen >= hostSize)
        return -1;
    memcpy(host, p, hostLen);
    host[hostLen] = 0;

    const char* pathStart = hostEnd != NULL ? hostEnd : p + hostLen;
    if (strlen(pathStart) >= pathSize)
        return -1;
    strcpy(path, pathStart);

    return 0;
}

static int send_request(int fd, const char* host, const char* path) {
    char request[8192];
    int requestLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Moonlight/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        path, host);
    if (requestLen < 0)
        return -1;
    {
        char preview[256];
        int n = requestLen < 255 ? requestLen : 255;
        memcpy(preview, request, n);
        preview[n] = 0;
        printf("[http] request (%d bytes): %s\n", requestLen, preview);
    }

    int sent = 0;
    while (sent < requestLen) {
        int r = send(fd, request + sent, requestLen - sent, 0);
        if (r <= 0) {
            printf("[http] send failed at %d/%d (errno=%d: %s)\n", sent, requestLen, errno, strerror(errno));
            return -1;
        }
        sent += r;
        printf("[http] sent %d/%d bytes\n", sent, requestLen);
    }
    printf("[http] send complete (%d bytes)\n", sent);
    return 0;
}

// Parse the Content-Length header (case-insensitive) from a header block.
// Returns the value, or -1 if the header is absent.
static long parse_content_length(const char* headers, size_t len) {
    for (size_t i = 0; i + 14 <= len; i++) {
        static const char pat[] = "content-length";
        int match = 1;
        for (size_t j = 0; j < sizeof(pat) - 1; j++) {
            char c = headers[i + j];
            if (c >= 'A' && c <= 'Z')
                c = (char) (c + 32);
            if (c != pat[j]) {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;
        const char* p = headers + i + 14;
        while (*p == ' ' || *p == '\t' || *p == ':')
            p++;
        return strtol(p, NULL, 10);
    }
    return -1;
}

// True once the full response (headers + body) has been received. With a
// Content-Length we stop as soon as that many body bytes are in; otherwise we
// rely on the server closing the connection (EOF).
static int response_complete(const HTTP_DATA* data) {
    if (data->memory == NULL)
        return 0;
    const char* mem = (const char*) data->memory;
    const char* sep = strstr(mem, "\r\n\r\n");
    size_t headerLen;
    if (sep != NULL)
        headerLen = (size_t) (sep + 4 - mem);
    else {
        sep = strstr(mem, "\n\n");
        if (sep == NULL)
            return 0;
        headerLen = (size_t) (sep + 2 - mem);
    }
    long cl = parse_content_length(mem, headerLen);
    if (cl < 0)
        return 0;
    return (data->size - headerLen) >= (size_t) cl;
}

static int read_response(int fd, HTTP_DATA* data, int timeout) {
    char buf[4096];

    // Reset the buffer so a response never accumulates on top of the previous
    // request's response (the same HTTP_DATA is reused for all pair requests).
    if (data->memory != NULL) {
        free(data->memory);
        data->memory = NULL;
    }
    data->size = 0;

    printf("[http] waiting for response (timeout=%ds)\n", timeout);
    for (;;) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        struct timeval tv;
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        int sr = select(fd + 1, &rset, NULL, NULL, &tv);
        if (sr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (sr == 0) {
            printf("[http] response timeout (no data in %ds)\n", timeout);
            break;
        }

        int r = recv(fd, buf, sizeof(buf), 0);
        if (r > 0) {
            if (append_data(data, buf, r) != 0) {
                printf("[http] out of memory appending response\n");
                break;
            }
            printf("[http] recv %d bytes (total %d)\n", r, (int) data->size);
            if (response_complete(data))
                break;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        printf("[http] recv returned %d (errno=%d), treating as EOF\n", r, errno);
        break; // EOF or error
    }

    strip_headers(data);
    return data->size > 0 ? 0 : -1;
}

static int tls_request(int fd, const char* host, const char* path,
                      const char* certPath, const char* keyPath, HTTP_DATA* data) {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pkey;
    mbedtls_ctr_drbg_context drbg;
    int ret = -1;

    tlsSendBytes = 0;
    tlsRecvBytes = 0;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&pkey);
    mbedtls_ctr_drbg_init(&drbg);

    if (mbedtls_ctr_drbg_seed(&drbg, tls_rng, NULL, NULL, 0) != 0)
        goto finish;

    int certRet = mbedtls_x509_crt_parse_file(&cert, certPath);
    if (certRet != 0) {
        printf("[http] cert parse_file failed ret=-0x%04x path=%s\n", (unsigned)(-certRet), certPath);
        goto finish;
    }
    printf("[http] cert parsed OK\n");

    int keyRet = mbedtls_pk_parse_keyfile(&pkey, keyPath, NULL, mbedtls_ctr_drbg_random, &drbg);
    if (keyRet != 0) {
        printf("[http] key parse_file failed ret=-0x%04x path=%s\n", (unsigned)(-keyRet), keyPath);
        goto finish;
    }
    printf("[http] key parsed OK\n");

    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
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

    int setupRet = mbedtls_ssl_setup(&ssl, &conf);
    if (setupRet != 0) {
        printf("[http] ssl_setup failed: -0x%04x\n", (unsigned)(-setupRet));
        goto finish;
    }

    mbedtls_ssl_set_bio(&ssl, (void*) (intptr_t) fd, fd_send, fd_recv, NULL);
    mbedtls_ssl_set_hostname(&ssl, host);

    printf("[http] TLS handshake start\n");
    int handshakeResult;
    do {
        handshakeResult = mbedtls_ssl_handshake(&ssl);
    } while (handshakeResult == MBEDTLS_ERR_SSL_WANT_READ || handshakeResult == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (handshakeResult != 0) {
        printf("[http] TLS handshake failed: -0x%04x\n", (unsigned)(-handshakeResult));
        goto finish;
    }
    printf("[http] TLS handshake OK\n");

    char request[4096];
    int requestLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, host);
    if (requestLen < 0)
        goto finish;

    int sent = 0;
    while (sent < requestLen) {
        int r = mbedtls_ssl_write(&ssl, (const unsigned char*) request + sent, requestLen - sent);
        if (r <= 0)
            goto finish;
        sent += r;
    }

    // Reset the buffer so a response never accumulates on top of the previous
    // request's response (the same HTTP_DATA is reused for all pair requests).
    if (data->memory != NULL) {
        free(data->memory);
        data->memory = NULL;
    }
    data->size = 0;

    char buf[4096];
    for (;;) {
        int r = mbedtls_ssl_read(&ssl, (unsigned char*) buf, sizeof(buf));
        if (r > 0) {
            if (append_data(data, buf, r) != 0)
                goto finish;
            continue;
        }
        break;
    }
    strip_headers(data);

    ret = 0;
finish:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cert);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&drbg);
    return ret;
}

HTTP* http_create(const char* keydir) {
    HTTP* http = calloc(1, sizeof(struct HTTP_T));
    if (http == NULL)
        return NULL;

    snprintf(http->certPath, sizeof(http->certPath),
        "%s" PATH_SEPARATOR "%s", keydir, CERTIFICATE_FILE_NAME);
    snprintf(http->keyPath, sizeof(http->keyPath),
        "%s" PATH_SEPARATOR "%s", keydir, KEY_FILE_NAME);
    http->timeout = 10;

    return http;
}

int http_request(HTTP* http, char* url, HTTP_DATA* data) {
    int isHttps = 0;
    char host[256];
    int port = 0;
    char path[4096];

    if (parse_url(url, &isHttps, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        printf("[http] bad url: %s\n", url);
        return -1;
    }

    int fd = connect_host(host, port, http->timeout);
    if (fd < 0)
        return -1;

    int ret;
    if (isHttps)
        ret = tls_request(fd, host, path, http->certPath, http->keyPath, data);
    else
        ret = send_request(fd, host, path) == 0 ? read_response(fd, data, http->timeout) : -1;

    close(fd);
    if (data->size > 0)
        printf("[http] raw response (%s %s:%d, %d bytes):\n---\n%s\n---\n",
               isHttps ? "https" : "http", host, port, (int) data->size, (const char*) data->memory);
    if (ret != 0)
        printf("[http] request failed (%s %s:%d, %d bytes)\n",
                isHttps ? "https" : "http", host, port, (int) data->size);
    return ret;
}

void http_destroy(HTTP* http) {
    free(http);
}

void http_set_timeout(HTTP* http, int timeout) {
    http->timeout = timeout;
}

HTTP_DATA* http_data_alloc() {
    HTTP_DATA* data = calloc(1, sizeof(HTTP_DATA));
    if (data != NULL)
        data->memory = NULL;
    return data;
}

void http_data_free(HTTP_DATA* data) {
    if (data != NULL) {
        free(data->memory);
        free(data);
    }
}
