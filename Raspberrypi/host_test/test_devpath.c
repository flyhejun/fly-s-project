/**
  * @file    test_devpath.c
  * @brief   host 测试：验证 ble_device_path 把 conf MAC 转成 BlueZ 设备路径
  *
  * 本机 gcc 编译运行，零依赖（不依赖 glib）：
  *   gcc -Wall -Wextra -o test_devpath test_devpath.c && ./test_devpath
  *
  * 注意：本文件内联了 src/ble_write.c 的 ble_device_path 实现副本，
  *       修改该函数后需同步本文件（两端行为必须一致）。
  */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- 以下为 src/ble_write.c 中 ble_device_path 的逐字副本 ---- */
static void ble_device_path(char *mac, size_t mac_len, char *out)
{
    uint8_t     i;
    char        path[64] = "/org/bluez/hci0/dev_";
    uint8_t     path_len = strlen(path);

    for(i = 0; i < mac_len; i++)
    {
        if(mac[i] == ':')
        {
            path[path_len] = '_';
        }
        else if(mac[i] >= 'a' && mac[i] <= 'f')
        {
            path[path_len] = mac[i] - 'a' + 'A';
        }
        else
        {
            path[path_len] = mac[i];
        }

        path_len += 1;
    }

    strncpy(out, path, 63);
    out[63] =  '\0';

    return ;
}
/* ---- 副本结束 ---- */

static int check(const char *input, const char *expect)
{
    char out[64];

    ble_device_path((char *)input, strlen(input), out);

    if (strcmp(out, expect) != 0)
    {
        printf("[FAIL] %-20s -> \"%s\" (期望 \"%s\")\n", input, out, expect);
        return 1;
    }

    printf("[PASS] %-20s -> \"%s\"\n", input, out);
    return 0;
}

int main(void)
{
    int fail = 0;

    /* 默认 conf MAC（真实场景） */
    fail += check("58:8c:81:0e:4e:16", "/org/bluez/hci0/dev_58_8C_81_0E_4E_16");
    /* 全小写字母 */
    fail += check("aa:bb:cc:dd:ee:ff", "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF");
    /* 全零 MAC */
    fail += check("00:00:00:00:00:00", "/org/bluez/hci0/dev_00_00_00_00_00_00");
    /* 全大写输入（不应被误转，原样保留） */
    fail += check("AA:BB:CC:DD:EE:FF", "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF");

    if (fail)
        printf("\n%d 个用例失败\n", fail);
    else
        printf("\n全部 4 个用例通过\n");

    return fail ? 1 : 0;
}