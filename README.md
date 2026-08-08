# ESP32 NFC 卡复制器

ESP32 + RC522(MFRC522)+ DST-013 1.3" OLED(SH1106)+ 2 按键,读/写/克隆 MIFARE Classic Mini/1K/4K。

## 功能
- **READ**:两级密钥字典 + KEY A/B + 密钥收割(key 链补轮),读整卡含密钥
- **WRITE Gen2 / Gen1a**:Gen1a 后门整卡克隆,尾块回填真密钥 → 克隆卡密钥 = 源卡
- **SAVE / LOAD / DELETE**:动态 NVS 存储(卡数动态)+ PackBits 压缩(UID/密钥不压)
- **PC BRIDGE**:USB 串口读卡器,配电脑端脚本(读 UID / dump / 写回)
- 开机 RC522 自检(读版本寄存器,便于排查接线)

## 接线图

```
                      ┌───────────────────────┐
     RC522 (SPI,3V3)  │       ESP32 (XX5R69)   │   DST-013 OLED (I2C)
   ┌──────────┐       │                        │       ┌──────────┐
   │ SDA/SS ──┼───────┤ GPIO5           GPIO21 ├───────┼─ SDA     │
   │ SCK    ──┼───────┤ GPIO18          GPIO22 ├───────┼─ SCL     │
   │ MOSI   ──┼───────┤ GPIO23            3V3  ├───┬───┼─ VCC     │
   │ MISO   ──┼───────┤ GPIO19            GND  ├─┬─┴───┼─ GND     │
   │ RST    ──┼───────┤ GPIO27                 │ │     └──────────┘
   │ 3.3V   ──┼───┐   │                        │ │
   │ GND    ──┼─┐ │   │ GPIO32 ─── [BTN1] ──── GND (切换菜单)
   └──────────┘ │ └───┤ 3V3     GPIO33 ─ [BTN2] ─ GND (确认/执行)
                └─────┤ GND                    │
                      └───────────────────────┘
  ⚠ RC522 与 OLED 的 VCC 都接 3V3, 绝不接 5V(5V 会把 I²C/SPI 灌进 3.3V GPIO)
  ⚠ 最易错: MISO↔MOSI 接反 → 开机自检显示 ver:0x00。MISO 必须到 GPIO19
```

## 接线

### RC522 (SPI, 3.3V!! 千万别接 5V)
| RC522 | ESP32   |
|-------|---------|
| SDA/SS| GPIO5   |
| SCK   | GPIO18  |
| MOSI  | GPIO23  |
| MISO  | GPIO19  |
| RST   | GPIO27  |
| VCC   | 3V3     |
| GND   | GND     |

### DST-013 OLED (I2C)
| OLED | ESP32 |
|------|-------|
| VCC  | 3V3   |
| GND  | GND   |
| SCL  | GPIO22|
| SDA  | GPIO21|

### 按键(另一脚接 GND,内部上拉)
| 按键 | ESP32  | 作用          |
|------|--------|---------------|
| BTN1 | GPIO32 | 切换菜单       |
| BTN2 | GPIO33 | 确认 / 执行    |

## 软件

### PlatformIO(推荐)
工程已配好 `platformio.ini`(board = `esp32dev`,依赖锁定 MFRC522 + U8g2):
```
cd ~/esp32-nfc-copier
pio run              # 编译
pio run -t upload    # 烧录(自动找 /dev/ttyUSB0)
pio device monitor   # 串口监视 115200
```
源码在 `src/esp32-nfc-copier.ino`。实测占用:Flash 23.7%、RAM 7.4%。

### Arduino IDE(备选)
装 ESP32 板支持 + 库管理器装 `MFRC522`(miguelbalboa)、`U8g2`(oliver),
把 `src/esp32-nfc-copier.ino` 打开烧录即可。
> 注意:MFRC522 ≥1.4.12 的 `PICC_GetTypeName()` 返回 `__FlashStringHelper*`,
> 代码里已用 `String(...)` 兼容,不用改。

> 屏幕若显示花屏/全黑:多数 1.3" OLED 是 **SH1106**;若是 SSD1306,把 .ino 里
> `U8G2_SH1106_...` 那行换成注释里的 `U8G2_SSD1306_...`。
> I2C 地址一般 0x3C,U8g2 会自动处理。

## 菜单(BTN1 切换,BTN2 执行)
| 项 | 作用 |
|----|------|
| `INFO`        | 贴卡看 UID / 类型,先确认是不是 MIFARE 1K |
| `READ`        | 贴**源卡**,读全部 16 扇区进内存;逐块记录「是否读成功」 |
| `WRITE Gen2`  | 标准写。跳过密钥块;UID(块0)只有 Gen2 magic 卡能写 |
| `WRITE Gen1a` | **后门写**。`0x40/0x43` 解锁后整卡克隆,含块0(UID)与密钥块 |
| `SAVE`        | 把内存缓冲存到 flash 槽位(0–3) |
| `LOAD`        | 从 flash 槽位读回内存 |
| `PC BRIDGE`   | USB 串口桥接:电脑跑脚本即可读卡/dump/读写块。BTN1 退出 |

典型流程:`READ` 源卡 → (可选 `SAVE`) → 换目标卡 → `WRITE Gen1a`(magic 卡)或 `WRITE Gen2`。

