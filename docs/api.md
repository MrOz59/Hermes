# API

Hermes has a RESTful API which can be used to interact with the service. It is
served by the Web UI's HTTPS server, on port 47990 by default.

Two different callers are authenticated in two different ways:

- **Web UI endpoints** (everything except `/api/hestia/v1/*`) use the admin
  account. Authenticate with basic authentication using the admin username and
  password, or with the session cookie `/api/login` returns.
- **Hestia endpoints** (`/api/hestia/v1/*`) use the *paired client certificate*
  presented during the TLS handshake. This uses the pairing completed by Hestia,
  Artemis, and Moonlight clients. Each endpoint also requires the matching
  per-client permission. An unpaired certificate is rejected with
  `401 unauthorized`; a paired client without the permission gets
  `403 permission_denied`. These endpoints never accept the admin password. The
  one exception is `capabilities`, which is deliberately unauthenticated so a
  client can detect Hermes before it has paired; it is read-only and exposes no
  host state.

Unless otherwise specified, authentication is required for all API calls.

See the [C++ reference](pathname:///reference/cpp/index.htm) for host internals.


## POST /api/login

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-login).

## POST /api/otp

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-otp).

## POST /api/pin

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-pin).

## GET /api/apps

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-apps).

## POST /api/apps

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-apps).

## POST /api/apps/delete

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-apps-delete).

## POST /api/apps/reorder

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-apps-reorder).

## POST /api/apps/launch

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-apps-launch).

## POST /api/apps/close

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-apps-close).

## POST /api/covers/upload

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-covers-upload).

## GET /api/clients/list

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-clients-list).

## POST /api/clients/update

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-clients-update).

## POST /api/clients/disconnect

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-clients-disconnect).

## POST /api/clients/unpair

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-clients-unpair).

## POST /api/clients/unpair-all

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-clients-unpair-all).

## GET /api/config

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-config).

## GET /api/configLocale

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-configlocale).

## POST /api/config

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-config).

## POST /api/password

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-password).

## GET /api/logs

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-logs).

## GET /api/metrics

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-metrics).

## GET /api/gamemode/status

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-gamemode-status).

## GET /api/sessions/list

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-sessions-list).

## POST /api/sessions/terminate

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-sessions-terminate).

## POST /api/reset-display-device-persistence

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-reset-display-device-persistence).

## POST /api/restart

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-restart).

## POST /api/quit

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-quit).

## Virtual display drivers

These back the live diagnostic and the guided install in the Audio/Video
settings tab. They are Linux-only; on other platforms they answer with
`error` set.

### GET /api/evdi/status

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-evdi-status).

### POST /api/evdi/install

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-evdi-install).

### GET /api/evdi/install/status

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-evdi-install-status).

### GET /api/hermes-kms/status

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-hermes-kms-status).

## Clipboard sharing

### GET /api/clipboard/status
Report whether clipboard sharing is available on this host, and what to install
if it is not.

### POST /api/clipboard/install
Install the clipboard helper this host is missing. On Linux pkexec owns the
privilege prompt, so Hermes never handles an admin password.

## Hestia API v1

Authenticated by the paired client certificate, not the admin password — see
the note at the top of this page. Clients should gate enhanced behaviour on the
capabilities response and fall back to the standard Moonlight/Sunshine flow when
these endpoints are absent.

### GET /api/hestia/v1/capabilities

[Source-generated reference](pathname:///reference/cpp/http_api.htm#get-api-hestia-v1-capabilities).

`GET /api/hestia/v1` answers with the same document. This is the only endpoint
on this page that requires no authentication.

### POST /api/hestia/v1/session/prepare

[Source-generated reference](pathname:///reference/cpp/http_api.htm#post-api-hestia-v1-session-prepare).

### POST /api/hestia/v1/session/stop
Clear this client's prepared session and stop the stream it owns, if one is
running. Requires the `launch` permission.

### GET /api/hestia/v1/display/status
Report the virtual display and physical-monitor state: which backend is in use,
whether the physical outputs were disabled for a stream, and whether a recovery
state file exists. Requires the `view` permission.

### POST /api/hestia/v1/display/recover
Restore the physical display layout after a session left it disabled. Requires
the `launch` permission.

### GET /api/hestia/v1/client/permissions
Report the calling client's own id and the permissions it was granted, plus the
features those permissions enable. Available to any paired client.

### GET /api/hestia/v1/diagnostics
Return the live runtime status, the streaming-readiness preflight and the state
of optional dependencies. Requires the `view` permission.

### GET /api/hestia/v1/clipboard
Read the host clipboard. Requires the `clipboard_read` permission.

### POST /api/hestia/v1/clipboard
Write the host clipboard. Requires the `clipboard_set` permission.

### GET /api/hestia/v1/commands
List the server commands this client may run, as configured by `server_cmd`.
Requires the `server_cmd` permission.

### POST /api/hestia/v1/commands/run
Run one of those server commands by name. Requires the `server_cmd` permission.
