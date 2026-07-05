# Vendored libftpvita

This directory vendors `xerpi/libftpvita` so vitacompanion can carry local FTP
compatibility fixes without depending on the VitaSDK-installed archive.

Upstream: https://github.com/xerpi/libftpvita
Vendored commit: `77a1d39c1d6f11a55fe65a6ed9fe707bff5ede4b`

Local changes:

- Normalize FTP command paths consistently so `ux0:/...`, `uma0:/...`, and
  `/ux0:/...` resolve to the same internal full-path form.
- Make `LIST ux0:/...` behave like `LIST /ux0:/...` instead of falling back to
  the current directory.
- Tolerate common `LIST` options such as `-a` and `-la`.
- Add `EPSV` support using the existing passive data socket setup.
- Add `NLST` support for clients that request filename-only listings.
