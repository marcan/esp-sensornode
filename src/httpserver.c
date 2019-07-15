#include <string.h>

#include "mem.h"
#include "lwip/tcp.h"
#include "cont.h"

#include "httpserver.h"

#define HTTP_MAX_LINE_SIZE 512

#define HTTP_MAX_HANDLERS 5

struct httpconn {
    struct httpserver *hs;

    int used;
    struct tcp_pcb *tcpb;

    int eof;
    struct pbuf *recv_data;
    size_t recv_off;

    char request[HTTP_MAX_LINE_SIZE];
    int no_more_headers;

    cont_t cont;
    int exited;
};

struct httphandler {
    const char *path;
    http_handler_t func;
};

struct httpserver {
    struct tcp_pcb *listener;
    int port;
    int maxconns;
    httpconn_t *conns;
    int handler_count;
    struct httphandler handlers[HTTP_MAX_HANDLERS];
};

ICACHE_FLASH_ATTR
static void httpserver_handle_client(void *arg);

ICACHE_FLASH_ATTR
static void httpserver_run_client(httpconn_t *conn)
{
//     os_printf("cont_run %08x\n", (uint32_t)conn);
    if (conn->exited) {
        os_printf("refusing to run dead connection %08x!\n", (uint32_t)conn);
        return;
    }
    cont_run(&conn->cont, httpserver_handle_client, conn);
//     os_printf("cont_run returned\n");
}

ICACHE_FLASH_ATTR
static int httpserver_readline(httpconn_t *conn, char *buf, size_t size)
{
    char *p = buf;
    char *end = p + size - 1;
    while (p < end)
    {
        while (!conn->recv_data && !conn->eof) {
//             os_printf("yield (read)\n");
            cont_yield(&conn->cont);
//             os_printf("yield ret\n");
        }
        if (conn->eof) {
            break;
        }
        size_t l = pbuf_copy_partial(conn->recv_data, p, end - p, conn->recv_off);
//         os_printf("copied %d/%d bytes @%d\n", l, conn->recv_data->tot_len, conn->recv_off);
        for (int i = 0; i < l; i++) {
            if (p[i] == '\n') {
                p[i] = '\0';
                if ((i > 0 || p > buf) && p[i - 1] == '\r')
                    p[i - 1] = '\0';
                conn->recv_off += i + 1;
//                 os_printf("recved: %d\n", i);
                tcp_recved(conn->tcpb, i + 1);
                if (conn->recv_off >= conn->recv_data->tot_len) {
                    pbuf_free(conn->recv_data);
                    conn->recv_data = NULL;
                    conn->recv_off = 0;
                }
                return p - buf;
            }
        }
        conn->recv_off += l;
//         os_printf("recved: %d\n", l);
        tcp_recved(conn->tcpb, l);
        p += l;
        if (conn->recv_off >= conn->recv_data->tot_len) {
            pbuf_free(conn->recv_data);
            conn->recv_data = NULL;
            conn->recv_off = 0;
        }
    }
    p[0] = '\0';
    return p - buf + 1;
}

ICACHE_FLASH_ATTR
int httpserver_write_data(httpconn_t *conn, const void *data, size_t length)
{
    err_t ret;
    const uint8_t *p = data;

    while (length) {
        size_t block = length;
        size_t sendq = tcp_sndbuf(conn->tcpb);
        if (block > sendq)
            block = sendq;
//         os_printf("tcp_write: '%s'\n", p);
        ret = tcp_write(conn->tcpb, p, length, TCP_WRITE_FLAG_COPY);
        if (ret == ERR_MEM) {
            tcp_output(conn->tcpb);
//             os_printf("yield (write)\n");
            cont_yield(&conn->cont);
            continue;
        } else if (ret != ERR_OK) {
            os_printf("tcp_write failed: %d\n", ret);
            return p - (const uint8_t*)data;
        }
        length -= block;
        p += block;
    }
    return p - (const uint8_t*)data;
}

ICACHE_FLASH_ATTR
int httpserver_write_string(httpconn_t *conn, const char *data)
{
    return httpserver_write_data(conn, data, strlen(data));
}

