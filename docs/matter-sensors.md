# Matter sensor reads

The private wire format is [protobuf v3](../micro-thread-matter-daemon/docs/matter-ipc-v3.md), served
by `mthread-matter` in `micro-thread-matter-daemon/`. JSON examples describe public/fixture representations. Nullable values
also use explicit protobuf null variants on the private wire.

Boolean State (cluster 69) is read as a typed boolean using the generic
Interaction Model attribute path and is included in subscription reports.
Reports map into shared native/UI contact state.

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
The native worker validates and maps these values into the shared device catalog,
dashboard and sensor history. Supported units retain separate history series;
unknown/null samples do not fabricate measurements. See the
[history contract](../microhad/SENSOR_HISTORY_FORMAT.md) and
[HTTP API](../microhad/docs/matter-native-api-v1.md).

## Electrical measurements

Electrical Power Measurement (`0x0090`) and Electrical Energy Measurement
(`0x0091`) are discovered using AttributeList and FeatureMap. Subscriptions
request only supported scalar attributes, with a maximum report interval of
60 seconds (the device may negotiate a different interval). Harmonic arrays
remain outside the measurement catalog. No periodic quantity is accumulated
into a synthetic lifetime counter.

The shared `config/electrical-measurements.def` defines power, voltage,
current, frequency, power factor, and cumulative/periodic imported/exported
energy quantities. Values are normalized to W, V, A, Hz, a unitless ratio,
and kWh/kVAh/kvarh. Signed and zero readings are preserved; null is unknown.
Energy-period timestamps and cumulative-reset metadata are retained in their
Matter epoch/systime units; `observed_at` uses Unix receipt time.

Public status includes an `electrical` map. Each measurement has `value`,
`unit`, `quality`, `observed_at` (gateway receipt time), and its actual Matter
`endpoint`. Energy entries also contain the optional period timestamps.
`electrical.cumulative_energy_reset` contains reset metadata rather than a
numeric measurement. Existing `has_power`, `power_known`, and `power_watts`
continue to describe active watts.

When exactly one actuator exists on a node, electrical measurements from a
separate endpoint are attached to that actuator. Multi-actuator nodes retain
measurements on their original endpoints. Reports never change the address
used for switch commands. Fresh inventory snapshots retain measured values;
after service restart subscriptions refresh the measurements.

Electrical reads use the existing `/api/matter/devices/read` endpoint with
the actual measurement endpoint, cluster, and attribute (for example endpoint
2, cluster 144, attribute 8 for active power). IPC values use the additive
`electrical` variant, carrying quantities plus optional energy metadata;
update native and Matter services together.
