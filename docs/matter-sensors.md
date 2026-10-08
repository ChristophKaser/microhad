# Matter sensor reads

Boolean State (cluster 69) is now read as a typed boolean using the generic
Interaction Model attribute path and is included in subscription reports. The
shared native/UI contact mapping is tracked in the native plan 07 checkpoint.

The Rust Matter service accepts capability-gated reads of attribute 0 from
advertised Air Quality (cluster 91), Temperature Measurement (1026), Relative
Humidity Measurement (1029), Carbon Dioxide Concentration Measurement (1037),
and PM2.5 Concentration Measurement (1066) clusters. It executes the read over
CASE and returns a typed result; an endpoint that does not advertise the
requested cluster fails before an exchange starts.

Temperature values use Matter's signed hundredths-of-a-degree Celsius encoding
and are converted to Celsius. Relative humidity uses hundredths of a percent
and values above 100 percent are rejected. Concentration values are returned
with the cluster's MeasurementUnit attribute, including ppm, ppb, ppt, mass per
volume, particle count, Bq/m3, and dB/m units. Non-finite or negative
concentration values are rejected. The Air Quality enum is returned as one of
`unknown`, `good`, `fair`, `moderate`, `poor`, `very_poor`, or
`extremely_poor`.

Nullable measured attributes remain JSON `null` with quality `unknown`; concrete
readings carry quality `measured`. Measurement replies have the shape
`{"type":"measurement","value":{"value":NUMBER_OR_NULL,"unit":UNIT,"quality":QUALITY}}`.
Air-quality replies use `{"type":"air_quality","value":{"value":LEVEL,"quality":QUALITY}}`.
These are service IPC values, not yet the shared gateway device-model or
dashboard representation. Matter UI remains limited to joining and
Matter/Thread-specific settings; sensors must be projected through the shared
Zigbee/Matter/Shelly device catalog before product UI acceptance.

## Verification

From the `ziggurat-compilation` directory, run:

```sh
CARGO_INCREMENTAL=0 CARGO_TARGET_DIR="$PWD/build/thread-matter/target" \
  cargo +nightly-2026-10-02 test \
  --locked --manifest-path thread-matter/Cargo.toml \
  --features controller-service --lib
```
checks request capability gating and exact typed JSON, including signed Celsius
and null values. The encrypted fixture is run with:

```sh
bash thread-matter/scripts/build.sh host commissioning
python3 thread-matter/scripts/test-openthread-matter-transport.py --commissioning --ble --service-owner --service-discovery
```

The fixture synthetic accessory advertises all five sensor clusters and verifies
temperature, humidity, CO2, PM2.5 and Air Quality reads through the production
service owner and CASE. It uses a synthetic RCP and phone GATT endpoint; it does
not test a physical accessory or radio.
