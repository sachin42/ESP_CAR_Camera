#include <stdio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>
#include <driver/ledc.h>
#include <esp_http_server.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "homepage.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WIFI_SSID "ESP32-CAM-AP"
#define WIFI_PASS "12345678"
#define MAX_STA_CONN 4

#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1 // software reset will be performed
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27

#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

#define LEDC_TIMER LEDC_TIMER_1
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO (4) // Define the output GPIO
#define LED_CHANNEL LEDC_CHANNEL_2

#define LEDC_DUTY_RES LEDC_TIMER_8_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY (4096)               // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LEDC_FREQUENCY (4000)          // Frequency in Hertz. Set frequency at 4 kHz

static const char *TAG = "Camera";

#define ASYNC_WORKER_TASK_PRIORITY 5
#define ASYNC_WORKER_TASK_STACK_SIZE 8192
#define CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS 2

static QueueHandle_t async_req_queue;
static SemaphoreHandle_t worker_ready_count;
static TaskHandle_t worker_handles[CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS];
typedef esp_err_t (*httpd_req_handler_t)(httpd_req_t *req);
typedef struct
{
    httpd_req_t *req;
    httpd_req_handler_t handler;
} httpd_async_req_t;

extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");

static bool is_on_async_worker_thread(void)
{
    // is our handle one of the known async handles?
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS; i++)
    {
        if (worker_handles[i] == handle)
        {
            return true;
        }
    }
    return false;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        printf("Disconnected. Reconnecting...\n\n");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        printf("connected to SSID: %s \n\n", event->ssid);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        printf("Connected! IP Address: " IPSTR "\n\n", IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t submit_async_req(httpd_req_t *req, httpd_req_handler_t handler)
{
    // must create a copy of the request that we own
    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK)
    {
        return err;
    }

    httpd_async_req_t async_req = {
        .req = copy,
        .handler = handler,
    };

    // How should we handle resource exhaustion?
    // In this example, we immediately respond with an
    // http error if no workers are available.
    int ticks = 0;

    // counting semaphore: if success, we know 1 or
    // more asyncReqTaskWorkers are available.
    if (xSemaphoreTake(worker_ready_count, ticks) == false)
    {
        ESP_LOGE(TAG, "No workers are available");
        httpd_req_async_handler_complete(copy); // cleanup
        return ESP_FAIL;
    }

    // Since worker_ready_count > 0 the queue should already have space.
    // But lets wait up to 100ms just to be safe.
    if (xQueueSend(async_req_queue, &async_req, pdMS_TO_TICKS(100)) == false)
    {
        ESP_LOGE(TAG, "worker queue is full");
        httpd_req_async_handler_complete(copy); // cleanup
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void async_req_worker_task(void *p)
{
    while (true)
    {

        // counting semaphore - this signals that a worker
        // is ready to accept work
        xSemaphoreGive(worker_ready_count);

        // wait for a request
        httpd_async_req_t async_req;
        if (xQueueReceive(async_req_queue, &async_req, portMAX_DELAY))
        {

            printf("invoking %s\n", async_req.req->uri);

            // call the handler
            async_req.handler(async_req.req);

            // Inform the server that it can purge the socket used for
            // this request, if needed.
            if (httpd_req_async_handler_complete(async_req.req) != ESP_OK)
            {
                ESP_LOGE(TAG, "failed to complete async req");
            }
        }
    }

    ESP_LOGW(TAG, "worker stopped");
    vTaskDelete(NULL);
}

static void start_async_req_workers(void)
{
    // counting semaphore keeps track of available workers
    worker_ready_count = xSemaphoreCreateCounting(
        CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS, // Max Count
        0);                                // Initial Count
    if (worker_ready_count == NULL)
    {
        ESP_LOGE(TAG, "Failed to create workers counting Semaphore");
        return;
    }

    // create queue
    async_req_queue = xQueueCreate(1, sizeof(httpd_async_req_t));
    if (async_req_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create async_req_queue");
        vSemaphoreDelete(worker_ready_count);
        return;
    }

    // start worker tasks
    for (int i = 0; i < CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS; i++)
    {
        bool success = xTaskCreate(async_req_worker_task, "async_req_worker",
                                   ASYNC_WORKER_TASK_STACK_SIZE, // stack size
                                   (void *)0,                    // argument
                                   ASYNC_WORKER_TASK_PRIORITY,   // priority
                                   &worker_handles[i]);

        if (!success)
        {
            ESP_LOGE(TAG, "Failed to start asyncReqWorker");
            continue;
        }
    }
}

static esp_err_t init_camera(void)
{
    static camera_config_t camera_config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        // XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG, // YUV422,GRAYSCALE,RGB565,JPEG
        .frame_size = FRAMESIZE_VGA,    // QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

        .jpeg_quality = 5, // 0-63, for OV series camera sensors, 0er number means 1er quality
        .fb_count = 4,     // When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

static esp_err_t jpg_stream_httpd_handler(httpd_req_t *req)
{
    if (is_on_async_worker_thread() == false)
    {
        // submit
        if (submit_async_req(req, jpg_stream_httpd_handler) == ESP_OK)
        {
            return ESP_OK;
        }
        else
        {
            httpd_resp_set_status(req, "503 Busy");
            httpd_resp_sendstr(req, "<div> no workers available. server busy.</div>");
            return ESP_OK;
        }
    }

    if (req->method == HTTP_GET)
    {
        if (httpd_req_to_sockfd(req) < 0)
        {
            ESP_LOGE(TAG, "Failed to get socket for WebSocket");
            return ESP_FAIL;
        }
    }

    // Upgrade to WebSocket
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    while (true)
    {
        // Capture a frame
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGE(TAG, "Failed to capture frame");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Send frame as WebSocket binary data
        ws_pkt.payload = fb->buf;
        ws_pkt.len = fb->len;

        esp_err_t ret = httpd_ws_send_frame(req, &ws_pkt);
        esp_camera_fb_return(fb);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "WebSocket send failed: %s", esp_err_to_name(ret));
            break;
        }

        // Control the frame rate
        vTaskDelay(pdMS_TO_TICKS(25)); // Adjust for desired frame rate
    }

    printf("WebSocket client disconnected\n\n");
    ledc_set_duty(LEDC_MODE, LED_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LED_CHANNEL);
    return ESP_OK;
}

static esp_err_t control_handler(httpd_req_t *req)
{
    char buf[100];
    int ret, light_value;

    // Read the data sent by the client
    ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    { // Error handling
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        else
        {
            ESP_LOGW("LIGHT", "Socket error during recv: %d", ret);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0'; // Null-terminate the received string

    // ESP_LOGI("Control", "Recived %s", buf);

    // Parse the light value
    if (sscanf(buf, "Light=%d", &light_value) == 1)
    {
        printf("Light value set to: %d\n\n", light_value);
        ledc_set_duty(LEDC_MODE, LED_CHANNEL, light_value);
        ledc_update_duty(LEDC_MODE, LED_CHANNEL);
    }

    // Respond to the client
    httpd_resp_sendstr(req, "Response Received");
    return ESP_OK;
}

static esp_err_t http_server_favicon_ico_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_end - favicon_ico_start);

    return ESP_OK;
}

static esp_err_t index_httpd_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, htmlHomePage, strlen(htmlHomePage));
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS + 3;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_httpd_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = jpg_stream_httpd_handler,
            .user_ctx = NULL,
            .is_websocket = true,
        };
        httpd_register_uri_handler(server, &stream_uri);

        httpd_uri_t set_light_uri = {
            .uri = "/control",
            .method = HTTP_POST,
            .handler = control_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &set_light_uri);

        httpd_uri_t favicon_ico = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = http_server_favicon_ico_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &favicon_ico);
    }
    return server;
}

static void init_wifi_ap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        }};

    wifi_config_t sta_config = {
        .sta = {
            .ssid = "project",
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .failure_retry_cnt = 5,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
        }};

    if (strlen(WIFI_PASS) == 0)
    {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("WiFi AP started. SSID: %s, Password: %s\n\n", WIFI_SSID, WIFI_PASS);
}

static void setupPins()
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY, // Set output frequency at 4 kHz
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LED_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LEDC_OUTPUT_IO,
        .duty = 0, // Set duty to 0%
        .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());                // Initialize NVS for Wi-Fi storage
    ESP_ERROR_CHECK(esp_netif_init());                // Initialize network interface
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Initialize default event loop

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    init_wifi_ap();

    if (ESP_OK != init_camera())
    {
        return;
    }
    setupPins();
    start_async_req_workers();
    start_webserver();
}
