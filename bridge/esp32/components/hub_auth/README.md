# hub_auth

The bridge's optional PIN (bridge spec 8.9): the PIN stored as a salted
PBKDF2 hash, paired clients' tokens stored as SHA-256 hashes, and the
pairing rate limits of Link spec 6.2. Crypto, randomness, time and saving
come in through `hub_auth_io_t`, so the logic runs on a computer too. If
the random source or a hash fails, nothing is set, issued or accepted: the
functions answer `HUB_AUTH_UNAVAILABLE`. The same answer, for every PIN and
token, follows `hub_auth_mark_unreadable()`, until a new PIN is set: a
saved PIN that cannot be read never turns into no PIN.

| File | What it does | Portable |
|------|--------------|----------|
| `include/hub_auth.h`, `hub_auth.c` | Setting and checking the PIN, pairing, issuing and checking tokens, the client list, revocation and rate limits. | yes |

Public API: `hub_auth_init()`, `hub_auth_mark_unreadable()`,
`hub_auth_unreadable()`, `hub_auth_tick()`, `hub_auth_set_pin()`,
`hub_auth_check_pin()`, `hub_auth_pair()`, `hub_auth_issue()`,
`hub_auth_token()`, `hub_auth_touch()`, `hub_auth_revoke()`,
`hub_auth_clients()`, `hub_auth_pin_enabled()`, `hub_auth_pin_is_valid()`,
`hub_auth_copy_name()`.

Tests: `test/host/test_hub_auth.c`, with `fake_crypto.c`.
