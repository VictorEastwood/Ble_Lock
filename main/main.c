/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "esp_log.h"
#include "nvs_flash.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "main.h"
#include "driver/uart.h"
#include "driver/ledc.h"

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          (2) // Define the output GPIO
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_14_BIT // Set duty resolution to 14 bits
#define LEDC_DUTY               (1229) // Set duty to 50%. (2 ** 14) * 50% = 8192
#define LEDC_FREQUENCY          (50) // Frequency in Hertz. Set frequency at 50 Hz

static int ble_spp_server_gap_event(struct ble_gap_event *event, void *arg);
static uint8_t own_addr_type;
int gatt_svr_register(void);
QueueHandle_t spp_common_uart_queue = NULL;
static bool conn_handle_subs[CONFIG_BT_NIMBLE_MAX_CONNECTIONS + 1];
static uint16_t ble_spp_svc_gatt_read_val_handle;
static void send_welcome_message(uint16_t conn_handle);// Flag to indicate if the door is locked or not
void ble_store_config_init(void);
static void local_ledc_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}
static void local_ledc_set_duty(uint32_t duty)
{
    // Set the duty cycle for the LEDC channel
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    // Update the LEDC channel with the new duty cycle
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}
/**
 * Logs information about a connection to the console.
 */
static void
ble_spp_server_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    MODLOG_DFLT(INFO, "handle=%d our_ota_addr_type=%d our_ota_addr=",
                desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    MODLOG_DFLT(INFO, " our_id_addr_type=%d our_id_addr=",
                desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    MODLOG_DFLT(INFO, " peer_ota_addr_type=%d peer_ota_addr=",
                desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    MODLOG_DFLT(INFO, " peer_id_addr_type=%d peer_id_addr=",
                desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    MODLOG_DFLT(INFO, " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                      "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency,
                desc->supervision_timeout,
                desc->sec_state.encrypted,
                desc->sec_state.authenticated,
                desc->sec_state.bonded);
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void
ble_spp_server_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]){
        BLE_UUID16_INIT(BLE_SVC_SPP_UUID16)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return;
    }

    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_spp_server_gap_event, NULL);
    if (rc != 0)
    {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
        return;
    }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * ble_spp_server uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param ctxt                  Various information pertaining to the event.
 * @param arg                   Application-specified argument; unused by
 *                                  ble_spp_server.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int
ble_spp_server_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type)
    {
    case BLE_GAP_EVENT_LINK_ESTAB:
        /* A new connection was established or a connection attempt failed. */
        MODLOG_DFLT(INFO, "connection %s; status=%d ",
                    event->link_estab.status == 0 ? "established" : "failed",
                    event->link_estab.status);
        if (event->link_estab.status == 0)
        {
            rc = ble_gap_conn_find(event->link_estab.conn_handle, &desc);
            assert(rc == 0);
            ble_spp_server_print_conn_desc(&desc);

            /* Send welcome message when connection is established */
            vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to ensure connection is ready
            send_welcome_message(event->link_estab.conn_handle);
        }
        MODLOG_DFLT(INFO, "\n");
        if (event->link_estab.status != 0 || CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 1)
        {
            /* Connection failed or if multiple connection allowed; resume advertising. */
            ble_spp_server_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        ble_spp_server_print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");

        conn_handle_subs[event->disconnect.conn.conn_handle] = false;

        /* Connection terminated; resume advertising. */
        ble_spp_server_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        MODLOG_DFLT(INFO, "connection updated; status=%d ",
                    event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        ble_spp_server_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "advertise complete; reason=%d",
                    event->adv_complete.reason);
        ble_spp_server_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        MODLOG_DFLT(INFO, "subscribe event; conn_handle=%d attr_handle=%d "
                          "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.reason,
                    event->subscribe.prev_notify,
                    event->subscribe.cur_notify,
                    event->subscribe.prev_indicate,
                    event->subscribe.cur_indicate);
        conn_handle_subs[event->subscribe.conn_handle] = true;

        /* Send welcome message when client subscribes to notifications */
        if (event->subscribe.cur_notify)
        {
            vTaskDelay(pdMS_TO_TICKS(50)); // Small delay
            send_welcome_message(event->subscribe.conn_handle);
        }
        return 0;

    default:
        return 0;
    }
}

