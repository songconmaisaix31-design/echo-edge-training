/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_BLE_H
#define BSP_BLE_H
#include "bsp_radio.h"
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        BSP_BLE_ADVERTISE = 1u,
        BSP_BLE_SCAN = 2u,
        BSP_BLE_PERIPHERAL = 4u,
        BSP_BLE_CENTRAL = 8u,
        BSP_BLE_GATT_SERVER = 16u,
        BSP_BLE_GATT_CLIENT = 32u
    };
    typedef struct
    {
        uint32_t operations;
        uint16_t max_mtu, max_connections, max_attributes;
    } BspBleCapabilities;
    typedef struct
    {
        uint8_t bytes[16];
    } BspBleUuid; /* Canonical 128-bit UUID big-endian bytes. */
    enum
    {
        BSP_BLE_READ = 1u,
        BSP_BLE_WRITE = 2u,
        BSP_BLE_NOTIFY = 4u,
        BSP_BLE_INDICATE = 8u
    };
    typedef struct
    {
        uint16_t id;
        BspBleUuid uuid;
        uint32_t properties;
        uint16_t max_length;
    } BspBleCharacteristic;
    typedef struct
    {
        BspBleUuid uuid;
        const BspBleCharacteristic *items;
        size_t count;
    } BspBleService;
    typedef struct
    {
        uint16_t slot;
        uint64_t generation;
    } BspBleConnection;
    typedef enum
    {
        BSP_BLE_OFF,
        BSP_BLE_STARTING,
        BSP_BLE_READY,
        BSP_BLE_STOPPING,
        BSP_BLE_FAULT
    } BspBlePhase;
    typedef enum
    {
        BSP_BLE_CONNECTED,
        BSP_BLE_DISCONNECTED,
        BSP_BLE_WRITTEN,
        BSP_BLE_SUBSCRIBED,
        BSP_BLE_INDICATION_DONE
    } BspBleEventType;
#define BSP_BLE_EVENT_DATA_MAX 512u
    typedef struct
    {
        BspBleEventType type;
        BspBleConnection connection;
        uint16_t characteristic, length;
        uint64_t operation;
        bool notify_enabled, indicate_enabled;
        BspError error;
        uint8_t data[BSP_BLE_EVENT_DATA_MAX];
    } BspBleEvent;
    typedef struct
    {
        BspBlePhase phase;
        BspError error;
        uint32_t dropped_events;
    } BspBleState;
    typedef struct
    {
        uint32_t interval_us, window_us, duration_ms;
        bool active, filter_duplicates;
    } BspBleScanConfig;
    typedef enum
    {
        BSP_BLE_ADDRESS_PUBLIC,
        BSP_BLE_ADDRESS_RANDOM,
        BSP_BLE_ADDRESS_PUBLIC_IDENTITY,
        BSP_BLE_ADDRESS_RANDOM_IDENTITY
    } BspBleAddressType;
    typedef enum
    {
        BSP_BLE_ADV_CONNECTABLE,
        BSP_BLE_ADV_DIRECTED,
        BSP_BLE_ADV_SCANNABLE,
        BSP_BLE_ADV_NONCONNECTABLE,
        BSP_BLE_SCAN_RESPONSE
    } BspBleReportType;
