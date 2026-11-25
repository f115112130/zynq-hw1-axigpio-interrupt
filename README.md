# HW1：Zynq-7000 按鈕中斷與 Timer 中斷（AXI GPIO，未包裝 IP）

> 學號：f115112130　姓名：蔡孟廷

開發板：ego-xz7（Zynq-7000 xc7z020clg484-1）
工具：Vivado 2018.3、Xilinx SDK 2018.3

## 作業目標
1. 找 Zynq-7000 的使用手冊（UG585 Technical Reference Manual）與程式設計指南（UG821），了解中斷相關暫存器與 BSP。
2. 用 C（SDK）控制 FPGA 上的 LED。
3. 按下按鈕後 LED 有變化，並寫中斷服務函式（ISR）處理按鈕中斷。

本版本使用 Xilinx 內建的 **AXI GPIO** 與 **AXI Timer** IP，不自己寫 RTL。

## 硬體架構（Block Design）

```
               ┌──────────────── PL ────────────────┐
 BTN x5 ─────► │ axi_gpio_1 (0x41210000) ─ ip2intc ─┼──┐
 LED x8 ◄───── │ axi_gpio_0 (0x41200000)            │  │  xlconcat_0
               │ axi_timer_0 (0x42800000) ─ intr ───┼──┤  In0 ← GPIO
               └────────────────┬───────────────────┘  │  In1 ← Timer
                                │ AXI (M_AXI_GP0)       ▼
               ┌──────────────── PS ────────────────┐ IRQ_F2P[1:0]
               │ Cortex-A9 ◄── GIC ◄────────────────┼───────┘
               │ UART1 (115200) → 終端機             │
               └────────────────────────────────────┘
```

| 元件 | 位址 | 中斷 |
|---|---|---|
| axi_gpio_0（LED 8 bits，輸出） | 0x41200000 | — |
| axi_gpio_1（按鈕 5 bits，輸入，開中斷） | 0x41210000 | IRQ_F2P[0] → GIC **61** |
| axi_timer_0（50 MHz） | 0x42800000 | IRQ_F2P[1] → GIC **62** |
| PS UART1 | 0xE0001000 | — |

腳位見 [`constraints/pin.xdc`](constraints/pin.xdc)。

## 軟體功能（[`src/main.c`](src/main.c)）
- **按鈕中斷**：每按一次，ISR 用 `xil_printf` 印出中斷次數，並把 LED 計數歸零。
- **Timer 中斷**：每 0.2 秒中斷一次讓 LED 計數 +1；累積 25 次（5 秒）才印一次提醒，避免洗版。
- 主迴圈只負責把計數值寫到 LED。

Timer 計算：`0xFFFFFFFF - 0xFF676980 + 1 = 10,000,000` counts，50 MHz 下 = 0.2 秒。

### 中斷初始化順序
1. `XScuGic_LookupConfig` / `XScuGic_CfgInitialize`：初始化 GIC
2. `Xil_ExceptionRegisterHandler`：把 CPU 的 IRQ 例外接到 `XScuGic_InterruptHandler`
3. `XScuGic_Connect(61, Intr_Handler)`、`XScuGic_Connect(62, XTmrCtr_InterruptHandler)`：在中斷向量表註冊 ISR
4. `XScuGic_Enable`：在 GIC 開啟這兩個中斷
5. `Xil_ExceptionEnable`：最後才讓 CPU 接受中斷
6. `XTmrCtr_Start`：中斷都接好後才啟動 Timer

## 執行方式
1. Vivado：Generate Bitstream → File → Export → Export Hardware（勾 Include bitstream）
2. SDK：建立 Application Project（Empty / Hello World），把 `src/main.c` 放進 `src/`
3. Run Configurations → Target Setup：選正確的 hw_platform，勾 Reset entire system、Program FPGA、Run ps7_init、Run ps7_post_config
4. 終端機：COM port、**115200**、8N1

## 執行結果
SDK Terminal（COM6, 115200）實際輸出：
```
[TIMER] 5 sec reminder #998  (total 4990 sec)
[TIMER] 5 sec reminder #999  (total 4995 sec)
[TIMER] 5 sec reminder #1000  (total 5000 sec)
...
[TIMER] 5 sec reminder #1003  (total 5015 sec)
```
Timer 中斷每 5 秒提醒一次，連續執行超過 80 分鐘穩定。

按鈕中斷預期輸出（每按一次一行，LED 歸零）：
```
[BTN] Interrupt : 1  (btn = 0x1)
```
<!-- TODO：補上按鈕中斷的終端機截圖與 LED 照片 -->

建議在 `docs/` 放：Block Design 截圖、Address Editor 截圖、終端機輸出截圖、板子 LED 照片。

## 遇到的問題與解決方法

| # | 問題 | 原因 | 解決方法 |
|---|---|---|---|
| 1 | 不知道要不要設定鮑率 | — | Zynq PS UART 的鮑率由 FSBL / `ps7_init` 設好（115200），程式不用設定，只要終端機設成 115200 8N1 |
| 2 | Hello World 能跑，這支程式「沒反應」 | Run Configuration 指向不存在的 `design_1_wrapper_hw_platform_0`，跳出 `Invalid hardware project`，程式根本沒下載 | Run Configurations → Target Setup 改選目前存在的 hw_platform；或刪掉舊設定重建 |
| 3 | SDK 跳出 `Plug-in org.eclipse.cdt.ui was unable to load class ... CEditor` | Eclipse workspace 設定檔損壞 / 路徑問題 | 用 `-clean` 啟動 SDK；刪除 `.metadata/.plugins/org.eclipse.e4.workbench/workbench.xmi`；workspace 路徑不要有中文或空格 |
| 4 | 按一次按鈕印兩次 | AXI GPIO 的中斷在**按下與放開**都會觸發 | ISR 裡讀按鈕值，只有 `btn_val != 0`（按下）才計數 |
| 5 | Timer 每 0.2 秒印一次，訊息洗版 | Timer 週期 0.2 秒 | 用計數器累積 25 次（5 秒）才印一次 |
| 6 | 原本用 `printf`，也想改成老師要求的 `xil_printf` | `printf` 較肥大，且原程式把列印放在主迴圈 | 全部改用 `xil_printf`，換行用 `\r\n`；列印集中在 ISR，避免主迴圈與 ISR 同時列印造成輸出交錯 |
| 7 | 中斷在 handler 註冊前就進來 | 原程式先啟動 Timer、才設定中斷 | 改成先設好 GIC 和所有 handler，最後 `Xil_ExceptionEnable`，再 `XTmrCtr_Start` |

## 參考資料
- UG585 Zynq-7000 SoC Technical Reference Manual（第 7 章 Interrupts）
- UG821 Zynq-7000 SoC Software Developers Guide
- PG144 AXI GPIO、PG079 AXI Timer
