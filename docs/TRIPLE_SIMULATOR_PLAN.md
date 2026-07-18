# Triple Gauge Simulator Plan

## Physical architecture

The real system contains three independent 360x360 round displays:

1. Center master gauge
2. Left slave gauge
3. Right slave gauge

The center master receives OBD data and distributes vehicle data to both
slave gauges through ESP-NOW.

## Desktop architecture

The desktop simulator will run the same executable three times:

```text
BRZGaugeSimulator --role master
BRZGaugeSimulator --role slave-left
BRZGaugeSimulator --role slave-right

