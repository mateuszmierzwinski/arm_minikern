# Test fixtures

- `ustar.tar`: `tar --format=ustar -cf ustar.tar ./init etc etc/motd` (GNU tar 1.35), where
  `init` holds `abc` and `etc/motd` holds `hi\n`.
- `pax.tar`: `tar --format=pax -cf pax.tar init`, the same `init`. It starts with a PAX `x`
  extension record that the reader must skip.
