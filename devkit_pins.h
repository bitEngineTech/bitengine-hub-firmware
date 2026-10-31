#pragma once

// =============================================================
// PCB GPIO Mapping — ESP32-C5-WROOM-1U ↔ EFR32MG24B020F1536IM40
// Source of truth: io_list_确认.xlsx + iot_hub_SCH(1).pdf (2026-06-30)
// (File name kept as devkit_pins.h for include compatibility; values
//  are the production PCB mapping per Q4=B decision.)
// =============================================================

// EZSP UART — to EFR32MG24 NCP (USART0)
// NOTE on direction: the schematic net names (USART_TX/RX) are from the
// MG24's perspective. From the C5's perspective the lines are swapped:
//   C5 TX (IO5) → MG24 PA06 (net "USART_RX")
//   C5 RX (IO4) ← MG24 PA05 (net "USART_TX")
#define EZSP_UART_TX_PIN    5    // C5 TX  → MG24 PA06
#define EZSP_UART_RX_PIN    4    // C5 RX  ← MG24 PA05
/* Flow control. The hard wiring is only IO2↔PA08 and IO3↔PA07 (470R series
   R33/R34, schematic sheet 5); which end calls its pin RTS is a firmware
   choice, because both chips route CTS/RTS through a pin matrix.
   We pick the direction that makes the schematic net names literally true —
   net "USART_CTS" really is the MG24's CTS, net "USART_RTS" really is its
   RTS — so anyone probing a net reads what the label claims:
       C5 RTS (IO2) ──R33── USART_CTS ── PA08  MG24 CTS in
       C5 CTS (IO3) ──R34── USART_RTS ── PA07  MG24 RTS out
   The MG24 side must agree; see mg24/README.md (both sides must be flashed as a
   pair; production images live in mg24/release/).
   ⚠️ These were 3/2 (the opposite) while flow control was disabled and the
   pins unused. Enabling CONFIG_BITENGINE_EZSP_HW_FLOWCTRL against an MG24
   still running no-flow-control firmware leaves our CTS floating and blocks
   TX forever — flash both sides together. */
#define EZSP_UART_RTS_PIN   2    // C5 RTS → MG24 PA08 (MG24's CTS)
#define EZSP_UART_CTS_PIN   3    // C5 CTS ← MG24 PA07 (MG24's RTS)
#define EZSP_UART_BAUD      460800
#define EZSP_UART_NUM       UART_NUM_1

// Button — multifunction button, active low, external 10K pull-up
#define BUTTON_PIN          26

// WS2812B LED — 4 LEDs daisy-chained (SYS / Zigbee / thread / net)
#define WS2812B_DATA_PIN    25   // RMT output, driven via MMBT3904
#define WS2812B_LED_COUNT   4
/* ⚠️ IO27 是 RS706 的手动复位输入 MR，不是 LED 控制脚。
   2026-08-09 对照原理图第 5 页确认：IO27 ──R125 49.9Ω── MR 网络
   （该网络还有 R57 10K 上拉到 VDD33、C75+C78 去抖电容、R55 4.7K 接 WDO）。
   拉低 MR 会让 RS706 输出 200ms 复位脉冲，经 R58 拖垮 C5 的 EN —— 整板重启。

   旧代码把它当作"以太网 LED 的 MOSFET 栅极"（该器件在 io_list_确认.xlsx 中
   根本不存在），并在夜间模式下持续拉低，等于把板子送进无限重启循环。
   ⚠️ 禁止把此脚配置为输出、更禁止驱动低电平。 */
#define RS706_MR_PIN        27

// DM9051 Ethernet PHY — SPI2 (shared with W25Q32)
#define ETH_SPI_HOST        SPI2_HOST
#define ETH_SPI_CLK_PIN     6    // FSPICLK  → DM9051 SPI_CK
#define ETH_SPI_MOSI_PIN    7    // FSPID    → DM9051 SPI_MOSI
#define ETH_SPI_MISO_PIN    9    // FSPIQ    ← DM9051 SPI_MISO
#define ETH_SPI_CS_PIN      10   // FSPICS0  → DM9051 SPI_CS
#define ETH_SPI_INT_PIN     28   // DM9051 PHY_INT, active low, external pull-up
#define ETH_SPI_RST_PIN     8    // DM9051 PHY_RESET
#define ETH_SPI_CLOCK_MHZ   20

// PoE detect — voltage divider R96 68K / R97 100K on 5V_POE rail
#define POE_DETECT_PIN      0    // high = PoE present

// Watchdog — RS706, WDI fed by 500ms toggle
#define WDT_WDI_PIN         1    // RS706 WDI input (was IO12 in old docs)

// MG24 reset / boot control (per io_list_确认.xlsx)
#define MG24_RESET_PIN      23   // MG24 RESETn (active low)
#define MG24_NBOOT_PIN      24   // MG24 nBOOT — low = enter bootloader
