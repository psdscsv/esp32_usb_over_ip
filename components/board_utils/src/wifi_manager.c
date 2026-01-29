#include "wifi_manager.h"
#include "web_page.h"
#include <sys/param.h>

static const char *TAG = "WiFiManager";

// 全局变量
static EventGroupHandle_t s_wifi_event_group = NULL;
static httpd_handle_t s_http_server = NULL;
static char s_wifi_list_buffer[1024] = "";
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_ap_mode_active = false;
static bool s_connect_attempted = false;

// 事件组位定义
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_AP_STARTED_BIT BIT2
#define WIFI_SCAN_DONE_BIT BIT3

// URL解码函数
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    char a, b;
    size_t i = 0, j = 0;

    while (src[i] && j < dst_size - 1)
    {
        if (src[i] == '%' && src[i + 1] && src[i + 2])
        {
            a = src[i + 1];
            b = src[i + 2];

            if (a >= '0' && a <= '9')
                a -= '0';
            else if (a >= 'a' && a <= 'f')
                a = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F')
                a = a - 'A' + 10;

            if (b >= '0' && b <= '9')
                b -= '0';
            else if (b >= 'a' && b <= 'f')
                b = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F')
                b = b - 'A' + 10;

            dst[j++] = 16 * a + b;
            i += 3;
        }
        else if (src[i] == '+')
        {
            dst[j++] = ' ';
            i++;
        }
        else
        {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// 启动AP模式
static void wifi_start_ap(void)
{
    ESP_LOGI(TAG, "启动AP模式...");

    // 如果AP已经启动，先停止
    if (s_ap_mode_active)
    {
        ESP_LOGI(TAG, "AP模式已启动，无需重复启动");
        return;
    }

    // 设置为AP模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 配置AP参数
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(CONFIG_AP_SSID),
            .channel = 1,
            .authmode = strlen(CONFIG_AP_PASS) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .max_connection = 4,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strncpy((char *)wifi_config.ap.ssid, CONFIG_AP_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, CONFIG_AP_PASS, sizeof(wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    s_ap_mode_active = true;
    ESP_LOGI(TAG, "AP模式启动完成，SSID: %s", CONFIG_AP_SSID);
    ESP_LOGI(TAG, "AP IP地址: 192.168.4.1");
}

// 停止AP模式
static void wifi_stop_ap(void)
{
    if (!s_ap_mode_active)
    {
        return;
    }

    ESP_LOGI(TAG, "停止AP模式...");

    // 关闭Web服务器
    if (s_http_server)
    {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        ESP_LOGI(TAG, "Web服务器已停止");
    }

    // 设置为STA模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_mode_active = false;

    ESP_LOGI(TAG, "AP模式已停止");
}

// WiFi事件处理
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP模式启动成功");
            xEventGroupSetBits(s_wifi_event_group, WIFI_AP_STARTED_BIT);
            s_ap_mode_active = true;
            break;

        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "设备连接: MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "设备断开: MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            break;
        }

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA模式启动");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi连接断开");
            if (!s_ap_mode_active)
            {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "连接到AP");
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "获取到IP地址: " IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "网关: " IPSTR, IP2STR(&event->ip_info.gw));
            ESP_LOGI(TAG, "子网掩码: " IPSTR, IP2STR(&event->ip_info.netmask));

            // 停止AP模式
            if (s_ap_mode_active)
            {
                ESP_LOGI(TAG, "STA连接成功，停止AP模式");
                wifi_stop_ap();
            }

            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        }

        case IP_EVENT_AP_STAIPASSIGNED:
            ESP_LOGI(TAG, "为客户端分配IP");
            break;
        }
    }
}

