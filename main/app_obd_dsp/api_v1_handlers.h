#pragma once
/*
 * api_v1_handlers.h — /api/v1/... device API (device web page)
 *
 * Hosted by ota_wifi_server on the OTA SoftAP. Every request needs the
 * session X-OTA-Token (checked through the auth callback). Only the vehicle
 * install/select/reboot POST allowlist exposes CORS; other endpoints are
 * same-origin only for browsers. Errors are
 *   {"ok":false,"error":"<code>","detail":"..."}
 * with a matching HTTP status. Endpoint list: docs/APP_PROTOCOL.md.
 */

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when the request carries a valid session token. */
typedef bool (*api_v1_auth_fn)(httpd_req_t *req);

/* Register the "/api/v1/" wildcard dispatcher (GET/POST/PUT/DELETE; 4 URI slots).
 * The server must use httpd_uri_match_wildcard. */
esp_err_t api_v1_register_handlers(httpd_handle_t server, api_v1_auth_fn auth);

/* Drop any half-finished /api/v1 upload (buffers, .part files). Called when an
 * /ota upload starts and when the server stops. */
void api_v1_reset(void);
/* HTTP task only: maintenance must not erase a partially uploaded resource. */
bool api_v1_upload_busy(void);

#ifdef __cplusplus
}
#endif
