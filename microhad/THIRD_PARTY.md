# Third-party source

`third_party/mbedtls` is a vendored copy of mbedTLS at commit
`bfe79030a92329ab68b0a1f1ad572cdf9a03216d` from the copy already supplied
with this workspace. It is licensed under Apache-2.0.

The project builds its libraries from source with the RSDK in `make mips`.
Generated objects and archives are ignored; no host-built cryptographic object
is shipped or linked into the MIPS binary.
