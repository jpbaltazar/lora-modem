#include "lrw.h"
#include <assert.h>
#include <LoRaWAN/Utilities/timeServer.h>
#include <LoRaWAN/Utilities/utilities.h>
#include <loramac-node/src/mac/LoRaMac.h>
#include <loramac-node/src/mac/LoRaMacTest.h>
#include <loramac-node/src/mac/region/Region.h>
#include <loramac-node/src/radio/radio.h>
#include <loramac-node/src/mac/LoRaMacCrypto.h>
#include "adc.h"
#include "cmd.h"
#include "system.h"
#include "halt.h"
#include "log.h"
#include "part.h"
#include "utils.h"
#include "eeprom.h"
#include "irq.h"
#include "nvm.h"

#define MAX_BAT 254


static McpsConfirm_t tx_params;
static int joins_left = 0;
static TimerEvent_t join_retry_timer;


enum lora_event {
    NO_EVENT = 0,
    RETRANSMIT_JOIN = (1 << 0)
};

static unsigned events;


static struct {
    const char *name;
    int id;
} region_map[] = {
    { "AS923", LORAMAC_REGION_AS923 },
    { "AU915", LORAMAC_REGION_AU915 },
    { "CN470", LORAMAC_REGION_CN470 },
    { "CN779", LORAMAC_REGION_CN779 },
    { "EU433", LORAMAC_REGION_EU433 },
    { "EU868", LORAMAC_REGION_EU868 },
    { "KR920", LORAMAC_REGION_KR920 },
    { "IN865", LORAMAC_REGION_IN865 },
    { "US915", LORAMAC_REGION_US915 },
    { "RU864", LORAMAC_REGION_RU864 }
};


#ifdef RESTORE_CHMASK_AFTER_JOIN
static uint16_t saved_chmask[REGION_NVM_CHANNELS_MASK_SIZE];
#endif


static int region2id(const char *name)
{
    if (name == NULL) return -1;

    for (unsigned int i = 0; i < sizeof(region_map) / sizeof(region_map[0]); i++)
        if (!strcmp(region_map[i].name, name)) return region_map[i].id;
    return -2;
}

#if defined(DEBUG)
static const char *region2str(int id)
{
    for (unsigned int i = 0; i < sizeof(region_map) / sizeof(region_map[0]); i++)
        if (region_map[i].id == id ) return region_map[i].name;
    return NULL;
}
#endif

static uint8_t get_battery_level(void)
{
    // callback to get the battery level in % of full charge (254 full charge, 0
    // no charge)
    return MAX_BAT;
}


static void process_notify(void)
{
    // This handler can be invoked from the IRQ context (on timers or events
    // generated by the radio), or from the thread context (manually invoked by
    // LoRaMac during ABP activation).
    //
    // Disable sleep so that LoRaMacProcess() gets a chance to run to process
    // the event.
    uint32_t mask = disable_irq();
    system_sleep_lock |= SYSTEM_MODULE_LORA;
    reenable_irq(mask);
}


static void save_state(void)
{
    uint32_t mask;
    LoRaMacNvmData_t *s;

    if (nvm_flags == LORAMAC_NVM_NOTIFY_FLAG_NONE) {
        mask = disable_irq();
        system_sleep_lock &= ~SYSTEM_MODULE_NVM;
        reenable_irq(mask);
        return;
    }

    mask = disable_irq();
    system_sleep_lock |= SYSTEM_MODULE_NVM;
    reenable_irq(mask);

    s = lrw_get_state();

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_CRYPTO) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving Crypto state to NVM");
        if (!part_write(&nvm_parts.crypto, 0, &s->Crypto, sizeof(s->Crypto)))
            log_error("Error while writing Crypto state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_CRYPTO;
        return;
    }

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP1) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving MacGroup1 state to NVM");
        if (!part_write(&nvm_parts.mac1, 0, &s->MacGroup1, sizeof(s->MacGroup1)))
            log_error("Error while writing MacGroup1 state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP1;
        return;
    }

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP2) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving MacGroup2 state to NVM");
        if (!part_write(&nvm_parts.mac2, 0, &s->MacGroup2, sizeof(s->MacGroup2)))
            log_error("Error while writing MacGroup2 state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP2;
        return;
    }

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_SECURE_ELEMENT) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving SecureElement state to NVM");
        if (!part_write(&nvm_parts.se, 0, &s->SecureElement, sizeof(s->SecureElement)))
            log_error("Error while writing SecureElement state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_SECURE_ELEMENT;
        return;
    }

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_REGION_GROUP1) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving RegionGroup1 state to NVM");
        if (!part_write(&nvm_parts.region1, 0, &s->RegionGroup1, sizeof(s->RegionGroup1)))
            log_error("Error while writing RegionGroup1 state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_REGION_GROUP1;
        return;
    }

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_REGION_GROUP2) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving RegionGroup2 state to NVM");
        if (!part_write(&nvm_parts.region2, 0, &s->RegionGroup2, sizeof(s->RegionGroup2)))
            log_error("Error while writing RegionGroup2 state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_REGION_GROUP2;
        return;
    }

    if (nvm_flags & LORAMAC_NVM_NOTIFY_FLAG_CLASS_B) {
        if (LoRaMacIsBusy()) return;

        log_debug("Saving ClassB state to NVM");
        if (!part_write(&nvm_parts.classb, 0, &s->ClassB, sizeof(s->ClassB)))
            log_error("Error while writing ClassB state to NVM");
        nvm_flags &= ~LORAMAC_NVM_NOTIFY_FLAG_CLASS_B;
        return;
    }
}


