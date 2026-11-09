# Firmware catalog downloads

Device settings match Zigbee firmware from SONOFF, IKEA and Koenkk's zigbee-OTA
index. Matching requires the device's OTA image type and manufacturer (using the node
descriptor manufacturer as a fallback until the OTA manufacturer is known). Catalog model/name,
hardware-version and installed-version restrictions must all be satisfied.
Wake sleeping devices and use the existing device refresh action to collect
missing OTA attributes. Query Next Image requests also update these identifiers.
These values are retained in the RAM device cache and exposed as nullable fields:
`ota_endpoint`, `ota_manufacturer`, `ota_image_type`, `ota_file_version`, and
`hardware_version`. Unknown values remain null; zero is a valid identifier/version.

**Download firmware** is an ordinary link to the original remote firmware URL.
The browser downloads the file. The gateway never downloads, extracts, caches,
or installs firmware as part of catalog discovery. Some manufacturer files are
wrapped containers and need extraction before using the existing manual uploader.
Matter devices use Basic Information vendor ID, product ID and installed software version to query the CSA Matter production and testnet ledgers. Testnet results are labeled as test releases. Only valid newer images whose applicable software-version range includes the installed version are shown; the Matter OTA provider checks the image header and device identity before offering an uploaded image. Hardware version is exposed alongside the identity. Missing identity, catalog failure and an empty result are shown separately. The Matter refresh button requests a fresh discovery and restores the node’s subscriptions; sleeping devices must be awake to respond.

## Configured destinations

Built-in destinations and IKEA's public root CA are maintained in
`microhad/config/firmware-catalogs.inc`. The IKEA feed and certificate follow
<https://github.com/zigpy/zigpy/blob/dev/zigpy/ota/providers.py>.

An optional `firmware_catalogs` string in the gateway configuration selects an
administrator-owned override file. An override replaces all built-in providers;
an empty file disables catalogs. Use one JSON object per line, for example:

```json
{"id":"sonoff","label":"SONOFF","url":"https://zigbee-ota.sonoff.tech/releases/upgrade.json","manufacturer":4742}
{"id":"ikea","label":"IKEA","url":"https://fw.ota.homesmart.ikea.com/DIRIGERA/version_info.json","manufacturer":4476,"ca":"ikea"}
{"id":"zigbee-ota","label":"zigbee-OTA","url":"https://raw.githubusercontent.com/Koenkk/zigbee-OTA/master/index.json"}
```

Matter providers use IDs `matter-dcl-production` and `matter-dcl-testnet` and URLs ending in `/dcl/model/versions`; their device-specific numeric paths are constructed by the browser and restricted by the gateway proxy to vendor ID, product ID and optional software version. The built-in URLs are `https://on.dcl.csa-iot.org/dcl/model/versions` and `https://on.test-net.dcl.csa-iot.org/dcl/model/versions`. Overrides must include these providers to enable Matter checks.

`id` selects the parser: `ikea` and `sonoff` use their vendor formats; other IDs
use zigbee-OTA format. Optional `manufacturer` restricts when the UI queries that
provider. Optional `ca` is a provider-scoped CA file path, or `ikea` for the
embedded IKEA root. Otherwise the configured `ca_bundle` is used. Providers
with a custom CA use the gateway immediately; others try browser access first.
Do not expose private CA files through the Web UI directory.

`GET /api/ota/catalogs` returns provider IDs, labels, catalog URLs, manufacturer
filters and whether gateway access is required. `GET /api/ota/catalog/ID` fetches
only that provider's configured catalog. Both use the existing API authentication
and roles. No endpoint accepts a proxy URL, method or custom request headers.

Each configured URL is an exact destination allowlist entry, except the two Matter DCL providers, whose only permitted suffixes are canonical decimal vendor/product IDs and an optional software version. HTTPS is required;
redirects to any other URL are rejected. DNS results must all be public IPv4
addresses, and the connection uses a validated address without resolving again.
IPv6, internal addresses, credentials in URLs, fragments, queries, encoded paths
and traversal are rejected. Certificate and hostname checks remain enabled.
The transport uses a five-second connect timeout, three-second socket timeouts
and a 25-second request deadline after starting resolution; DNS resolution itself
uses the system resolver's timeout policy. Catalogs are limited to 4 MiB, and
only one gateway catalog fetch runs at a time. Cached data expires after one
hour, with total cache storage limited to 4 MiB of RAM. No catalog data is
written to gateway flash. Browser downloads are independent of this metadata
allowlist and use the original HTTP/HTTPS URLs supplied by valid catalog entries.

## Verification

Run `make -C microhad host test`, `make -C microhad mips`, and
`make -C microhad webui`. Run `npm run test:firmware` in `microhad/webui` for
catalog adapter, compatibility matching and headless browser checks. The browser
fixture verifies that only metadata is fetched by JavaScript and firmware links
point directly to the original remote URLs.