ICACHE_FLASH_ATTR
int httpserver_read_header(httpconn_t *conn, char **header, char **value)
{
    if (conn->no_more_headers)
        return 0;

    httpserver_readline(conn, conn->request, sizeof(conn->request));

    if (!conn->request[0]) {
        conn->no_more_headers = 1;
        return 0;
    }

    char *phdr = conn->request;
    char *pval = strchr(phdr, ':');

    if (pval) {
        *pval++ = '\0';
        if (*pval == ' ')
            *pval++ = '\0';
    }

    if (header)
        *header = phdr;
    if (value)
        *value = pval;

//     os_printf("HDR: %s='%s'\n", phdr, pval);
    return 1;
}

ICACHE_FLASH_ATTR
int httpserver_end_request(httpconn_t *conn)
{
    while (httpserver_read_header(conn, NULL, NULL));
    return 0;
}

ICACHE_FLASH_ATTR
int httpserver_start_response(httpconn_t *conn, int code, const char *text)
{
    char buf[32];
    os_sprintf(buf, "HTTP/1.1 %d ", code);
    httpserver_write_string(conn, buf);
    httpserver_write_string(conn, text);
    httpserver_write_string(conn, "\r\nConnection: close\r\n");
    return 0;
}

ICACHE_FLASH_ATTR
int httpserver_send_header(httpconn_t *conn, const char *header, const char *value)
{
    httpserver_write_string(conn, header);
    httpserver_write_string(conn, ": ");
    httpserver_write_string(conn, value);
    httpserver_write_string(conn, "\r\n");
    return 0;
}

ICACHE_FLASH_ATTR
int httpserver_end_headers(httpconn_t *conn)
{
    httpserver_write_string(conn, "\r\n");
}

ICACHE_FLASH_ATTR
void httpserver_handle_404(httpconn_t *conn, const char *path, char *query_string)
{
    httpserver_end_request(conn);
    httpserver_start_response(conn, 404, "Not Found");
    httpserver_send_header(conn, "Content-Type", "text/plain");
    httpserver_end_headers(conn);
    httpserver_write_string(conn, "No handler at specified URL");
}

ICACHE_FLASH_ATTR
static void httpserver_handle_client(void *arg)
{
    httpconn_t *conn = arg;
//     os_printf("httpserver_handle_client %08x\n", (uint32_t)arg);

    httpserver_readline(conn, conn->request, sizeof(conn->request));
    os_printf("> %s\n", conn->request);

    char *method = strtok(conn->request, " ");
    char *path = strtok(NULL, " ");
    char *version = strtok(NULL, " ");

    if (!method || !path || !version || !*method || !*path || !*version)
        goto cleanup;

//     os_printf("method: '%s' path: '%s' version: '%s'\n", method, path, version);

    if (strcmp(method, "GET")) {
        // POST not supported yet
        httpserver_end_request(conn);
        httpserver_start_response(conn, 405, "Method Not Allowed");
        httpserver_end_headers(conn);
        goto cleanup;
    }

    char *qs = strchr(path, '?');
    if (qs) {
        *qs++ = '\0';
    }

//     os_printf("path: '%s' query: '%s'\n", path, qs);

    httpserver_t *hs = conn->hs;

    for (int i = 0; i < hs->handler_count; i++) {
        if (!strcmp(path, hs->handlers[i].path)) {
            hs->handlers[i].func(conn, path, qs);
            goto cleanup;
        }
    }

    httpserver_handle_404(conn, path, qs);

cleanup:
    tcp_output(conn->tcpb);
    conn->exited = 1;
    conn->used = 0;
    if (tcp_close(conn->tcpb) == ERR_OK) {
        conn->used = 0;
    } else {
        os_printf("tcp_close failed\n");
    }
    tcp_arg(conn->tcpb, NULL);
//     os_printf("httpserver_handle_client returning\n");
}