static void restore_state(void)
{
    size_t size;
    const unsigned char *p;
    LoRaMacNvmData_t s;

    memset(&s, 0, sizeof(s));

    p = part_mmap(&size, &nvm_parts.crypto);
    if (p && size >= sizeof(s.Crypto)) memcpy(&s.Crypto, p, sizeof(s.Crypto));

    p = part_mmap(&size, &nvm_parts.mac1);
    if (p && size >= sizeof(s.MacGroup1)) memcpy(&s.MacGroup1, p, sizeof(s.MacGroup1));

    p = part_mmap(&size, &nvm_parts.mac2);
    if (p && size >= sizeof(s.MacGroup2)) memcpy(&s.MacGroup2, p, sizeof(s.MacGroup2));

    p = part_mmap(&size, &nvm_parts.se);
    if (p && size >= sizeof(s.SecureElement)) memcpy(&s.SecureElement, p, sizeof(s.SecureElement));

    p = part_mmap(&size, &nvm_parts.region1);
    if (p && size >= sizeof(s.RegionGroup1)) memcpy(&s.RegionGroup1, p, sizeof(s.RegionGroup1));

    p = part_mmap(&size, &nvm_parts.region2);
    if (p && size >= sizeof(s.RegionGroup2)) memcpy(&s.RegionGroup2, p, sizeof(s.RegionGroup2));

    p = part_mmap(&size, &nvm_parts.classb);
    if (p && size >= sizeof(s.ClassB)) memcpy(&s.ClassB, p, sizeof(s.ClassB));

    MibRequestConfirm_t r = {
        .Type = MIB_NVM_CTXS,
        .Param = { .Contexts = &s }
    };
    int rc = LoRaMacMibSetRequestConfirm(&r);
    if (rc != LORAMAC_STATUS_OK)
        log_error("LoRaMac: Error while restoring NVM state: %d", rc);
}


static uint8_t dev_eui[SE_EUI_SIZE];
static_assert(sizeof(((SecureElementNvmData_t *)0)->DevEui) == sizeof(dev_eui), "Unsupported DevEUI size found in LoRaMac-node");

static void load_deveui(void)
{
    size_t size;
    uint32_t crc;

    memset(dev_eui, '\0', sizeof(dev_eui));

    const SecureElementNvmData_t *p = part_mmap(&size, &nvm_parts.se);
    if (p == NULL) return;
    if (size < sizeof(SecureElementNvmData_t)) return;

    // Only restore the DevEUI if the crc32 checksum over the entire block
    // matches, or if the checksum calculate over the DevEui parameter matches.
    // The latter is a special case used by the factory reset command to
    // indicate that the structure has a valid DevEUI value preserved from
    // before the reset, but the entire block should not be restored. This is
    // used to re-initialize all parameters but DevEUI to defaults.

    // Read the checksum into a local variable in the case p->Crc32 isn't
    // properly aligned in memory. In the current implementation the returned
    // pointer will be properly aligned, but better be safe than sorry in case
    // that changes in the future.
    memcpy(&crc, &p->Crc32, sizeof(crc));

    if (check_block_crc(p, sizeof(SecureElementNvmData_t)) ||
        Crc32((uint8_t *)p->DevEui, sizeof(p->DevEui)) == crc) {
        memcpy(dev_eui, p->DevEui, sizeof(dev_eui));
    }
}


static int restore_region(void)
{
    size_t size;
    LoRaMacRegion_t region;
    uint32_t crc;

    const LoRaMacNvmDataGroup2_t *p = part_mmap(&size, &nvm_parts.mac2);
    if (p == NULL) goto out;
    if (size < sizeof(LoRaMacNvmDataGroup2_t)) goto out;

    // Only restore the region parameter value if the crc32 checksum over the
    // entire block matches, or if the checksum calculate over the region
    // parameter only matches. The latter is a special case used by
    // lrw_set_region to indicate that the structure has a valid region value,
    // but the entire block should not be restored. This is used to
    // re-initialize the parameters from defaults when switching regions.

    // Read the checksum into a local variable in the case p->Crc32 isn't
    // properly aligned in memory. In the current implementation the returned
    // pointer will be properly aligned, but better be safe than sorry in case
    // that changes in the future.
    memcpy(&crc, &p->Crc32, sizeof(crc));

    if (check_block_crc(p, sizeof(LoRaMacNvmDataGroup2_t)) ||
        Crc32((uint8_t *)&p->Region, sizeof(p->Region)) == crc) {
        memcpy(&region, &p->Region, sizeof(region));
        return region;
    }

out:
    return region2id(DEFAULT_ACTIVE_REGION);
}


static void state_changed(uint16_t flags)
{
    nvm_flags |= flags;
}