#define BSP_BLE_SCAN_DATA_MAX 31u
    typedef struct
    {
        BspBleAddressType address_type;
        uint8_t address[6]; /* Conventional display order, most significant byte first. */
        BspBleReportType type;
        int8_t rssi_dbm;
        bool rssi_valid;
        uint8_t size;
        uint8_t data[BSP_BLE_SCAN_DATA_MAX]; /* Raw legacy AD; scan response is separate. */
    } BspBleScanReport;
    typedef struct
    {
        uint64_t operation;
        bool active;
        BspError error;
        uint32_t dropped_reports; /* Saturating local loss count, not RF packet loss. */
    } BspBleScanState;
    /* Native legacy scan, no automatic connection. Positive window<=interval,
     * in us; unsupported granularity/range rejects rather than rounds.
     * duration_ms=0 runs until explicit scan_stop/service stop; otherwise native
     * scan duration, not a hard caller deadline. Duplicate filtering is the
     * controller's policy, not a unique-device guarantee. No caller memory held.
     * OK accepts a scan and writes operation; rejection leaves it unchanged.
     * A new accepted scan replaces prior reports/result. Only the latest operation
     * is queryable; query/read do not infer completion from a software timer. */
    BspStatus bsp_ble_scan_start(BspRadio id, const BspBleScanConfig *config, uint64_t *operation);
    BspStatus bsp_ble_scan_query(BspRadio id, uint64_t operation, BspBleScanState *out);
    /* Copies and consumes reports. Empty queue is OK/count=0, even while active.
     * Parameter/token errors do not consume data or modify outputs. */
    BspStatus bsp_ble_scan_read(BspRadio id, uint64_t operation, BspBleScanReport *out,
                                size_t capacity, size_t *count);
    /* Native cancel completion; SDK call may block, no finite-budget promise.
     * Successful return stops this scan; queued reports remain readable.
     * An already completed current scan returns OK. Native failure is retained;
     * no implicit service release or retry. Unsupported targets have no effects. */
    BspStatus bsp_ble_scan_stop(BspRadio id, uint64_t operation);
    BspStatus bsp_ble_capabilities(BspRadio id, BspBleCapabilities *out);
    /* Service table copied at acceptance, no live reads into module memory.
 * services=NULL/count=0 initializes the stack without application GATT services
 * (e.g. for scanning only). Otherwise both must be nonempty.
 * No implicit bonding or NVS erase. init OK=accepted, query READY requires host sync. */
    BspStatus bsp_ble_init(BspRadio id, const BspBleService *services, size_t count);
    BspStatus bsp_ble_query(BspRadio id, BspBleState *out);
    typedef enum
    {
        BSP_BLE_ADVERTISE_NONCONNECTABLE = 0,
        BSP_BLE_ADVERTISE_CONNECTABLE = 1
    } BspBleAdvertisingMode;
    typedef struct
    {
        BspBleAdvertisingMode mode;
        const uint8_t *ad;
        size_t ad_size;
        const uint8_t *scan_response;
        size_t scan_size;
        uint32_t interval_us;
    } BspBleAdvertisingConfig;
    /* Explicit legacy advertising mode. NONCONNECTABLE allows discovery/scan
     * responses but does not accept connections; CONNECTABLE permits peripheral
     * connections. Both payloads are counted AD records <=31 bytes, copied before
     * return. No application pointers are retained, including on failure.
     * Unsupported mode/config rejects before native effects; no silent fallback.
     * ADVERTISE capability means at least one mode; PERIPHERAL is additionally
     * required for CONNECTABLE. Native admission success is not RF reception proof.
     * No automatic advertising restart. Reconfigure via service stop/init.
     * A native failure after partial configuration can retain resources for explicit
     * bsp_ble_stop; it must not falsely release hardware/storage still in use.
     * Calls use native synchronous command completion, without a finite wait promise.
     * query READY describes the BLE service, not continuous advertising activity. */
    BspStatus bsp_ble_advertise_configured(BspRadio id, const BspBleAdvertisingConfig *config);
    /* Compatibility entry: CONNECTABLE mode, identical to configured form.
     * Legacy raw AD records <=31 bytes each, copied. Interval is in microseconds;
 * unrepresentable values reject, not silently rounded. */
    BspStatus bsp_ble_advertise(BspRadio id, const uint8_t *ad, size_t ad_size,
                                const uint8_t *scan_response, size_t scan_size,
                                uint32_t interval_us);
    /* Attribute value is copied. Remote reads use stored value; no blocking callback.
 * Remote writes update atomically only if capacity/event handling can accept them. */
    BspStatus bsp_ble_set_value(BspRadio id, uint16_t characteristic, const void *data, size_t n);
    /* send copies on OK; no retained caller buffer. notify OK is local acceptance;
 * indicate=true yields terminal event with operation ID, not immediate peer ACK.
 * Value must fit negotiated MTU; no implicit multi-message fragmentation. */
    BspStatus bsp_ble_send(BspRadio id, BspBleConnection connection, uint16_t characteristic,
                           const void *data, size_t n, bool indicate, uint64_t *operation);
    BspStatus bsp_ble_events(BspRadio id, BspBleEvent *out, size_t capacity, size_t *count);
    BspStatus bsp_ble_stop(BspRadio id, uint32_t cleanup_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
