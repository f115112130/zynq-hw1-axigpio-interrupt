/*
 * HW1：AXI GPIO 按鈕中斷 + AXI Timer 中斷（bare-metal，未包裝自製 IP）
 *
 * 硬體（Vivado Block Design）
 *   axi_gpio_0 : LED x8 輸出        0x41200000
 *   axi_gpio_1 : 按鈕 x5 輸入，開中斷 0x41210000  → IRQ_F2P[0] = GIC 61
 *   axi_timer_0: 50MHz             0x42800000  → IRQ_F2P[1] = GIC 62
 *
 * 功能
 *   - 每按一次按鈕觸發中斷，xil_printf 印出次數，LED 計數歸零
 *   - Timer 每 0.2 秒中斷一次讓 LED 計數 +1，累積 25 次（5 秒）印一次提醒
 */
#include <stdio.h>
#include "platform.h"
#include "xparameters.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xtmrctr.h"
#include "xil_exception.h"
#include "xil_printf.h"

#define BTN_ID            XPAR_AXI_GPIO_1_DEVICE_ID
#define LED_ID            XPAR_AXI_GPIO_0_DEVICE_ID
#define TIMER_ID          XPAR_AXI_TIMER_0_DEVICE_ID

#define INTC_GPIO_ID      XPAR_FABRIC_AXI_GPIO_1_IP2INTC_IRPT_INTR   /* 61 */
#define INTC_TIMER_ID     XPAR_FABRIC_AXI_TIMER_0_INTERRUPT_INTR     /* 62 */

#define BTN_INT           XGPIO_IR_CH1_MASK
#define GIC_ID            XPAR_SCUGIC_SINGLE_DEVICE_ID

/* Timer 設定：50MHz 下 10,000,000 counts = 0.2 秒 */
#define TIMER_RESET_VAL   0xFF676980u
#define TICKS_PER_5SEC    25          /* 0.2s * 25 = 5s */

XGpio LED, BTN;
XScuGic INTCInst;
XTmrCtr TimerInst;

volatile int LED_num   = 0;
volatile int Intr_CTN  = 0;   /* 按鈕中斷次數 */
volatile int Timer_CTN = 0;   /* Timer tick 計數（0.2s 一次） */
volatile int Sec5_CTN  = 0;   /* 5 秒提醒次數 */

/* ================================
 * 按鈕中斷處理函式：每次中斷都印 */
void Intr_Handler(void *CallbackRef) {
    XGpio *GpioPtr = (XGpio *)CallbackRef;

    /* 先關閉中斷，避免處理中重入 */
    XGpio_InterruptDisable(GpioPtr, BTN_INT);

    u32 btn_val = XGpio_DiscreteRead(GpioPtr, 1);

    /* AXI GPIO 在按下與放開都會觸發，只在「按下」時計數 */
    if (btn_val != 0) {
        Intr_CTN++;
        LED_num = 0;
        xil_printf("[BTN] Interrupt : %d  (btn = 0x%x)\r\n", Intr_CTN, btn_val);
    }

    /* 清除中斷狀態並重新開啟 */
    XGpio_InterruptClear(GpioPtr, BTN_INT);
    XGpio_InterruptEnable(GpioPtr, BTN_INT);
}

/* Timer 中斷處理函式：每 0.2 秒進來一次，累積 25 次 = 5 秒才印 */
void Timer_Intr_Handler(void *CallBackRef, u8 TmrCtrNumber) {
    LED_num++;
    Timer_CTN++;

    if (Timer_CTN >= TICKS_PER_5SEC) {
        Timer_CTN = 0;
        Sec5_CTN++;
        xil_printf("[TIMER] 5 sec reminder #%d  (total %d sec)\r\n",
                   Sec5_CTN, Sec5_CTN * 5);
    }
}

/* ================================
 * 中斷系統初始化 */
int Intr_Setup(XScuGic *GicInstancePtr, XGpio *GpioInstancePtr, XTmrCtr *TmrInstancePtr) {
    int Status;
    XScuGic_Config *IntcConfig;

    IntcConfig = XScuGic_LookupConfig(GIC_ID);
    if (NULL == IntcConfig) {
        return XST_FAILURE;
    }

    Status = XScuGic_CfgInitialize(GicInstancePtr, IntcConfig, IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
        (Xil_ExceptionHandler)XScuGic_InterruptHandler, GicInstancePtr);

    /* 1. GPIO 中斷 */
    Status = XScuGic_Connect(GicInstancePtr, INTC_GPIO_ID,
        (Xil_ExceptionHandler)Intr_Handler, (void *)GpioInstancePtr);
    if (Status != XST_SUCCESS) return XST_FAILURE;
    XGpio_InterruptEnable(GpioInstancePtr, BTN_INT);
    XGpio_InterruptGlobalEnable(GpioInstancePtr);
    XScuGic_Enable(GicInstancePtr, INTC_GPIO_ID);

    /* 2. Timer 中斷 */
    Status = XScuGic_Connect(GicInstancePtr, INTC_TIMER_ID,
        (Xil_ExceptionHandler)XTmrCtr_InterruptHandler, (void *)TmrInstancePtr);
    if (Status != XST_SUCCESS) return XST_FAILURE;
    XTmrCtr_SetHandler(TmrInstancePtr, (XTmrCtr_Handler)Timer_Intr_Handler, TmrInstancePtr);
    XScuGic_Enable(GicInstancePtr, INTC_TIMER_ID);

    /* 全部設定完才開 CPU 中斷 */
    Xil_ExceptionEnable();

    return XST_SUCCESS;
}

/* Timer 初始化（只設定，不啟動） */
int Timer_Setup(void) {
    int Status = XTmrCtr_Initialize(&TimerInst, TIMER_ID);
    if (Status != XST_SUCCESS) return XST_FAILURE;

    XTmrCtr_SetOptions(&TimerInst, 0, XTC_INT_MODE_OPTION | XTC_AUTO_RELOAD_OPTION);
    XTmrCtr_SetResetValue(&TimerInst, 0, TIMER_RESET_VAL);
    return XST_SUCCESS;
}

/* ================================ */
int main() {
    init_platform();

    XGpio_Initialize(&LED, LED_ID);
    XGpio_SetDataDirection(&LED, 1, 0x0);          /* CH1 輸出 */

    XGpio_Initialize(&BTN, BTN_ID);
    XGpio_SetDataDirection(&BTN, 1, 0xFFFFFFFF);   /* CH1 輸入 */

    if (Timer_Setup() != XST_SUCCESS) {
        xil_printf("Timer setup failed\r\n");
        return XST_FAILURE;
    }

    if (Intr_Setup(&INTCInst, &BTN, &TimerInst) != XST_SUCCESS) {
        xil_printf("Interrupt setup failed\r\n");
        return XST_FAILURE;
    }

    XTmrCtr_Start(&TimerInst, 0);   /* 中斷接好後才啟動 Timer */

    xil_printf("Init successful\r\n");

    while (1) {
        XGpio_DiscreteWrite(&LED, 1, LED_num);
    }

    cleanup_platform();
    return 0;
}