static void on_ack(bool ack_received)
{
    if (ack_received) {
        cmd_print("+ACK\r\n\r\n");
    } else {
        cmd_print("+NOACK\r\n\r\n");
    }
}


static void recv(uint8_t port, uint8_t *buffer, uint8_t length)
{
    atci_printf("+RECV=%d,%d\r\n\r\n", port, length);

    if (sysconf.data_format) {
        atci_print_buffer_as_hex(buffer, length);
    } else {
        atci_write((char *) buffer, length);
    }
}


static void mcps_confirm(McpsConfirm_t *param)
{
    log_debug("mcps_confirm: McpsRequest: %d, Channel: %ld AckReceived: %d", param->McpsRequest, param->Channel, param->AckReceived);
    tx_params = *param;

    if (param->McpsRequest == MCPS_CONFIRMED)
        on_ack(param->AckReceived == 1);
}


static void mcps_retransmit(void)
{
    cmd_event(CMD_EVENT_NETWORK, CMD_NET_RETRANSMISSION);
}


static void mcps_indication(McpsIndication_t *param)
{
    log_debug("mcps_indication: status: %d rssi: %d", param->Status, param->Rssi);

    if (param->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
        return;
    }

    if (param->RxData) {
        recv(param->Port, param->Buffer, param->BufferSize);
    }

    if (param->IsUplinkTxPending == true) {
        // do nothing for now
    }
}


// Copy the device class value from sys config to the MIB. The value in MIB can
// be overwritten by LoRaMac at runtime, e.g., after a Join.
static int sync_device_class(void)
{
    int rc;
    MibRequestConfirm_t r = { .Type = MIB_DEVICE_CLASS };

    rc = LoRaMacMibGetRequestConfirm(&r);
    if (rc != LORAMAC_STATUS_OK) return rc;

    if (r.Param.Class == sysconf.device_class)
        return LORAMAC_STATUS_OK;

    r.Param.Class = sysconf.device_class;
    return LoRaMacMibSetRequestConfirm(&r);
}


#ifdef LORAMAC_ABP_VERSION
static int set_abp_mac_version(void)
{
    // If we are in ABP mode and the application has defined a specific MAC
    // version to be used in this mode, set it now. There is no automatic
    // version negotiation in ABP mode, so this needs to be done manually.
    MibRequestConfirm_t r = {
        .Type = MIB_ABP_LORAWAN_VERSION,
        .Param = { .AbpLrWanVersion = { .Value = LORAMAC_ABP_VERSION }}};
    return LoRaMacMibSetRequestConfirm(&r);
}
#endif


#ifdef RESTORE_CHMASK_AFTER_JOIN
static void save_chmask(void)
{
    MibRequestConfirm_t r = { .Type = MIB_CHANNELS_DEFAULT_MASK };
    LoRaMacMibGetRequestConfirm(&r);
    memcpy(saved_chmask, r.Param.ChannelsMask, sizeof(saved_chmask));
}


static void restore_chmask(void)
{
    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_MASK,
        .Param = { .ChannelsDefaultMask = saved_chmask }
    };
    LoRaMacMibSetRequestConfirm(&r);

    r.Type = MIB_CHANNELS_MASK;
    r.Param.ChannelsMask = saved_chmask;
    LoRaMacMibSetRequestConfirm(&r);
}
#endif


static void linkcheck_callback(MlmeConfirm_t *param)
{
    if (param->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
        cmd_event(CMD_EVENT_NETWORK, CMD_NET_ANSWER);
        cmd_ans(param->DemodMargin, param->NbGateways);
    } else {
        cmd_event(CMD_EVENT_NETWORK, CMD_NET_NOANSWER);
    }
}


static void join_callback_abp(MlmeConfirm_t *param)
{
#ifdef LORAMAC_ABP_VERSION
    if (param->Status == LORAMAC_EVENT_INFO_STATUS_OK)
        set_abp_mac_version();
#endif

    // During the Join operation, LoRaMac internally switches the device class
    // to class A. Thus, we need to restore the original class from
    // sysconf.device_class here.
    sync_device_class();
}


static int send_join(void)
{
    MlmeReq_t mlme = { .Type = MLME_JOIN };
    mlme.Req.Join.NetworkActivation = ACTIVATION_TYPE_OTAA;
    mlme.Req.Join.Datarate = DR_0;
    return LoRaMacMlmeRequest(&mlme);
}


static void stop_join(unsigned int status)
{
    TimerStop(&join_retry_timer);
    joins_left = 0;

    cmd_event(CMD_EVENT_JOIN, status);

    // During the Join operation, LoRaMac internally switches the device class
    // to class A. Thus, we need to restore the original class from
    // sysconf.device_class here.
    sync_device_class();

#ifdef RESTORE_CHMASK_AFTER_JOIN
    MibRequestConfirm_t r = { .Type = MIB_NETWORK_ACTIVATION };
    LoRaMacMibGetRequestConfirm(&r);

    if (r.Param.NetworkActivation == ACTIVATION_TYPE_OTAA)
        restore_chmask();
#endif
}


static void retransmit_join(void)
{
    log_debug("Retransmitting Join");
    LoRaMacStatus_t rc = send_join();
    if (rc != LORAMAC_STATUS_OK) {
        log_error("Error while retransmitting Join (%d)", rc);
        stop_join(CMD_JOIN_FAILED);
    }
}