// 扫描WiFi网络
static bool wifi_scan_and_update_list(void)
{
    ESP_LOGI(TAG, "开始扫描WiFi网络...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi扫描失败: %s", esp_err_to_name(err));
        snprintf(s_wifi_list_buffer, sizeof(s_wifi_list_buffer),
                 "<option>扫描失败</option>");
        return false;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    if (ap_count == 0)
    {
        ESP_LOGW(TAG, "未发现WiFi网络");
        snprintf(s_wifi_list_buffer, sizeof(s_wifi_list_buffer),
                 "<option>未发现可用网络</option>");
        return false;
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list)
    {
        ESP_LOGE(TAG, "内存分配失败");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    memset(s_wifi_list_buffer, 0, sizeof(s_wifi_list_buffer));
    char *ptr = s_wifi_list_buffer;
    int remaining = sizeof(s_wifi_list_buffer);

    for (int i = 0; i < ap_count; i++)
    {
        // 只显示非隐藏网络
        if (ap_list[i].ssid[0] != 0 && strlen((char *)ap_list[i].ssid) > 0)
        {
            int len = snprintf(ptr, remaining,
                               "<option value=\"%.32s\">%.32s (信号强度: %d)</option>",
                               ap_list[i].ssid, ap_list[i].ssid, ap_list[i].rssi);
            if (len > 0 && len < remaining)
            {
                ptr += len;
                remaining -= len;
            }
            else
            {
                break;
            }
        }
    }

    free(ap_list);
    ESP_LOGI(TAG, "扫描完成，发现 %d 个网络", ap_count);
    xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    return true;
}

// 尝试连接保存的WiFi
static bool wifi_try_connect_saved(void)
{
    nvs_handle_t nvs_handle;
    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};

    if (nvs_open("wifi_config", NVS_READONLY, &nvs_handle) != ESP_OK)
    {
        ESP_LOGI(TAG, "无法打开NVS存储");
        return false;
    }

    size_t len = sizeof(saved_ssid);
    esp_err_t ret = nvs_get_str(nvs_handle, "ssid", saved_ssid, &len);
    nvs_close(nvs_handle);

    if (ret != ESP_OK || strlen(saved_ssid) == 0)
    {
        ESP_LOGI(TAG, "未找到保存的WiFi配置");
        return false;
    }

    // 重新打开NVS获取密码
    nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
    len = sizeof(saved_pass);
    nvs_get_str(nvs_handle, "password", saved_pass, &len);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "找到保存的WiFi配置: %s (密码长度: %d)", saved_ssid, strlen(saved_pass));

    int retry_count = 0;

    while (retry_count < MAX_RECONNECTED_TIMES)
    {
        // 如果是重试，先断开之前的连接
        if (retry_count > 0)
        {
            ESP_LOGI(TAG, "第%d次重试连接...", retry_count + 1);
            esp_wifi_disconnect();
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }

        // 配置WiFi连接参数
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid) - 1);

        if (strlen(saved_pass) > 0)
        {
            strncpy((char *)wifi_config.sta.password, saved_pass, sizeof(wifi_config.sta.password) - 1);
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }
        else
        {
            wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }

        wifi_config.sta.scan_method = WIFI_FAST_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wifi_config.sta.threshold.rssi = -127;

        // 先检查WiFi状态
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA)
        {
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        }

        // 设置WiFi配置（如果有错误，继续尝试）
        esp_err_t config_err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (config_err != ESP_OK)
        {
            ESP_LOGW(TAG, "设置WiFi配置失败: %s (尝试继续)", esp_err_to_name(config_err));
            // 不直接返回，而是等待一下继续尝试
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            retry_count++;
            continue;
        }

        // 开始连接
        ESP_LOGI(TAG, "尝试连接到保存的WiFi: %s", saved_ssid);
        s_connect_attempted = true;
        esp_err_t connect_err = esp_wifi_connect();

        if (connect_err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi连接失败: %s", esp_err_to_name(connect_err));
            retry_count++;
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        // 等待连接结果（15秒超时）
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               15000 / portTICK_PERIOD_MS);

        if (bits & WIFI_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "成功连接到WiFi!");
            return true;
        }
        else if (bits & WIFI_FAIL_BIT)
        {
            ESP_LOGW(TAG, "连接失败");
        }
        else
        {
            ESP_LOGW(TAG, "连接超时");
        }

        retry_count++;

        if (retry_count < MAX_RECONNECTED_TIMES)
        {
            ESP_LOGI(TAG, "等待2秒后重试...");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }

    ESP_LOGW(TAG, "连接保存的WiFi失败(%d次重试)，将启动AP配网模式", MAX_RECONNECTED_TIMES);
    return false;
}

