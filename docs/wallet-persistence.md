# Encrypted wallet persistence

`wallet.dat` is an encrypted binary container, never a marker file and never a
plaintext private-key export. It is wallet-local state, not consensus data.

The file consists of canonical fields: `NWLT` magic, little-endian version
`1`, little-endian PBKDF2 iteration count `300000`, 16-byte random salt,
12-byte random AES-GCM nonce, bounded ciphertext, and a 16-byte GCM tag.
The plaintext holds a version, next address index, canonical private-key/index
pairs, then canonical wallet UTXOs including their confirmation state.

Keys are derived with PBKDF2-HMAC-SHA-256 and used only with AES-256-GCM.
Authentication failure, unknown KDF parameters, malformed canonical data,
duplicate keys/coins, invalid values, trailing bytes, and any size limit
failure reject the entire file.

Writes create only an encrypted sibling temporary file, flush it, and replace
the destination atomically (with write-through replacement on Windows). Empty
passphrases never produce a persisted wallet. Daemon deployments must supply
the passphrase out of band; it must not be placed in command-line arguments,
logs, or normal RPC responses.
