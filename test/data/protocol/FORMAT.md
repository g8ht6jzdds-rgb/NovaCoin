# NovaCoin protocol vector format

This directory is test data for the normative vector rules in
`docs/protocol.md` section 29.  The source document remains normative if this
file conflicts with it.

Each `.ncpv` file uses UTF-8 without a BOM, ASCII characters only, LF line
endings, and exactly one trailing LF. It starts with `NCPV1`. A record is
canonical `name=value` lines in strictly increasing ASCII name order, followed
by a line containing only `.`. Blank lines, comments, duplicate fields, and
unknown required fields are invalid.

Bytes are lowercase even-length hexadecimal. Decimal values are unsigned
canonical decimal (no sign or leading zeros, except `0`). `MANIFEST.sha256`
is an uppercase SHA-256 manifest over all `.ncpv` files, sorted by relative
path. Test data does not establish a consensus rule that is absent from the
protocol.