static void
ble_spp_server_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
ble_spp_server_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0)
    {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);

    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");
    /* Begin advertising. */
    ble_spp_server_advertise();
}

/* Function to send welcome message to client */
static void send_welcome_message(uint16_t conn_handle)
{
    const char *welcome_msg =
        "◈═════◈═════◈\n"
        "喂！旅行者！你终于回来了！\n"
        "派蒙需要确认你的身份～\n"
        "快说出口令，不然不让你进！\n"
        "◈═════◈═════◈\n"
        "> ";

    struct os_mbuf *txom;
    txom = ble_hs_mbuf_from_flat((const uint8_t *)welcome_msg, strlen(welcome_msg));

    int rc = ble_gatts_notify_custom(conn_handle, ble_spp_svc_gatt_read_val_handle, txom);
    if (rc == 0)
    {
        MODLOG_DFLT(INFO, "Welcome message sent successfully to conn_handle=%d", conn_handle);
    }
    else
    {
        MODLOG_DFLT(INFO, "Error sending welcome message, rc=%d", rc);
    }
}

void ble_spp_server_host_task(void *param)
{
    MODLOG_DFLT(INFO, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

static void open_door(void)
{
    MODLOG_DFLT(INFO, "Opening door...");
    local_ledc_set_duty(1229-400);
    //Servo Angle 1
    vTaskDelay(pdMS_TO_TICKS(1000)); // Simulate door opening delay
    //Servo Angle 0
    local_ledc_set_duty(1229);
    MODLOG_DFLT(INFO, "Door opened successfully!");
    // Here you can add code to control the door lock mechanism
}
static int password_check(uint16_t conn_handle, const char *received_password)
{
    const char *correct_password = "200296"; // 正确的密码
    const char *response;

    if (strcmp(received_password, correct_password) == 0)
    {
        response =
            "\n"
            "◈═════◈═════◈\n"
            "🎉 口令正确！ (＾▽＾)\n"
            "不愧是派蒙最好的伙伴！\n"
            "正在开门...应急食品已就位！\n"
            "◈═════◈═════◈\n";

        MODLOG_DFLT(INFO, "Password correct");
        open_door();
    }
    else
    {
        response = "\n"
                   "◈═════◈═════◈\n"
                   "❌ 喂！不对不对！(╯°□°)╯\n"
                   "派蒙可不记得设过这个密码！\n"
                   "再试一次，不然不给你晚饭吃！\n"
                   "◈═════◈═════◈\n"
                   "> ";
        MODLOG_DFLT(INFO, "Password incorrect");
    }

    // 将结果通过 BLE 通知客户端
    struct os_mbuf *txom;
    txom = ble_hs_mbuf_from_flat((const uint8_t *)response, strlen(response));
    int rc = ble_gatts_notify_custom(conn_handle, ble_spp_svc_gatt_read_val_handle, txom);
    if (rc != 0)
    {
        MODLOG_DFLT(ERROR, "Failed to send response, rc=%d", rc);
    }

    return strcmp(received_password, correct_password) == 0;
}

/* Callback function for custom service */
static int ble_svc_gatt_handler(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        MODLOG_DFLT(INFO, "Callback for read");
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        MODLOG_DFLT(INFO, "Data received in write event,conn_handle = %x,attr_handle = %x", conn_handle, attr_handle);
        // Echo received data back to client for now
        if (ctxt->om->om_len > 0)
        {
            // struct os_mbuf *txom;
            // txom = ble_hs_mbuf_from_flat(ctxt->om->om_data, ctxt->om->om_len);
            ble_hs_mbuf_from_flat(ctxt->om->om_data, ctxt->om->om_len);
            // ble_gatts_notify_custom(conn_handle, ble_spp_svc_gatt_read_val_handle, txom);
            // 将接收到的密码打印到终端
            // MODLOG_DFLT(INFO, "Received data: %.*s", ctxt->om->om_len, (char *)ctxt->om->om_data);
            // check if the password is correct
            char received_password[20] = {0};
            memcpy(received_password, ctxt->om->om_data, ctxt->om->om_len);
            password_check(conn_handle, received_password);
        }
        break;

    default:
        MODLOG_DFLT(INFO, "\nDefault Callback");
        break;
    }
    return 0;
}

/* Define new custom service */
static const struct ble_gatt_svc_def new_ble_svc_gatt_defs[] = {
    {
        /*** Service: SPP */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]){{
                                                           /* Support SPP service */
                                                           .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_CHR_UUID16),
                                                           .access_cb = ble_svc_gatt_handler,
                                                           .val_handle = &ble_spp_svc_gatt_read_val_handle,
                                                           .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                                                       },
                                                       {
                                                           0, /* No more characteristics */
                                                       }},
    },
    {
        0, /* No more services. */
    },
};

