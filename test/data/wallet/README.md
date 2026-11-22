# Empty version-one compatibility fixture

`empty-v1.nwlt` was produced by the pre-remediation wallet library retained from
candidate `9f4d0a595514a1013d46d8b0ac3dad3eb50317d7`, before rebuilding the sensitive
buffer correction. It contains **zero keys and zero coins**, and therefore no
private scalar, seed, credential, funds or actual wallet backup.

The intentionally public test passphrase is `empty-v1-fixture`. Decrypted bytes
are version1 (`01 00 00 00`), next index0 (`00 00 00 00`), key count0 (`00`) and
coin count0 (`00`), exactly ten bytes. The existing version1 PBKDF2/AES-GCM format
uses a random salt/nonce; these 67 encrypted bytes are fixed here and bound by
`SHA256SUMS`. This proves the stated empty-file compatibility case, not every
wallet size, key count, cryptographic provider or operating environment.

Do not replace this fixture with a newly encrypted file during tests. Do not use
this public passphrase for a wallet holding keys. No signing or MAINNET operation
was performed to construct the fixture.
