# Matter physical acceptance

Host fixtures establish the gateway software contract, but they do not prove
interoperability with a physical accessory or reliability on the target radio.
Run this acceptance only as an explicitly authorized gateway test, using a
factory-new Matter-over-Thread accessory that exposes the features being
validated.

Before testing, capture redacted Zigbee/network status and target resource
headroom. Use the [maintenance deployment](deployment.md#maintenance-release)
procedure; preserve configuration, Zigbee state, Matter identity and Thread
network state.

Acceptance requires all of the following:

- Phone-assisted commissioning reaches `complete`, then CASE reads, supported
  commands and reports work after the phone disconnects.
- Restarting the Matter service and paired gateway restores the existing fabric
  and node without recommissioning, while Zigbee remains formed and online.
- Concurrent Zigbee and Matter traffic remains reliable for the intended
  workload, with acceptable target CPU, memory and flash headroom.
- Recovery from interrupted commissioning, Thread/RCP loss and a Matter
  service restart is observable and does not damage the Zigbee network or
  persistent Matter state.

Record only redacted versions, hashes, timings, resource samples and outcomes.
Do not retain setup codes, datasets, keys or session payloads in test reports.