static void on_join_timer(void *ctx)
{
    // This handler is invoked in the ISR context within an interrupt generated
    // by the RTC. Do no work here, just set an even flag and prevent sleep so
    // that the event gets a chance to be handled on the next run of the main
    // processing function in this module.
    (void)ctx;
    system_sleep_lock |= SYSTEM_MODULE_LORA;
    events |= RETRANSMIT_JOIN;
}


static void join_callback_otaa(MlmeConfirm_t *param)
{
    joins_left--;

    // If the previous Join request timed out and we have Join retransmissions
    // left, transmit again. In all other cases, consider the Join transmission
    // to be done, stop retransmissions, and notify the application.
    if (joins_left > 0 && param->Status == LORAMAC_EVENT_INFO_STATUS_RX2_TIMEOUT) {
        // Apply a random delay before each Join retransmission, as recommended
        // in Section 7 of LoRaWAN Specification 1.1. We kind of arbitrarily
        // choose a delay between 100 ms and 500 ms.
        uint32_t delay = randr(100, 500);
        TimerSetValue(&join_retry_timer, delay);
        TimerStart(&join_retry_timer);
    } else {
        stop_join(param->Status == LORAMAC_EVENT_INFO_STATUS_OK ?
            CMD_JOIN_SUCCEEDED : CMD_JOIN_FAILED);
    }
}


static void join_callback(MlmeConfirm_t *param)
{
    MibRequestConfirm_t r = { .Type = MIB_NETWORK_ACTIVATION };
    LoRaMacMibGetRequestConfirm(&r);

    if (r.Param.NetworkActivation == ACTIVATION_TYPE_ABP)
        join_callback_abp(param);
    else
        join_callback_otaa(param);
}


static void cw_callback(MlmeConfirm_t *param)
{
    if (param->Status == LORAMAC_EVENT_INFO_STATUS_TX_TIMEOUT) {
        cmd_event(CMD_EVENT_CW, CMD_CW_END);
    }
}


static void mlme_confirm(MlmeConfirm_t *param)
{
    log_debug("mlme_confirm: MlmeRequest: %d Status: %d", param->MlmeRequest, param->Status);
    tx_params.Status = param->Status;

    switch(param->MlmeRequest) {
        case MLME_JOIN:
            join_callback(param);
            break;

        case MLME_LINK_CHECK:
            linkcheck_callback(param);
            break;

        case MLME_TXCW:
            cw_callback(param);
            break;

        default:
            break;
    }
}


static void mlme_indication(__attribute__((unused)) MlmeIndication_t *param)
{
    log_debug("MlmeIndication: MlmeIndication: %d Status: %d", param->MlmeIndication, param->Status);
}


static LoRaMacPrimitives_t primitives = {
    .MacMcpsConfirm    = mcps_confirm,
    .MacMcpsRetransmit = mcps_retransmit,
    .MacMcpsIndication = mcps_indication,
    .MacMlmeConfirm    = mlme_confirm,
    .MacMlmeIndication = mlme_indication
};


static LoRaMacCallback_t callbacks = {
    .GetBatteryLevel     = get_battery_level,
    .GetTemperatureLevel = adc_get_temperature_celsius,
    .NvmDataChange       = state_changed,
    .MacProcessNotify    = process_notify
};


static void log_device_info(void)
{
    MibRequestConfirm_t r;

    log_compose();
    log_debug("LoRaMac: Device");

    r.Type = MIB_DEV_EUI;
    LoRaMacMibGetRequestConfirm(&r);
    log_debug(" DevEUI: %02X%02X%02X%02X%02X%02X%02X%02X",
        r.Param.DevEui[0], r.Param.DevEui[1], r.Param.DevEui[2], r.Param.DevEui[3],
        r.Param.DevEui[4], r.Param.DevEui[5], r.Param.DevEui[6], r.Param.DevEui[7]);

    r.Type = MIB_DEVICE_CLASS;
    LoRaMacMibGetRequestConfirm(&r);
    log_debug(" class: %c", r.Param.Class + 'A');

    r.Type = MIB_ADR;
    LoRaMacMibGetRequestConfirm(&r);
    log_debug(" ADR: %d", r.Param.AdrEnable);

    log_finish();
}


static void log_network_info(void)
{
    MibRequestConfirm_t r;

    log_compose();
    log_debug("LoRaMac: Network");

    r.Type = MIB_PUBLIC_NETWORK;
    LoRaMacMibGetRequestConfirm(&r);
    log_debug(" public: %d", r.Param.EnablePublicNetwork);

    r.Type = MIB_NETWORK_ACTIVATION;
    LoRaMacMibGetRequestConfirm(&r);
    log_debug(" activated: ");
    switch(r.Param.NetworkActivation) {
        case ACTIVATION_TYPE_NONE: log_debug("No");   break;
        case ACTIVATION_TYPE_ABP : log_debug("ABP");  break;
        case ACTIVATION_TYPE_OTAA: log_debug("OTAA"); break;
        default: log_debug("?"); break;
    }

    if (r.Param.NetworkActivation != ACTIVATION_TYPE_NONE) {
        r.Type = MIB_LORAWAN_VERSION;
        LoRaMacMibGetRequestConfirm(&r);
        log_debug(" MAC: %d.%d.%d",
            r.Param.LrWanVersion.LoRaWan.Fields.Major,
            r.Param.LrWanVersion.LoRaWan.Fields.Minor,
            r.Param.LrWanVersion.LoRaWan.Fields.Patch);

        r.Type = MIB_NET_ID;
        LoRaMacMibGetRequestConfirm(&r);
        log_debug(" NetID: %08lX", r.Param.NetID);

        r.Type = MIB_DEV_ADDR;
        LoRaMacMibGetRequestConfirm(&r);
        log_debug(" DevAddr: %08lX", r.Param.DevAddr);
    }

    log_finish();
}


