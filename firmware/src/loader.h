/**
 * Remote JS Loader protocol client (lwIP BSD sockets - real half-close via
 * shutdown(WR), which the loader requires to see end-of-payload).
 * Covers the SEND and INSTALL flows.
 *
 * Protocol: TCP to the console on :9026, one
 * message per connection: send bytes, half-close, then read log lines the
 * console streams back until it closes (or deadline).
 */
#pragma once

#include <Arduino.h>

namespace loader {

typedef void (*LogFn)(const char *line, void *user);
typedef void (*ProgressFn)(int done, int total, const char *phase, void *user);

/**
 * Streams a file's bytes to the loader, half-closes, then pumps the response
 * log through `log` line by line (also until `timeoutMs` total). UI stays
 * responsive: the read loop yields to LVGL while waiting.
 * Returns false and fills `err` on transport failure. Payload content errors
 * are reported through the log, not the return value (the loader evals and
 * reports "Error in payload: ..." itself).
 */
bool sendFile(const char *path, LogFn log, void *user, String *err, uint32_t timeoutMs);

/**
 * Blocking TCP connect to the loader (target = net::targetIp()). Use for a
 * click-time loader check; call from a UI handler, it blocks the UI for up
 * to `timeoutMs`.
 */
bool ping(uint32_t timeoutMs = 8000);

/**
 * Runs the full savefile-installer protocol: reset connection,
 * chunked template upload (each chunk response must confirm execution),
 * apply payload, response must contain the installer's done marker.
 * `progress` fires per template chunk; `log` gets every streamed line.
 */
bool installBundle(LogFn log, ProgressFn progress, void *user, String *err,
                   uint32_t timeoutMs);

} // namespace loader
