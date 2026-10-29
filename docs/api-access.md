# Gateway API access

The gateway has three shared access levels, with one optional password each:

- **Guest:** view device data and history, retrieve companion configuration, register Android or browser push clients.
- **User:** guest access plus device controls, thermostat controls, scene activation, refresh and identify.
- **Administrator:** user access plus gateway and device configuration, pairing and commissioning, network management, diagnostics, push-client management, automations and access passwords.

Use **Gateway settings → Access passwords** in the dashboard to set, replace or remove a password. Empty passwords inherit the nearest configured password below that role. With no passwords set, everything is accessible without login. Examples:

| Passwords set | Guest functions | User functions | Administrator functions |
| --- | --- | --- | --- |
| None | Open | Open | Open |
| Guest | Guest password | Guest password | Guest password |
| User | Open | User password | User password |
| Guest and administrator | Guest password | Guest password | Administrator password |
| All three | Guest password | User password | Administrator password |

Choose the desired level at login and enter its effective password. Browser sessions expire after 12 hours unless **Remember me** is checked. App sessions and remembered browser sessions have no expiry. Persistent sessions survive gateway and client restarts. Logout clears the token on that client, including when the gateway is offline. A copied token remains valid until its expiry or a password change. Changing a password invalidates tokens and pending login challenges for subsequent requests and connection handshakes. Already established commissioning relay connections remain open; the new credentials are required when reconnecting. Log in again using the new effective password.

The gateway stores only salted password verifiers in private `api-access.json` beside its other runtime configuration. Preserve this file during deployment and backups; it contains authentication material. Sessions are signed tokens with no server-side session table, session count limit, or session persistence. Login, logout and ordinary requests do not write access state; password changes are persisted atomically. Corrupt or unreadable access storage denies API access rather than reverting to open access. An administrator with local filesystem access can recover by stopping the daemon and restoring this file from a private backup, or removing it to restore open defaults.

The signing key is derived from all three roles' salts and password verifiers in fixed order and cached in RAM. No separate signing secret is stored. Any change to this password configuration changes the key, invalidating previously issued tokens. Existing session-table tokens require a fresh login after upgrading; old session rows are ignored when reading the access file and removed on the next password save, without a startup migration write.

## HTTP and HTTPS clients

Login works over HTTP and HTTPS. Clients derive a 32-byte verifier using PBKDF2-HMAC-SHA256, 100,000 iterations, UTF-8 password bytes and a random 16-byte salt. Password changes submit only the salt and verifier. Login submits an HMAC proof for a single-use random challenge, never the password or verifier.

1. `GET /api/auth/status` returns the current numeric role (`-1` means none, `0` guest, `1` user, `2` administrator) and three booleans indicating explicitly configured passwords.
2. `POST /api/auth/challenge` with JSON `{"role":"administrator"}` returns `open: true` or a lowercase hexadecimal `challenge` and `salt`, plus `iterations: 100000`. Challenges expire after 60 seconds; issuance is bounded and rate limited.
3. Derive the verifier and compute HMAC-SHA256 over UTF-8 `microhad-login-v1\nCHALLENGE\nROLE\nMODE`, where MODE is `browser` or `persistent`.
4. `POST /api/auth/login` with JSON `challenge`, hexadecimal `proof` and `mode` returns a signed session `token`. Send `Authorization: Bearer TOKEN` with API requests. Login challenges can only be used once, including failed attempts.
5. Logout only clears client credentials; there is no logout endpoint. `POST /api/auth/password` requires administrator access and accepts JSON `role`, `salt`, `verifier`; empty salt and verifier remove that role's password.

Authentication POSTs require `application/json`. Cross-origin browser API requests are denied. Browser WebSockets carry the token in the `microhad-auth.TOKEN` offered subprotocol, avoiding URL credentials; native clients can use the Authorization header. The commissioning relay requires administrator access. Shelly callbacks continue using their existing per-device callback tokens.

HTTP does not encrypt session tokens, traffic or the delivered Web UI. Challenge-response avoids plaintext passwords and challenge replay; it does not prevent active network manipulation or session interception. HTTPS remains supported for transport protection.

## Signed token format

Tokens are 116 lowercase hexadecimal characters: version byte `02`, one role byte (`00`, `01`, `02`), an eight-byte unsigned big-endian expiry in Unix seconds (`0` means no expiry), a random 16-byte nonce, and a 32-byte HMAC-SHA256 signature. Version, role, expiry and nonce form the 52-character hexadecimal payload. The HMAC message is UTF-8 `microhad-session-token-v2\nPAYLOAD`; the signature covers the role and expiry as well as the nonce. Unknown versions, noncanonical encodings, bad signatures and expired tokens are rejected.

The 32-byte signing key is SHA-256 of UTF-8 `microhad-session-key-v2\n`, followed by `ROLE\nSALT\nVERIFIER\n` for guest, user and administrator, in that order. Salts and verifiers use their stored lowercase hexadecimal strings; unset fields are empty. Clients treat tokens as opaque strings. The server checks expiry using its current clock: an expiry equal to the current second is already expired.

## Host tools

Set `API_ADMIN_PASSWORD_FILE` in the gitignored `workspace.local.env` to the pathname of a private UTF-8 password file. It must contain the **effective administrator password**, which may be the user or guest password when inherited. One trailing line ending is accepted. Existing environment overrides and relative-path resolution follow the shared workspace settings rules. Never commit the password file.

Repository deployment verification, network backup, reporting diagnostics, release installation and Thread log capture use `scripts/gateway_api.py` for authenticated access. Its CLI can replace an API curl call:

```sh
python3 scripts/gateway_api.py http://GATEWAY/api/status
```

Tools use 12-hour signed tokens and clear their local credentials on normal exit without a logout request. They authenticate before submitting mutations and never automatically replay a failed mutation. Local synthetic integration fixtures continue to use open defaults and never load real gateway credentials. Configured host settings do not authorize deployment or device tests.