/* This function applies default settings according to the original Type ABZ
 * firmware. It is meant to be called after the MIB has been initialized from
 * the defaults built in LoRaMac-node and before settings are restored from NVM.
 */
static void set_defaults(void)
{
    // The original firmware has AppEUI set to 0101010101010101
    MibRequestConfirm_t r = {
        .Type  = MIB_JOIN_EUI,
        .Param = { .JoinEui = (uint8_t *)"\1\1\1\1\1\1\1\1" }
    };
    LoRaMacMibSetRequestConfirm(&r);

    // The original firmware has ADR enabled by default
    r.Type = MIB_ADR;
    r.Param.AdrEnable = 1;
    LoRaMacMibSetRequestConfirm(&r);

    /// The original firmware configures the TRX with 14 dBm in RFO mode
    r.Type  = MIB_CHANNELS_TX_POWER;
    r.Param.ChannelsTxPower = 1;
    LoRaMacMibSetRequestConfirm(&r);

#ifdef LORAMAC_ABP_VERSION
    // If we are in ABP mode and the application has defined a specific MAC
    // version to be used in this mode, set it now. There is no automatic
    // version negotiation in ABP mode, so this needs to be done manually.
    r.Type = MIB_ABP_LORAWAN_VERSION;
    r.Param.AbpLrWanVersion.Value = LORAMAC_ABP_VERSION;
    LoRaMacMibSetRequestConfirm(&r);
#endif

    // The original firmware configures the node in ABP mode by default
    r.Type = MIB_NETWORK_ACTIVATION;
    r.Param.NetworkActivation = ACTIVATION_TYPE_ABP;
    LoRaMacMibSetRequestConfirm(&r);

    // Disable LoRaWAN certification port by default
    r.Type = MIB_IS_CERT_FPORT_ON;
    r.Param.IsCertPortOn = false;
    LoRaMacMibSetRequestConfirm(&r);
}


void lrw_init(void)
{
    static const uint8_t zero_eui[SE_EUI_SIZE];
    LoRaMacStatus_t rc;
    MibRequestConfirm_t r;

    memset(&tx_params, 0, sizeof(tx_params));
    TimerInit(&join_retry_timer, on_join_timer);

    LoRaMacRegion_t region = restore_region();

    log_debug("LoRaMac: Initializing for region %s, regional parameters RP%03d-%d.%d.%d",
        region2str(region), REGION_VERSION >> 24, (REGION_VERSION >> 16) & 0xff,
        (REGION_VERSION >> 8) & 0xff, REGION_VERSION & 0xff);
    rc = LoRaMacInitialization(&primitives, &callbacks, region);
    switch(rc) {
        case LORAMAC_STATUS_OK:
            break;

        case LORAMAC_STATUS_PARAMETER_INVALID:
            halt("LoRaMac: Invalid initialization parameter(s)");
            break;

        case LORAMAC_STATUS_REGION_NOT_SUPPORTED:
            log_error("LoRaMac: Unsupported region %s", region2str(region));
            return;

        default:
            halt("LoRaMac: Initialization error");
            return;
    }

    set_defaults();
    restore_state();

    r.Type = MIB_SYSTEM_MAX_RX_ERROR;
    r.Param.SystemMaxRxError = 20;
    LoRaMacMibSetRequestConfirm(&r);

    sync_device_class();

    // Check if we have a DevEUI in NVM, either a stored value or a value
    // preserved from before factory reset. In both cases, the following
    // function will copy the DevEUI into the variable dev_eui. Otherwise,
    // dev_eui will be set to all zeroes.
    load_deveui();

    // If we get a DevEUI consisting of all zeroes, generate a unique one based
    // off of the MCU's unique id.
    if (!memcmp(dev_eui, zero_eui, sizeof(zero_eui)))
        system_get_unique_id(dev_eui);

    r.Type = MIB_DEV_EUI;
    r.Param.DevEui = dev_eui;
    rc = LoRaMacMibSetRequestConfirm(&r);
    if (rc != LORAMAC_STATUS_OK)
        log_error("LoRaMac: Error while setting DevEUI: %d", rc);

    log_device_info();

    r.Type = MIB_DEV_ADDR;
    LoRaMacMibGetRequestConfirm(&r);
    uint32_t devaddr = r.Param.DevAddr;

    // If we get a zero DevAddr, generate a unique one randomly in one of the
    // two prefixes allocated for experimental or private nodes:
    //   00000000/7 : 00000000 - 01ffffff
    //   02000000/7 : 02000000 - 03ffffff
    // We choose the second range in the code below to make sure that the
    // generated DevAddr cannot be all zeroes.
    // https://www.thethingsnetwork.org/docs/lorawan/prefix-assignments/
    if (devaddr == 0) {
        r.Param.DevAddr = devaddr = randr(0x02000000, 0x03ffffff);
        rc = LoRaMacMibSetRequestConfirm(&r);
        if (rc != LORAMAC_STATUS_OK)
            log_error("LoRaMac: Error while setting DevAddr: %d", rc);
    }

    log_network_info();
}