### SAVE / LOAD 槽位选择
进入后 **B1** 切换槽位(会显示 `USED/empty` 和该槽 UID,末尾一项是 `<< BACK` 取消),**B2** 确认。
数据存在 ESP32 内部 NVS(Preferences),掉电不丢;每槽含 1024B 全卡数据 + 有效位 + UID。

## Gen1a vs Gen2 magic 卡
- **Gen2(CUID/UID 可写)**:表现得像普通卡,直接 `MIFARE_Write` 块0 即可 → 用 `WRITE Gen2`。
- **Gen1a(后门卡)**:块0 写不进去,需要先发 `0x40`(7-bit)+`0x43` 解锁后门,再无认证写任意块 → 用 `WRITE Gen1a`。
- 分不清就先试 `WRITE Gen1a`,解锁失败会提示「not a Gen1a card」,再改用 `WRITE Gen2`。

## PC BRIDGE(让电脑用它读卡)
经典 ESP32(XX5R69 = ESP-WROOM-32)**没有原生 USB**,电脑只能把它认成串口,
**做不了系统原生识别的 PC/SC CCID 读卡器**。桥接模式是这块板上唯一可行的方案:

1. OLED 菜单选 `PC BRIDGE (USB)`,屏幕显示 "listening..."
2. 电脑端(装 `pip install pyserial`):
   ```
   python3 host/nfc_host.py            # 自动找串口, 交互模式
   python3 host/nfc_host.py uid        # 读一次 UID
   python3 host/nfc_host.py dump       # dump 整卡
   python3 host/nfc_host.py -p COM5    # Windows 指定串口
   ```
3. 交互命令:`uid` / `info` / `dump` / `rblk <n>` / `wblk <n> <32hex>` / `ping` / `quit`
4. 设备端按 **BTN1** 退出桥接,回到菜单。

串口协议(115200,每行一条命令,自己写程序也能对接):
`PING→PONG` · `UID` · `INFO` · `RBLK <n>` · `WBLK <n> <32hex>` · `DUMP`(以 `DONE` 结束)。

### 电脑端脚本 / 桌面图标
| 脚本 | 桌面图标 | 作用 |
|------|----------|------|
| `host/nfc_host.py` | ESP32 NFC 桥接器 | 交互:`uid/info/dump/rblk/wblk` |
| `host/dump-to-file.sh` → `write_from...`| ESP32 NFC 一键 dump | 整卡存 `dumps/card-<时间戳>.dump` |
| `host/write_from_dump.py` | ESP32 NFC 写回卡 | 选 `.dump` 写回卡(默认跳过 UID/密钥块) |

- 桌面图标位于 `~/桌面/`,应用菜单搜 "NFC" 也能找到。
- **写回限制**:串口 `WBLK` 用默认密钥认证写入,**不含 Gen1a 后门**。块0(UID)只有 Gen2 magic 卡能写(加 `--block0`);要连 UID 完整克隆 Gen1a 卡,用**设备端菜单 READ + WRITE Gen1a**。
- 串口打开已做**防复位**处理(DTR/RTS 置 False),否则经典 ESP32 一连就会重启退出 bridge 模式。

### 想要「真·即插即用 USB 读卡器」?必须换板子
这需要**原生 USB**,只有 **ESP32-S2 / S3** 能做,两条路:
- **HID 键盘 wedge(简单,推荐)**:S2/S3 伪装 USB 键盘,刷卡把 UID「打字」进任意输入框,零驱动。很多廉价 USB RFID 读卡器就是这么干的。
- **PC/SC CCID 读卡器(硬核,实验性)**:S2/S3 自研 CCID 类 + 把 PC-SC APDU 映射到 RC522,系统才会认成读卡器。TinyUSB 无现成 CCID 类,工程量大,且只能读 MIFARE。

不管哪块板:**刷卡支付、AirDrop、华为/小米共享都做不了**——支付要安全芯片,共享是 Wi-Fi Direct 私有协议,RC522 连 NFC P2P 都不支持。

## dump 保存位置
`ESP32 NFC 一键 dump` 桌面图标会弹 kdialog 让你**自选保存位置/文件名**(默认 `dumps/card-<时间戳>.dump`)。
命令行也可指定:`python3 host/nfc_host.py -o /任意/路径.dump dump`。写回用 `write_from_dump.py` 或桌面图标可打开任意 `.dump`。

## 重要限制
- 用**两级密钥字典 + KEY A/B + 密钥收割**尝试认证;字典外的真·随机密钥仍读不出(显示 `key unknown`),需 Proxmark 的 nested/darkside,RC522 做不到。
- `WRITE Gen1a` 做**完整克隆**并回填真密钥(克隆卡密钥=源卡);源卡没读到的扇区会跳过而非写零。
- 加密门禁卡 / 滚动码 / DESFire / CPU 卡 / 非 Type-A(Type-B / 15693 / FeliCa)/ 125kHz「ID 卡」**无法复制**——RC522 硬件与协议限制,非代码问题。
- flash 里的卡用菜单 `DELETE saved` 删除(动态计数);要全清可在 setup 里调用 `prefs.begin("nfc",false); prefs.clear(); prefs.end();` 烧一次再删掉。

## 法律 / 道德
仅用于复制你**本人拥有或已获授权**的卡(自家门禁、测试卡等)。未经授权复制他人门禁/支付卡是违法的。

## License
[MIT](LICENSE) © 2026 cat-huahua