// Web服务器：根目录处理
static esp_err_t web_root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "访问Web配置页面");

    // 构建完整的HTML响应
    size_t total_len = strlen(ROOT_HTML_1) + strlen(s_wifi_list_buffer) + strlen(ROOT_HTML_2) + 1;
    char *html_response = malloc(total_len);
    if (!html_response)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    strcpy(html_response, ROOT_HTML_1);
    strcat(html_response, s_wifi_list_buffer);
    strcat(html_response, ROOT_HTML_2);

    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, html_response, strlen(html_response));

    free(html_response);
    return ret;
}

// Web服务器：配置处理
static esp_err_t web_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "处理WiFi配置请求");

    char ssid[33] = {0};
    char password[65] = {0};
    char *content = NULL;

    // 读取POST数据
    int total_len = req->content_len;
    ESP_LOGI(TAG, "数据总长度: %d", total_len);

    if (total_len <= 0 || total_len > 512) // 减小缓冲区大小
    {
        ESP_LOGE(TAG, "无效的数据长度: %d", total_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    // 分配内存
    content = malloc(total_len + 1);
    if (!content)
    {
        ESP_LOGE(TAG, "内存分配失败");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, content, total_len);
    if (ret <= 0)
    {
        free(content);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        ESP_LOGE(TAG, "接收数据失败: %d", ret);
        return ESP_FAIL;
    }

    content[ret] = '\0';
    ESP_LOGI(TAG, "接收到的原始数据: %s", content);

    // 简化的解析逻辑（只处理application/x-www-form-urlencoded）
    char *ptr = content;
    while (*ptr)
    {
        if (strncmp(ptr, "ssid=", 5) == 0)
        {
            ptr += 5;
            char *end = strchr(ptr, '&');
            if (end)
            {
                *end = 0;
                url_decode(ptr, ssid, sizeof(ssid));
                ptr = end + 1;
            }
            else
            {
                url_decode(ptr, ssid, sizeof(ssid));
                break;
            }
        }
        else if (strncmp(ptr, "password=", 9) == 0)
        {
            ptr += 9;
            char *end = strchr(ptr, '&');
            if (end)
            {
                *end = 0;
                url_decode(ptr, password, sizeof(password));
                ptr = end + 1;
            }
            else
            {
                url_decode(ptr, password, sizeof(password));
                break;
            }
        }
        else
        {
            ptr++;
        }
    }

    free(content);

    // 验证数据
    if (strlen(ssid) == 0)
    {
        ESP_LOGW(TAG, "SSID为空!");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID不能为空");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "配置信息 - SSID: %s, Password: %s",
             ssid, strlen(password) > 0 ? "***" : "空");

    // 保存到NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_set_str(nvs_handle, "ssid", ssid);
        nvs_set_str(nvs_handle, "password", password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "WiFi配置已保存到NVS");
    }
    else
    {
        ESP_LOGE(TAG, "NVS保存失败: %s", esp_err_to_name(err));
    }

    // 发送成功响应
    char response[512];
    snprintf(response, sizeof(response),
             "<html><head><meta charset='UTF-8'></head>"
             "<body style='text-align:center;padding:20px;'>"
             "<h3>配置完成</h3>"
             "<p>SSID: %s</p>"
             "<p>重启中...</p>"
             "<script>setTimeout(()=>location.href='/', 2000);</script>"
             "</body></html>",
             ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    // 延迟后重启
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "重启设备以应用新配置...");
    esp_restart();

    return ESP_OK;
}

/**
 * @brief 启动Web服务器
 *
 * 启动配置Web服务器用于WiFi配置，端口为WEB_PORT。
 * 如果服务器已启动，则直接返回。
 */
static void wifi_start_webserver(void)
{
    if (s_http_server != NULL)
    {
        ESP_LOGI(TAG, "Web服务器已启动");
        return;
    }

    ESP_LOGI(TAG, "启动Web服务器...");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_PORT;
    config.max_open_sockets = 3;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    // URI处理器
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_root_handler,
        .user_ctx = NULL};

    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = web_config_handler,
        .user_ctx = NULL};

    if (httpd_start(&s_http_server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(s_http_server, &root_uri);
        httpd_register_uri_handler(s_http_server, &config_uri);
        ESP_LOGI(TAG, "Web服务器启动在端口 %d", WEB_PORT);
        ESP_LOGI(TAG, "配置页面: http://192.168.4.1");
    }
    else
    {
        ESP_LOGE(TAG, "Web服务器启动失败");
    }
}