int lrw_send(uint8_t port, void *buffer, uint8_t length, bool confirmed)
{
    McpsReq_t mr;
    LoRaMacTxInfo_t txi;
    LoRaMacStatus_t rc;

    memset(&mr, 0, sizeof(mr));

    // McpsReq_t provides an attribute called Datarate through which the caller
    // can select the datarate to be used for the request. However, that value
    // will only be considered by the MAC under certain conditions, e.g., when
    // ADR is off or when the device is activated with ABP. In ther cases, the
    // value provided here is ignored and the MAC uses the MIB datarate value,
    // subject to various regional restrictions.
    //
    // We want to allow the caller to select the datarate simply by modifying
    // the MIB value without having to worry about the state of the ADR. Thus,
    // we set the Datarate parameter of the request here to the value from MIB.
    // This will allow the caller to specify the Datarate to be used both with
    // ADR on and off simply by modifying the corresponding MIB value.
    //
    // If we didn't set it to the MIB value here, the Datarate parameter would
    // be implicitly set to 0 (DR0) which would be always used when ADR is off,
    // making it possible for the caller to override the value in that case.

    MibRequestConfirm_t r = { .Type = MIB_CHANNELS_DATARATE };
    LoRaMacMibGetRequestConfirm(&r);

    rc = LoRaMacQueryTxPossible(length, &txi);
    if (rc == LORAMAC_STATUS_LENGTH_ERROR) {
        log_info("Payload too long. Sending empty frame to flush MAC commands");

        // This branch may be triggered when the caller attempts to send a
        // packet with the slowest spreading factor and there are some MAC
        // commands that need to be transmitted via the FOpts header field.
        // Since the minimum payload size with the slowest spreading factor is
        // about 11 bytes (without MAC commands), it is easy for the optional
        // MAC commands to exhaust most of the available space.
        //
        // Sertting the port to 0, the payload buffer to NULL, and buffer size
        // to 0 will send an uplink message with FOpts but no port or payload.

        // Disable retransmissions for the internally generated flush uplink
        // message.
        r.Type = MIB_CHANNELS_NB_TRANS;
        r.Param.ChannelsNbTrans = 1;
        rc = LoRaMacMibSetRequestConfirm(&r);
        if (rc != LORAMAC_STATUS_OK) {
            log_debug("Could not configure retransmissions: %d", rc);
            return rc;
        }

        mr.Type = MCPS_UNCONFIRMED;
        mr.Req.Unconfirmed.fPort = 0;
        mr.Req.Unconfirmed.fBuffer = NULL;
        mr.Req.Unconfirmed.fBufferSize = 0;
        mr.Req.Unconfirmed.Datarate = r.Param.ChannelsDatarate;
        LoRaMacMcpsRequest(&mr);

        // Return the original status to the caller to indicate that we haven't
        // sent the requested payload.
        return rc;
    }

    if (port != 0 && length == 0) {
        // Messages with empty payload and non-zero port cannot be reliably sent
        // by LoRaMac. If LoRaMAC has any MAC commands pending (which can
        // happeny any time), it would put the MAC commands into the payload and
        // change the port number to 0 automatically.
        log_warning("LoRaMac cannot reliably send empty payload to non-zero port");
        return LORAMAC_STATUS_LENGTH_ERROR;
    }

    if (confirmed == false) {
        mr.Type = MCPS_UNCONFIRMED;
        mr.Req.Unconfirmed.fPort = port;
        mr.Req.Unconfirmed.fBufferSize = length;
        mr.Req.Unconfirmed.fBuffer = buffer;
        mr.Req.Unconfirmed.Datarate = r.Param.ChannelsDatarate;
    } else {
        mr.Type = MCPS_CONFIRMED;
        mr.Req.Confirmed.fPort = port;
        mr.Req.Confirmed.fBufferSize = length;
        mr.Req.Confirmed.fBuffer = buffer;
        mr.Req.Confirmed.Datarate = r.Param.ChannelsDatarate;
    }

    r.Type = MIB_CHANNELS_NB_TRANS;
    r.Param.ChannelsNbTrans = confirmed
        ? sysconf.confirmed_retransmissions
        : sysconf.unconfirmed_retransmissions;
    rc = LoRaMacMibSetRequestConfirm(&r);
    if (rc != LORAMAC_STATUS_OK) {
        log_debug("Could not configure retransmissions: %d", rc);
        return rc;
    }

    rc = LoRaMacMcpsRequest(&mr);
    if (rc != LORAMAC_STATUS_OK)
        log_debug("Transmission failed: %d", rc);

    return rc;
}


