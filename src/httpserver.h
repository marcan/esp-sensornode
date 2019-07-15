
struct httpserver;
typedef struct httpserver httpserver_t;

struct httpconn;
typedef struct httpconn httpconn_t;

httpserver_t *httpserver_init(int port, int maxconns);

typedef void (*http_handler_t)(httpconn_t *conn, char *path, char *query_string);
int httpserver_route(httpserver_t *hs, const char *path, http_handler_t handler);

int httpserver_read_header(httpconn_t *conn, char **header, char **value);
int httpserver_end_request(httpconn_t *conn);
int httpserver_start_response(httpconn_t *conn, int code, const char *text);
int httpserver_send_header(httpconn_t *conn, const char *header, const char *value);
int httpserver_end_headers(httpconn_t *conn);
int httpserver_write_data(httpconn_t *conn, const void *data, size_t length);
int httpserver_write_string(httpconn_t *conn, const char *data);

int httpserver_start(httpserver_t *hs);
