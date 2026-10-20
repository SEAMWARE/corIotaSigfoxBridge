# corIotaSigfoxBridge

**Prototype - not released.** The FIWARE IoT Agent for Sigfox in the coraine NGSI-LD broker: the Sigfox
backend's **callbacks** as **DataChannel records**, and a written attribute as a **downlink** -
`iotasigfox.so`, a bridge plugin loaded with `--bridges iotasigfox`. A callback iotagent-sigfox takes
(`GET /update?id=...&data=...`) it takes the same way, with the same answers.

## Callbacks

The Sigfox backend calls back on the **southbound port** - served by the broker for the plugin (bridge ABI
16, `httpListen`), plain HTTP on every address, no NGSI-LD and no authentication:

    GET  /<path>?id=<device>&data=<hex>&time=<seconds>&...          as iotagent-sigfox (its path: /update)
    POST /<path>   { "device": "<device>", "data": "<hex>", ... }   a Sigfox custom callback, application/json

A DataChannel's endpoint is `<path>` - every device that calls back on `/<path>`, its entity from the
record's `@deviceId` - or `<path>/<deviceId>`, one device (it takes the device's callbacks before the path's
DataChannel does; the device id compared without case).

- **The record** is the data decoded by the DataChannel's `mapping` (below), each field a column, plus
  `@deviceId` and `@data` (the hex). The callback's other variables are `@`-columns too: a POST's every member
  (`@seqNumber`, `@station`, ...); a GET's, of Sigfox's names - `time`, `seqNumber`, `deviceTypeId`, `ack`,
  `longPolling`, `duplicate`, `station`, `snr`, `avgSnr`, `rssi`, `lat`, `lng`, `linkQuality`, `operatorName`,
  `countryCode`, `computedLocation`, `fixedLat`, `fixedLng`, `statin`, `device` - as strings, as
  iotagent-sigfox has them.
- **Its time** (`observedAt`): the callback's `time`, seconds since the epoch.
- **No `mapping`:** the record is the `@`-columns alone - the data undecoded in `@data`.
- **The answers** are iotagent-sigfox's: `200 {}`; no device or no data `400 MANDATORY_FIELDS_NOT_FOUND`; a
  device no DataChannel has `404 DEVICE_NOT_FOUND` - `{"name": ..., "message": ...}`. Data that is not 0 to 12
  bytes of hex is 400 `BAD_DATA`, a method other than GET and POST 405.

### The payload format

`channelInfo` `mapping`: the grammar of the Sigfox backend's custom payload configuration - iotagent-sigfox's
`mapping` is the same, with the byte index left out:

    <name>:<byte index>:<type>:<arguments>      fields separated by white space

| Type | Arguments | The value |
|---|---|---|
| `uint` | bits: 8, 16, ... 64; `little-endian` or `big-endian` (the default) | a number |
| `int` | as uint - two's complement | a number |
| `float` | 32 or 64; the endianness | a number (a float32 as its shortest decimal: 21.3) |
| `bool` | the bit, 0 (the least significant) to 7 | a boolean |
| `char` | the length, bytes | a string, to its first NUL |

A field without a byte index follows the one before it; a bool after a bool is in the same byte:
`temp:0:int:16:little-endian hum::uint:8 door:3:bool:7 alarm:3:bool:0 level:4:float:32 tag:8:char:4`.
A field the data is too short for is left out of the record, as the Sigfox backend leaves it out.
iotagent-sigfox reads `uint` only.

### Downlinks

With `"channelDirection": "both"` (coraine's DataChannel write-back, `"idEncoding": "percent"`) and a
`downlinkMapping` - the grammar above, at most 8 bytes - a client's write to a mapped attribute is the device's
next downlink:

- The fragment's columns are encoded by the `downlinkMapping` into the device's **queued downlink**, 8 bytes.
  Writes before the device calls back are one downlink; a field never written is 0.
- The device's next **bidirectional** callback (`ack` true - a string in a GET, a boolean in a POST) is
  answered with it, and it is no longer queued:

      200 { "<device>": { "downlinkData": "0102580100000000" } }

  A bidirectional callback with none queued is answered 204 - no downlink; any other callback `200 {}`.
- **The device**: the fragment's `@deviceId` (an id template with `{@deviceId}`), else the endpoint's.
- Refused, nothing stored: a column the `downlinkMapping` has not, a value its field cannot hold (300 to a
  `uint:8`, a string to an `int`) - 400; a DataChannel without a `downlinkMapping` - 422. Up to 4096 devices
  may have a downlink queued.
- The device's next message is what it has: a record written after a downlink replaces the written values.

iotagent-sigfox has no downlinks.

A Channel or an AssetChannel configured for this bridge is refused (dormant): it carries DataChannels only.

## Configuration

```json
{
  "iotasigfox": {
    "southbound": { "port": 17428 },
    "ngsild": {
      "defaultEntity": false,
      "dataChannels": {
        "update": { "channelDirection": "both", "idEncoding": "percent",
                    "channelInfo": [ { "key": "mapping",         "value": "counter::uint:32 temperature::int:16 voltage::uint:16" },
                                     { "key": "downlinkMapping", "value": "led::uint:8 period::uint:16" } ],
                    "entities": [ { "type": "Sensor", "id": "urn:ngsi-ld:Sensor:{@deviceId}",
                                    "attributes": { "counter":     { "column": "counter" },
                                                    "temperature": { "column": "temperature" },
                                                    "led":         { "column": "led" },
                                                    "station":     { "column": "@station" } } } ] }
      }
    }
  }
}
```

| `southbound` member | |
|---|---|
| `port` | the southbound port - 17428, iotagent-sigfox's, when not said |
| `idField` | the callback's parameter that names the device (iotagent-sigfox's `idFieldName`). Not said: `id`, else `device` |

A channelInfo key other than `mapping` and `downlinkMapping`, or a format the grammar refuses: the DataChannel
is refused (the broker's log says why).

## Not built

The provisioning API (a DataChannel comes from the configuration file), iotagent-sigfox's `plugin` (a
JavaScript decoder), fields of fewer than 8 bits, a form-encoded POST, the backend's service callbacks
(status, error, geolocation as their own kinds), HTTPS on the southbound port.

## Build and test

    make && make test          # no transport library; the tests run curl and python3

The tests play the Sigfox backend with curl on the southbound port 17432.