void lrw_process()
{
    uint32_t mask = disable_irq();
    unsigned ev = events;
    events = NO_EVENT;

    system_sleep_lock &= ~SYSTEM_MODULE_LORA;
    reenable_irq(mask);

    if (ev & RETRANSMIT_JOIN) retransmit_join();

    if (Radio.IrqProcess != NULL) Radio.IrqProcess();
    LoRaMacProcess();
    save_state();
}


LoRaMacNvmData_t *lrw_get_state()
{
    MibRequestConfirm_t r = { .Type = MIB_NVM_CTXS };
    LoRaMacMibGetRequestConfirm(&r);
    return r.Param.Contexts;
}


int lrw_join(unsigned int retries)
{
    // If we are already transmitting a Join request, abort the request. Do this
    // check in both ABP and OTAA modes. We don't let the application to switch
    // to ABP while there is an active OTAA Join request.
    if (joins_left != 0)
        return LORAMAC_STATUS_BUSY;

    MibRequestConfirm_t r = { .Type = MIB_NETWORK_ACTIVATION };
    LoRaMacMibGetRequestConfirm(&r);

    if (r.Param.NetworkActivation == ACTIVATION_TYPE_ABP) {
        // In ABP mode the number of retransmissions must always be set to 0
        // since no actual Join request will be sent to the LNS.
        if (retries != 0)
            return LORAMAC_STATUS_PARAMETER_INVALID;

        // LoRaMac uses the same approach for both types of activation. In ABP
        // one still needs to invoke MLME_JOIN, although no actual Join will be
        // sent. The library will simply use the opportunity to perform internal
        // initialization.
        MlmeReq_t mlme = { .Type = MLME_JOIN };
        mlme.Req.Join.NetworkActivation = ACTIVATION_TYPE_ABP;
        return LoRaMacMlmeRequest(&mlme);
    } else {
        if (retries > 15)
            return LORAMAC_STATUS_PARAMETER_INVALID;

#ifdef RESTORE_CHMASK_AFTER_JOIN
        save_chmask();
#endif
        joins_left = retries + 1;
        return send_join();
    }
}


int lrw_set_region(unsigned int region)
{
    if (!RegionIsActive(region))
        return LORAMAC_STATUS_REGION_NOT_SUPPORTED;

    // Store the new region id in the NVM state in group MacGroup2
    LoRaMacNvmData_t *state = lrw_get_state();

    // Region did not change, nothing to do
    if (region == state->MacGroup2.Region) return -1;

    // The following function deactivates the MAC, the radio, and initializes
    // the MAC parameters to defaults.
    int rv = LoRaMacDeInitialization();
    if (rv != LORAMAC_STATUS_OK) return rv;

    // The crypto group needs special handling to preserve the DevNonce value
    // across the partial factory reset performed here.
    uint16_t nonce = state->Crypto.DevNonce;
    LoRaMacCryptoInit(&state->Crypto);
    state->Crypto.DevNonce = nonce;
    update_block_crc(&state->Crypto, sizeof(state->Crypto));

    // Reset all other configuration parameters except the secure element. Note
    // that we intentionally do not recompute the CRC32 checksums here (except
    // for MacGroup2) since we don't want the state to be reloaded upon reboot.
    // We want the LoRaMac to initialize itself from defaults.
    memset(&state->MacGroup1, 0, sizeof(state->MacGroup1));
    memset(&state->MacGroup2, 0, sizeof(state->MacGroup2));
    memset(&state->RegionGroup1, 0, sizeof(state->RegionGroup1));
    memset(&state->RegionGroup2, 0, sizeof(state->RegionGroup2));
    memset(&state->ClassB, 0, sizeof(state->ClassB));

    // Update the region and regenerate the CRC for this block so that the
    // region will be picked up upon reboot.
    state->MacGroup2.Region = region;

    // We don't want to restore the entire MacGroup2 on the next reboot, but we
    // do want to restore the region parameter. Thus, calculate the CRC32 value
    // only over the region field and save it into the Crc32 parameter in the
    // structure. That way, the checksum will fail for the entire structure, but
    // the function that retrieves the region from it will additional check if
    // the checksum matches the region parameter and if yes, reload it.
    state->MacGroup2.Crc32 = Crc32(&state->MacGroup2.Region, sizeof(state->MacGroup2.Region));

    // Save all reset parameters in non-volatile memory.
    state_changed(
        LORAMAC_NVM_NOTIFY_FLAG_CRYPTO        |
        LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP1    |
        LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP2    |
        LORAMAC_NVM_NOTIFY_FLAG_REGION_GROUP1 |
        LORAMAC_NVM_NOTIFY_FLAG_REGION_GROUP2 |
        LORAMAC_NVM_NOTIFY_FLAG_CLASS_B);

    return LORAMAC_STATUS_OK;
}


unsigned int lrw_get_mode(void)
{
    MibRequestConfirm_t r = { .Type = MIB_NETWORK_ACTIVATION };
    LoRaMacMibGetRequestConfirm(&r);

    switch(r.Param.NetworkActivation) {
        case ACTIVATION_TYPE_NONE: // If the value is None, we are in OTAA mode prior to Join
        case ACTIVATION_TYPE_OTAA:
            return 1;

        case ACTIVATION_TYPE_ABP:
        default:
            return 0;
    }
}


