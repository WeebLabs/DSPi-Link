/*
 * setup_api.h - what the bridge serves on its setup network (bridge spec
 * 9.2 and 9.3).
 *
 * Every request that reaches port 80 from the setup network comes here:
 *
 *   /setup/v1/...     the setup API the setup page uses (JSON)
 *   /, /assets/...    the setup page itself
 *   anything else     a redirect to the setup page, which is what makes a
 *                     phone's connectivity check open the page by itself
 *
 * These paths are outside the Link protocol (Link spec 4.2) and carry no
 * device commands. Runs on the HTTP server's task.
 */

#ifndef SETUP_API_H
#define SETUP_API_H

#include "esp_http_server.h"

/* Answers one request from the setup network. */
esp_err_t setup_api_handle(httpd_req_t *request);

#endif /* SETUP_API_H */