// 统一初始化函数
void wifi_init(void)
{
    if (s_wifi_initialized)
    {
        ESP_LOGI(TAG, "WiFi已经初始化");
        return;
    }

    ESP_LOGI(TAG, "初始化WiFi系统...");

    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS分区需要擦除...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 创建事件组
    s_wifi_event_group = xEventGroupCreate();

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建默认的STA和AP网络接口
    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // 设置AP IP地址
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);

    // WiFi初始化配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_AP_STAIPASSIGNED,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // 设置WiFi存储模式
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // 设置WiFi模式为STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 启动WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    // 设置WiFi电源模式
    wifi_ps_type_t ps_type = WIFI_PS_NONE; // 禁用省电模式，提高性能
    esp_wifi_set_ps(ps_type);

    // 设置TX功率
    esp_wifi_set_max_tx_power(78); // 19.5dBm

    // 获取当前配置
    wifi_config_t wifi_config;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);

    ESP_LOGI(TAG, "wifi初始化完毕");

    s_wifi_initialized = true;

    // 步骤1: 尝试连接保存的WiFi
    ESP_LOGI(TAG, "步骤1: 尝试连接保存的WiFi...");

    if (wifi_try_connect_saved())
    {
        ESP_LOGI(TAG, "WiFi连接成功！");
        return;
    }

    // 步骤2: 连接失败，启动AP配网模式
    ESP_LOGI(TAG, "步骤2: 启动AP配网模式...");

    // 启动AP模式
    wifi_start_ap();

    // 启动Web服务器
    wifi_start_webserver();

    ESP_LOGI(TAG, "AP配网模式已启动");
    ESP_LOGI(TAG, "请连接WiFi: %s", CONFIG_AP_SSID);
    ESP_LOGI(TAG, "然后访问: http://192.168.4.1");
}

// 断开WiFi连接
void wifi_disconnect(void)
{
    ESP_LOGI(TAG, "断开WiFi连接...");
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    if (s_wifi_event_group != NULL)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// 检查WiFi是否已连接
bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL)
    {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

// 获取WiFi状态信息
void wifi_get_status(wifi_status_t *status)
{
    if (status == NULL)
        return;

    memset(status, 0, sizeof(wifi_status_t));

    status->initialized = s_wifi_initialized;
    status->connected = wifi_is_connected();
    status->ap_mode_active = s_ap_mode_active;

    if (status->connected)
    {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            strncpy(status->ssid, (char *)ap_info.ssid, sizeof(status->ssid) - 1);
            status->rssi = ap_info.rssi;
        }

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif)
        {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            {
                status->ip.addr = ip_info.ip.addr;
                status->gw.addr = ip_info.gw.addr;
                status->netmask.addr = ip_info.netmask.addr;
            }
        }
    }
}

// 扫描WiFi网络
void wifi_scan(void)
{
    if (!s_wifi_initialized)
    {
        ESP_LOGW(TAG, "WiFi未初始化");
        return;
    }

    wifi_scan_and_update_list();
}

// 获取扫描结果
const char *wifi_get_scan_results(void)
{
    return s_wifi_list_buffer;
}

// 重新连接WiFi
void wifi_reconnect(void)
{
    if (!s_wifi_initialized)
    {
        wifi_init();
        return;
    }

    if (s_ap_mode_active)
    {
        wifi_stop_ap();
    }

    ESP_LOGI(TAG, "重新连接WiFi...");
    wifi_disconnect();
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // 尝试连接保存的WiFi
    if (wifi_try_connect_saved())
    {
        ESP_LOGI(TAG, "重新连接成功");
    }
    else
    {
        ESP_LOGW(TAG, "重新连接失败，启动AP模式");
        wifi_start_ap();
        wifi_start_webserver();
    }
}