static void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op)
    {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering characteristic %s with "
                           "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

int gatt_svr_init(void)
{
    int rc = 0;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(new_ble_svc_gatt_defs);

    if (rc != 0)
    {
        return rc;
    }

    rc = ble_gatts_add_svcs(new_ble_svc_gatt_defs);
    if (rc != 0)
    {
        return rc;
    }

    return 0;
}

void ble_server_uart_task(void *pvParameters)
{
    MODLOG_DFLT(INFO, "BLE server UART_task started\n");
    uart_event_t event;
    int rc = 0;
    for (;;)
    {
        // Waiting for UART event.
        if (xQueueReceive(spp_common_uart_queue, (void *)&event, (TickType_t)portMAX_DELAY))
        {
            switch (event.type)
            {
            // Event of UART receiving data
            case UART_DATA:
                if (event.size)
                {
                    uint8_t *ntf;
                    ntf = (uint8_t *)malloc(sizeof(uint8_t) * event.size);
                    memset(ntf, 0x00, event.size);
                    uart_read_bytes(UART_NUM_0, ntf, event.size, portMAX_DELAY);

                    for (int i = 0; i <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++)
                    {
                        /* Check if client has subscribed to notifications */
                        if (conn_handle_subs[i])
                        {
                            struct os_mbuf *txom;
                            txom = ble_hs_mbuf_from_flat(ntf, event.size);
                            rc = ble_gatts_notify_custom(i, ble_spp_svc_gatt_read_val_handle,
                                                         txom);
                            if (rc == 0)
                            {
                                MODLOG_DFLT(INFO, "Notification sent successfully");
                            }
                            else
                            {
                                MODLOG_DFLT(INFO, "Error in sending notification rc = %d", rc);
                            }
                        }
                    }

                    free(ntf);
                }
                break;
            default:
                break;
            }
        }
    }
    vTaskDelete(NULL);
}
static void ble_spp_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_RTS,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // Install UART driver, and get the queue.
    uart_driver_install(UART_NUM_0, 4096, 8192, 10, &spp_common_uart_queue, 0);
    // Set UART parameters
    uart_param_config(UART_NUM_0, &uart_config);
    // Set UART pins
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    xTaskCreate(ble_server_uart_task, "uTask", 4096, (void *)UART_NUM_0, 8, NULL);
}

void app_main(void)
{
    int rc;

    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    local_ledc_init();
    local_ledc_set_duty(1229);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK)
    {
        MODLOG_DFLT(ERROR, "Failed to init nimble %d \n", ret);
        return;
    }

    /* Initialize connection_handle array */
    for (int i = 0; i <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++)
    {
        conn_handle_subs[i] = false;
    }

    /* Initialize uart driver and start uart task */
    ble_spp_uart_init();

    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = ble_spp_server_on_reset;
    ble_hs_cfg.sync_cb = ble_spp_server_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = CONFIG_EXAMPLE_IO_TYPE;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#endif
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_our_key_dist = 1;
    ble_hs_cfg.sm_their_key_dist = 1;
#endif

    /* Register custom service */
    rc = gatt_svr_init();
    assert(rc == 0);

    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("ESP32_BLE_LOCK");
    assert(rc == 0);

    /* XXX Need to have template for store */
    ble_store_config_init();

    nimble_port_freertos_init(ble_spp_server_host_task);
}
