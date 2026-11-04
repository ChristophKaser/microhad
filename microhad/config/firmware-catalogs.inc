// Built-in catalog destinations. Overrides use one JSON object per line.
static const char defaultFirmwareCatalogs[] =
"{\"id\":\"sonoff\",\"label\":\"SONOFF\",\"url\":\"https://zigbee-ota.sonoff.tech/releases/upgrade.json\",\"manufacturer\":4742}\n"
"{\"id\":\"ikea\",\"label\":\"IKEA\",\"url\":\"https://fw.ota.homesmart.ikea.com/DIRIGERA/version_info.json\",\"manufacturer\":4476,\"ca\":\"ikea\"}\n"
"{\"id\":\"zigbee-ota\",\"label\":\"zigbee-OTA\",\"url\":\"https://raw.githubusercontent.com/Koenkk/zigbee-OTA/master/index.json\"}\n"
;
// IKEA Home smart root CA, from zigpy/ota/providers.py (public certificate).
static const char ikeaCatalogCa[] =
"-----BEGIN CERTIFICATE-----\n"
"MIICGDCCAZ+gAwIBAgIUdfH0KDnENv/dEcxH8iVqGGGDqrowCgYIKoZIzj0EAwMw\n"
"SzELMAkGA1UEBhMCU0UxGjAYBgNVBAoMEUlLRUEgb2YgU3dlZGVuIEFCMSAwHgYD\n"
"VQQDDBdJS0VBIEhvbWUgc21hcnQgUm9vdCBDQTAgFw0yMTA1MjYxOTAxMDlaGA8y\n"
"MDcxMDUxNDE5MDEwOFowSzELMAkGA1UEBhMCU0UxGjAYBgNVBAoMEUlLRUEgb2Yg\n"
"U3dlZGVuIEFCMSAwHgYDVQQDDBdJS0VBIEhvbWUgc21hcnQgUm9vdCBDQTB2MBAG\n"
"ByqGSM49AgEGBSuBBAAiA2IABIDRUvKGFMUu2zIhTdgfrfNcPULwMlc0TGSrDLBA\n"
"oTr0SMMV4044CRZQbl81N4qiuHGhFzCnXapZogkiVuFu7ZqSslsFuELFjc6ZxBjk\n"
"Kmud+pQM6QQdsKTE/cS06dA+P6NCMEAwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQUcdlEnfX0MyZA4zAdY6CLOye9wfwwDgYDVR0PAQH/BAQDAgGGMAoGCCqGSM49\n"
"BAMDA2cAMGQCMG6mFIeB2GCFch3r0Gre4xRH+f5pn/bwLr9yGKywpeWvnUPsQ1KW\n"
"ckMLyxbeNPXdQQIwQc2YZDq/Mz0mOkoheTUWiZxK2a5bk0Uz1XuGshXmQvEg5TGy\n"
"2kVHW/Mz9/xwpy4u\n"
"-----END CERTIFICATE-----\n"
;
