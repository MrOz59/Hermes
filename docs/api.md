# API

Hermes has a RESTful API which can be used to interact with the service. It is
served by the Web UI's HTTPS server, on port 47990 by default.

Two different callers are authenticated in two different ways:

- **Web UI endpoints** (everything except `/api/hestia/v1/*`) use the admin
  account. Authenticate with basic authentication using the admin username and
  password, or with the session cookie `/api/login` returns.
- **Hestia endpoints** (`/api/hestia/v1/*`) use the *paired client certificate*
  presented during the TLS handshake — the same pairing a Moonlight, Artemis or
  Hestia client completes — and each one additionally requires the matching
  per-client permission. An unpaired certificate is rejected with
  `401 unauthorized`; a paired client without the permission gets
  `403 permission_denied`. These endpoints never accept the admin password. The
  one exception is `capabilities`, which is deliberately unauthenticated so a
  client can detect Hermes before it has paired; it is read-only and exposes no
  host state.

Unless otherwise specified, authentication is required for all API calls.

@htmlonly
<script src="api.js"></script>
@endhtmlonly

## POST /api/login
@copydoc confighttp::login()

## POST /api/otp
@copydoc confighttp::getOTP()

## POST /api/pin
@copydoc confighttp::savePin()

## GET /api/apps
@copydoc confighttp::getApps()

## POST /api/apps
@copydoc confighttp::saveApp()

## POST /api/apps/delete
@copydoc confighttp::deleteApp()

## POST /api/apps/reorder
@copydoc confighttp::reorderApps()

## POST /api/apps/launch
@copydoc confighttp::launchApp()

## POST /api/apps/close
@copydoc confighttp::closeApp()

## POST /api/covers/upload
@copydoc confighttp::uploadCover()

## GET /api/clients/list
@copydoc confighttp::getClients()

## POST /api/clients/update
@copydoc confighttp::updateClient()

## POST /api/clients/disconnect
@copydoc confighttp::disconnect()

## POST /api/clients/unpair
@copydoc confighttp::unpair()

## POST /api/clients/unpair-all
@copydoc confighttp::unpairAll()

## GET /api/config
@copydoc confighttp::getConfig()

## GET /api/configLocale
@copydoc confighttp::getLocale()

## POST /api/config
@copydoc confighttp::saveConfig()

## POST /api/password
@copydoc confighttp::savePassword()

## GET /api/logs
@copydoc confighttp::getLogs()

## GET /api/metrics
@copydoc confighttp::getMetrics()

## GET /api/gamemode/status
@copydoc confighttp::getGameModeStatus()

## GET /api/sessions/list
@copydoc confighttp::getSessions()

## POST /api/sessions/terminate
@copydoc confighttp::terminateSession()

## POST /api/reset-display-device-persistence
@copydoc confighttp::resetDisplayDevicePersistence()

## POST /api/restart
@copydoc confighttp::restart()

## POST /api/quit
@copydoc confighttp::quit()

## Virtual display drivers

These back the live diagnostic and the guided install in the Audio/Video
settings tab. They are Linux-only; on other platforms they answer with
`error` set.

### GET /api/evdi/status
@copydoc confighttp::getEvdiStatus()

### POST /api/evdi/install
@copydoc confighttp::installEvdi()

### GET /api/evdi/install/status
@copydoc confighttp::getEvdiInstallStatus()

### GET /api/hermes-kms/status
@copydoc confighttp::getHermesKmsStatus()

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
@copydoc confighttp::getHestiaCapabilities()

`GET /api/hestia/v1` answers with the same document. This is the only endpoint
on this page that requires no authentication.

### POST /api/hestia/v1/session/prepare
@copydoc confighttp::prepare_hestia_session()

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

<div class="section_buttons">

| Previous                                    |                                  Next |
|:--------------------------------------------|--------------------------------------:|
| [Performance Tuning](performance_tuning.md) | [Troubleshooting](troubleshooting.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