ICACHE_FLASH_ATTR
static err_t httpserver_recv(void *arg, struct tcp_pcb *tcpb, struct pbuf *p, err_t err)
{
    httpconn_t *conn = arg;
//     os_printf("httpserver_recv %08x\n", (uint32_t)conn);
    if (!conn) {
        if (p || err != ERR_OK)
            os_printf("zombie httpserver_recv: %p, %d\n", p, err);
        return ERR_OK;
    }
    if (!p) {
        conn->eof = 1;
        httpserver_run_client(conn);
        return ERR_OK;
    }
    if (conn->recv_data) {
        os_printf("httpserver_recv queue busy, refusing data\n");
        return ERR_MEM;
    }
    conn->recv_data = p;
    httpserver_run_client(conn);
    return ERR_OK;
}

ICACHE_FLASH_ATTR
static err_t httpserver_sent(void *arg, struct tcp_pcb *tcpb, u16_t len)
{
    httpconn_t *conn = arg;
    if (!conn)
        return ERR_OK;
    os_printf("httpserver_sent %08x\n", (uint32_t)conn);
    httpserver_run_client(conn);
    return ERR_OK;
}

ICACHE_FLASH_ATTR
static void httpserver_err(void *arg, err_t err)
{
    httpconn_t *conn = arg;
    os_printf("httpserver_err: error %d\n", err);
    if (conn) {
        conn->exited = 1;
        conn->used = 0;
    }
}

ICACHE_FLASH_ATTR
static err_t httpserver_accept(void *arg, struct tcp_pcb *tcpb, err_t err)
{
    httpserver_t *hs = arg;
    httpconn_t *conn = NULL;

//     os_printf("accept\n");

    for (int i = 0; i < hs->maxconns; i++) {
        if (!hs->conns[i].used) {
            conn = &hs->conns[i];
            break;
        }
    }

    if (!conn) {
        os_printf("httpserver: too many connections\n");
        return ERR_MEM;
    }

    memset(conn, 0, sizeof(*conn));
    conn->used = 1;
    conn->hs = hs;
    conn->tcpb = tcpb;
    tcp_arg(tcpb, conn);
    tcp_recv(tcpb, httpserver_recv);
    tcp_sent(tcpb, httpserver_sent);
    tcp_err(tcpb, httpserver_err);
    cont_init(&conn->cont);
    httpserver_run_client(conn);

    return ERR_OK;
}

ICACHE_FLASH_ATTR
int httpserver_route(httpserver_t *hs, const char *path, http_handler_t handler)
{
    if (hs->handler_count >= HTTP_MAX_HANDLERS)
        return -1;

    hs->handlers[hs->handler_count].path = path;
    hs->handlers[hs->handler_count].func = handler;
    hs->handler_count++;
}

ICACHE_FLASH_ATTR
httpserver_t * httpserver_init(int port, int maxconns)
{
    httpserver_t *hs = os_malloc(sizeof(httpserver_t));
    memset(hs, 0, sizeof(*hs));

    os_printf("httpserver_init\n");
    if (!hs) {
        os_printf("Alloc httpserver_t failed\n");
        return NULL;
    }
    hs->listener = tcp_new();
    if (!hs->listener) {
        os_printf("Alloc httpserver_t failed\n");
    }
    hs->port = port;
    hs->maxconns = maxconns;
    hs->conns = os_malloc(sizeof(httpconn_t) * maxconns);
    memset(hs->conns, 0, sizeof(httpconn_t) * maxconns);
    if (!hs->conns) {
        os_printf("Alloc %d x httpconn_t failed\n", maxconns);
        return NULL;
    }
    if (tcp_bind(hs->listener, IP_ADDR_ANY, hs->port) != ERR_OK) {
        os_printf("tcp_bind failed\n");
    }
    return hs;
}

ICACHE_FLASH_ATTR
int httpserver_start(httpserver_t *hs)
{
    struct tcp_pcb *p = tcp_listen(hs->listener);
    if (!p) {
        os_printf("tcp_listen failed\n");
        return -1;
    }
    hs->listener = p;
    tcp_arg(hs->listener, hs);
    tcp_accept(hs->listener, httpserver_accept);
    os_printf("httpserver_start\n");
    return 0;
}