int lrw_set_mode(unsigned int mode)
{
    if (mode > 1) return LORAMAC_STATUS_PARAMETER_INVALID;

    MibRequestConfirm_t r = { .Type = MIB_NETWORK_ACTIVATION };
    LoRaMacMibGetRequestConfirm(&r);

    if (mode == 0) {
        // ABP mode. Invoke lrw_join right away. No Join will be sent, but the
        // library will perform any necessary internal initialization.

        // If we are in ABP mode already, there is nothing to do
        if (r.Param.NetworkActivation != ACTIVATION_TYPE_ABP) {
            r.Type = MIB_NETWORK_ACTIVATION;
            r.Param.NetworkActivation = ACTIVATION_TYPE_ABP;
            LoRaMacMibSetRequestConfirm(&r);
            return lrw_join(0);
        }
    } else {
        if (r.Param.NetworkActivation != ACTIVATION_TYPE_OTAA) {
            // If we are in ABP mode or have no activation mode, set the mode to
            // none util a Join is executed.
            r.Param.NetworkActivation = ACTIVATION_TYPE_NONE;
            return LoRaMacMibSetRequestConfirm(&r);
        }
    }

    return LORAMAC_STATUS_OK;
}


void lrw_set_maxeirp(unsigned int maxeirp)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    state->MacGroup2.MacParams.MaxEirp = maxeirp;
    state->MacGroup2.MacParamsDefaults.MaxEirp = maxeirp;
    state->MacGroup2.Crc32 = Crc32((uint8_t *)&state->MacGroup2, sizeof(state->MacGroup2) - 4);
    state_changed(LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP2);
}


int lrw_set_dwell(bool uplink, bool downlink)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    state->MacGroup2.MacParams.UplinkDwellTime = uplink == true ? 1 : 0;
    state->MacGroup2.MacParams.DownlinkDwellTime = downlink == true ? 1 : 0;
    state->MacGroup2.Crc32 = Crc32((uint8_t *)&state->MacGroup2, sizeof(state->MacGroup2) - 4);
    state_changed(LORAMAC_NVM_NOTIFY_FLAG_MAC_GROUP2);
    return 0;
}


int lrw_check_link(bool piggyback)
{
    LoRaMacStatus_t rc;
    MlmeReq_t mlr = { .Type = MLME_LINK_CHECK };

    rc = LoRaMacMlmeRequest(&mlr);
    if (rc != LORAMAC_STATUS_OK) {
        log_debug("Link check request failed: %d", rc);
        return rc;
    }

    if (!piggyback) {
        MibRequestConfirm_t mbr = { .Type = MIB_CHANNELS_DATARATE };
        LoRaMacMibGetRequestConfirm(&mbr);

        // Send an empty frame to piggy-back the link check operation on
        McpsReq_t mcr;
        memset(&mcr, 0, sizeof(mcr));
        mcr.Type = MCPS_UNCONFIRMED;
        // See the comments in lrw_send on why the following parameter is set to
        // the value from MIB
        mcr.Req.Unconfirmed.Datarate = mbr.Param.ChannelsDatarate;

        rc = LoRaMacMcpsRequest(&mcr);
        if (rc != LORAMAC_STATUS_OK)
            log_debug("Empty frame TX failed: %d", rc);
    }

    return rc;
}


DeviceClass_t lrw_get_class(void)
{
    return sysconf.device_class;
}


int lrw_set_class(DeviceClass_t device_class)
{
    sysconf.device_class = device_class;
    sysconf_modified = true;
    return sync_device_class();
}


int lrw_get_chmask_length(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();

    // If there is a better way to translate a region to a channel mask size, I
    // have not found it. It's a bit unfortunate that we have to duplicate the
    // code from RegionNvm.h here, but there appears to be no other way.
    switch (state->MacGroup2.Region) {
        case LORAMAC_REGION_CN470:
        case LORAMAC_REGION_US915:
        case LORAMAC_REGION_AU915:
            return 6;

        default:
            return 1;
    }
}


lrw_channel_list_t lrw_get_channel_list(void)
{
    lrw_channel_list_t result;
    LoRaMacNvmData_t *state = lrw_get_state();
    MibRequestConfirm_t r;

    result.chmask_length = lrw_get_chmask_length();

    GetPhyParams_t req = { .Attribute = PHY_MAX_NB_CHANNELS };
    PhyParam_t resp = RegionGetPhyParam(state->MacGroup2.Region, &req);
    result.length = resp.Value;

    r.Type = MIB_CHANNELS;
    LoRaMacMibGetRequestConfirm(&r);
    result.channels = r.Param.ChannelList;

    r.Type = MIB_CHANNELS_MASK;
    LoRaMacMibGetRequestConfirm(&r);
    result.chmask = r.Param.ChannelsMask;

    r.Type = MIB_CHANNELS_DEFAULT_MASK;
    LoRaMacMibGetRequestConfirm(&r);
    result.chmask_default = r.Param.ChannelsDefaultMask;

    return result;
}
