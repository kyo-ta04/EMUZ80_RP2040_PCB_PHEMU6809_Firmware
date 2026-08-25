/*
 * EMUZ80_RP2040_PCB_PHEMU6809_Firmware - NitrOS-9 + BASIC09
 *
 * @tendai22plus さんの EMUZ80_RP2040_PCB と
 * @comoneko さん作の PHEMU6809 を使って、
 * Z80 DIP40 に MC6809P のピンを組み替えて動作させるための
 * RP2040用 ファームウェアです。 
 * 
 *
 * 秋月電子通商 AE-RP2040 ボード上で動作。
 * RP2040 が 64KB RAM + MC6850 互換 ACIA ($FFD0-D1) をエミュレート。
 * TIMER は my6809/multicomp09 互換の $FFDD。
 *
 * Author:  @DragonBallEZ
 * License: MIT
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/structs/sio.h"
#include "hardware/clocks.h"
#include "hardware/structs/iobank0.h"
#include "hardware/vreg.h"
#include "tusb.h" // TinyUSB

// #include "basic9.h"
#include "track34.h"
#include "sd_emu.h"
#include "sd_img.h"

// === Easy Overclock Configuration ===
// Change this one value to switch clock speed (under 300MHz recommended)
// Common good values: 200000, 250000, 266667, 280000
// #define TARGET_SYS_CLK_KHZ 288000
#define TARGET_SYS_CLK_KHZ 125000

#define A0_15_BASE  0
#define A0_15_MASK  (0xFFFFu << A0_15_BASE)   // GPIO0-15
#define D0_BASE     16
#define D0_MASK     (0xFFu << D0_BASE)        // GPIO16-23
#define E_PIN       24  // GP24: MC6809 E in
#define RW_PIN      25  // GP25: MC6809 R/W# in H=Read/L=Write
// #define Q_PIN       26  // GP26: MC6809 Q in
#define IRQ_PIN     26  // GP26: MC6809 IRQ# out H=OFF/L=ON
#define MRDY_PIN    27  // GP27: MC6809 MRDY in L=WAIT
#define RESET_PIN   28  // GP28: RESET out L=Reset
#define CLK_PIN     29  // GP29: CLK (Clock) out PWM

#define E_MASK      (1u << E_PIN)
// #define Q_MASK      (1u << Q_PIN)
#define IRQ_MASK    (1u << IRQ_PIN)
#define RW_MASK     (1u << RW_PIN)
#define MRDY_MASK   (1u << MRDY_PIN)
#define RESET_MASK  (1u << RESET_PIN)
#define CLK_MASK    (1u << CLK_PIN)

// Memory MAP (my6809 / multicomp09-compatible I/O)
// $0000-$FFFF    flat 64K RAM (+ track34 @ $2600, vectors @ $FFF2)
// $FFD0-$FFD1  ACIA 6850 (status/data)
// $FFD8-$FFDC  virtual SD (256B blocks, sd_img[] in FLASH)
// $FFDD        TIMER (bit1 enable, bit7 IRQ clear/pending)
// MMIO hole is $FFD0-$FFDF only; vectors at $FFF0-$FFFF stay RAM/ROM.

#define ACIA_STAT   0xFFD0
#define ACIA_DATA   0xFFD1
#define TIMER_REG   0xFFDD
#define MMIO_BASE   0xFFD0
#define MMIO_MASK   0xFFF0   /* $FFD0-$FFDF */

#define MEMORY_SIZE 65536


// メモリ
static uint8_t __attribute__((aligned(65536))) __not_in_flash("bus_memory") memory[MEMORY_SIZE];

/* 
 * MC6809 TEST Program Code
 * Start Address: 0x0100
 * Description: Load from $0080, increment, store back, and loop.
 */

// テスト用プログラム
static uint8_t mc6809_test1[] = {
    0x96, 0x80,  // 0100: LDA $80 (Direct page access [1-4])
    0x4C,        // 0102: INCA     (Increment Accumulator A [4, 5])
    0x97, 0x80,  // 0103: STA $80 (Direct page access [4, 6])
    0x20, 0xF9   // 0105: BRA $0100 (Relative branch: $0107 - 7 bytes = $0100 [7-9])
};

// $100 - ACIA polled TX of 'A' via $FFD0/$FFD1
static uint8_t mc6809_test3[] = {
  0xb6,0xff,0xd0,0x85,0x02,0x27,0xf9,0x86,0x41,0xb7,0xff,0xd1,0x20,0xfe
};

// Hello World $0100- via ACIA $FFD0/$FFD1
static uint8_t mc6809_test3_1[] = {
  0x8e,0x01,0x15,0xa6,0x80,0x27,0x0c,0xf6,0xff,0xd0,0xc5,0x02,0x27,0xf9,0xb7,0xff,
  0xd1,0x20,0xf0,0x20,0xfe,0x0d,0x0a,0x48,0x65,0x6c,0x6c,0x6f,0x20,0x4d,0x43,0x36,
  0x38,0x30,0x39,0x20,0x57,0x6f,0x72,0x6c,0x64,0x21,0x0d,0x0a,0x00
};

// ベクタ
static uint8_t bin_fffe[] = {
    0x01, 0x00  // 0100 START
};


// reset_release_callback()
static int64_t reset_release_callback(alarm_id_t id, void *user_data) {
    gpio_set_dir(RESET_PIN, GPIO_IN);
    printf("RESET-OFF\n\n\n");
    return 0;
}


// MC6809 に供給するクロック周波数（Hz）
// #define MC6809_CLK_HZ 12000000       // 12MHzHz
// #define MC6809_CLK_HZ 11000000       // 11MHzHz HD63C09P - 3MHz
// #define MC6809_CLK_HZ 10000000       // 10MHzHz  -- MC68B09P - 2.5MHz
// #define MC6809_CLK_HZ 8000000       // 8MHzHz MC68B09P - 2MHz
// #define MC6809_CLK_HZ 6000000       // 6MHzHz MC68A09 - 1.5MHz
#define MC6809_CLK_HZ 4000000       // 4MHzHz  MC6809P - 1MHz
// #define MC6809_CLK_HZ 2000000       // 2MHzHz  MC6809P - 0.5MHz
// #define MC6809_CLK_HZ 400000     // 400,000Hz
// #define MC6809_CLK_HZ 100000     // 100,000Hz
    

uint clk_slice_num;     // PWM slice number

//
// init_clk()
//
static void init_clk(void) {
    gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);
    clk_slice_num = pwm_gpio_to_slice_num(CLK_PIN);
    uint chan = pwm_gpio_to_channel(CLK_PIN);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t target_freq = MC6809_CLK_HZ;

    if (target_freq == 0) target_freq = 1;

    // システムクロック125MHzデフォルトで 8Hz〜20MHz以上（最大62.5MHz程度）を出せるよう計算
    // 実際の周波数 = sys_clk / ((wrap + 1) * divider)
    uint32_t period = (sys_clk + (target_freq / 2)) / target_freq;

    uint16_t wrap;
    float divider;

    if (period > 65536) {
        // 低周波数側：wrap最大 + dividerで調整（最低約7.5Hz程度まで）
        divider = (float)period / 65536.0f;
        wrap = 65535;
    } else {
        // 高周波数側：divider=1.0 + wrapを小さくして高周波数対応
        divider = 1.0f;
        wrap = (uint16_t)(period - 1);
    }

    // divider範囲: 1.0 ～ 255.9375 (RP2040 PWMの最大)
    if (divider < 1.0f) divider = 1.0f;
    if (divider > 255.9375f) divider = 255.9375f;

    // 50% dutyを維持するため、最小period=2 (wrap=1) に制限（最大約62.5MHz）
    if (wrap < 1) {
        wrap = 1;
    }

    pwm_set_clkdiv(clk_slice_num, divider);
    pwm_set_wrap(clk_slice_num, wrap);
    pwm_set_chan_level(clk_slice_num, chan, (wrap + 1) / 2);  // 50% duty
    pwm_set_enabled(clk_slice_num, true);

    // デバッグ用：設定後の実際の周波数を概算表示
    float achieved_hz = (float)sys_clk / ((wrap + 1) * divider);
    printf("PWM: slice=%u chan=%u wrap=%u div=%.4f target=%luHz achieved≈%.1fHz\n",
           clk_slice_num, chan, wrap, (double)divider, (unsigned long)target_freq, (double)achieved_hz);
}


// Fast direct SIO register access (bypassing SDK for speed)
// Always inline for maximum speed in hot path
static inline __attribute__((always_inline)) void drive_databus(uint8_t data) {
    uint32_t bits = (uint32_t)data << D0_BASE;
    sio_hw->gpio_out = (sio_hw->gpio_out & ~D0_MASK) | bits;
    sio_hw->gpio_oe_set = D0_MASK;
}

static inline __attribute__((always_inline)) void release_databus(void) {
    sio_hw->gpio_oe_clr = D0_MASK;  // Release data bus (set to input)
}

static inline __attribute__((always_inline)) uint32_t read_gpio_all(void) {
    return sio_hw->gpio_in;
}

// MRDY direct SIO control (for speed in busemu)
static inline void mrdy_low(void)  { sio_hw->gpio_out = sio_hw->gpio_out & ~MRDY_MASK; }  // MRDY Low(WAIT)
static inline void mrdy_high(void) { sio_hw->gpio_out = sio_hw->gpio_out |  MRDY_MASK; }  // MRDY Higth(NO-WAIT)

// IRQ# direct SIO control (for speed in busemu)
static inline void irq_low(void)  { sio_hw->gpio_out = sio_hw->gpio_out & ~IRQ_MASK; }  // IRQ# Low(ON)
static inline void irq_high(void) { sio_hw->gpio_out = sio_hw->gpio_out |  IRQ_MASK; }  // IRQ# Higth(OFF)


// Debug toggle for busemu test prints
#define TRACE_LOG_SIZE 1000
#define TRACE_COUNT 500
// uint32_t eq_countofs = 2490; // 100kHz
// uint32_t eq_countofs = 9990; // 400kHz RESET 
// uint32_t eq_countofs = 40000; // 400kHz + debug
bool log_state = false;     // true=log on, false = log off

static uint32_t __attribute__((aligned(4))) __attribute__((section(".scratch_x"))) loop_count[10] = {0,0,0,0,0,0,0,0,0,0};
static volatile uint32_t __attribute__((aligned(4))) trace_log[TRACE_LOG_SIZE];
static volatile uint32_t __attribute__((section(".scratch_x"))) debug_val[10] = {0,0,0,0,0,0,0,0,0,0};

// 通信専用のコア0,1で共有するUART通信のためのグローバル変数
static volatile uint8_t __attribute__((section(".scratch_x"))) uart_tx_data; // 送信データバッファ(6809からPicoへ: 1バイト)
static volatile uint8_t __attribute__((section(".scratch_x"))) uart_rx_data; // 受信データバッファ(Picoから6809へ: 1バイト)
static volatile bool __attribute__((section(".scratch_x"))) uart_tx_ready; // 送信可フラグ (true=Ready, false=Busy)
static volatile bool __attribute__((section(".scratch_x"))) uart_rx_ready; // 受信完了フラグ (true=Ready, false=Empty)
static volatile bool exit_flag = false;



#define TIMER_US    20000               // TIMER 20ms

static volatile uint8_t timer_enabled = 0;        // 0:false/2:true
// GPIO26のコントロールレジスタのアドレス (RP2040)
#define GPIO26_CTRL_ADDR (IO_BANK0_BASE + 0x0d4)
// アトミックSETエイリアスのアドレス
#define GPIO26_CTRL_SET  (GPIO26_CTRL_ADDR + 0x2000)
// アトミックCLRエイリアスのアドレス
#define GPIO26_CTRL_CLR  (GPIO26_CTRL_ADDR + 0x3000)
#define FORCE_LOW_MASK   (2u << 8)
#define PWM_SLICE       5

uint irq_slice_num;     // IRQ GPIO26はスライス5のチャンネルA
volatile bool emulation_done = false;


//
// busemu()
//
static void __time_critical_func(busemu)(void) {
    uint32_t eq_counter = 0;
    uint32_t log_count = 0;
    int debug_valcount = 0;
    while (true) {
        uint32_t gpio;
        while (!(read_gpio_all() & E_MASK)) {}  // E Hi 待ち
        gpio = read_gpio_all();
        uint16_t ad = (uint16_t)((gpio & A0_15_MASK) >> A0_15_BASE);
        uint8_t dat = (uint8_t)((read_gpio_all() & D0_MASK) >> D0_BASE);
        bool rw = gpio & RW_MASK;       // R/W#
        bool irq = gpio & IRQ_MASK;   // IRQ
        if ((ad & MMIO_MASK) != MMIO_BASE) {    // Memory access (MMIO=$FFD0-$FFDF)
            if (rw) {          // READ  
                dat = memory[ad];  
                drive_databus(dat);
                while (read_gpio_all() & E_MASK) {}  // E Low 待ち
                release_databus();
            } else {            // WRITE
                dat = (uint8_t)((read_gpio_all() & D0_MASK) >> D0_BASE);
                memory[ad] = dat;
                while (read_gpio_all() & E_MASK);     // E Low 待ち
            }
        } else {               // I/O access ($FFD0-$FFDF)
            mrdy_low();             // WAIT-ON
            if (rw) {               // READ  
                volatile uint8_t d = 0;
                if (ad == ACIA_STAT) {
                    if (uart_tx_ready) {
                        d = 2;   // bit1 TDRE set
                    }
                    if (uart_rx_ready) {
                        d = (d | 1);  // bit0 RDRF set
                    }
                } else if (ad == ACIA_DATA) {
                    if (uart_tx_ready) {
                        d = uart_rx_data;
                        uart_rx_ready = false;  // Clear RX ready flag after reading
                    } 
                } else if (ad == TIMER_REG) {
                    d = timer_enabled;          // 0:false/2:true
                    d |= irq ? 0 : 0x80;        // 0x80=true/ false=0
                } else if (ad >= SD_DATA && ad <= SD_LBA2) {
                    d = sd_emu_read(ad);
                }
                dat = d;
                drive_databus(dat);
                mrdy_high();                            // WAIT 解除
                while (read_gpio_all() & E_MASK) {}  // E Low 待ち
                release_databus();
            } else {            // WRITE
                uint8_t d = (uint8_t)((read_gpio_all() & D0_MASK) >> D0_BASE);
                if (ad == ACIA_DATA) {
                    uart_tx_data = d;
                    uart_tx_ready = false;  // Set TX ready flag
                } else if (ad == TIMER_REG) {
                    timer_enabled = (d & 2);    // timer ON:2/ OFF:0
                    if (timer_enabled) {
                        pwm_set_enabled(irq_slice_num, true);
                    } else {
                        pwm_set_enabled(irq_slice_num, false);
                    }
                    if (d & 0x80) {  // アトミックCLRエイリアスを叩き、OUTOVERを 0x0 (NORMAL) に戻す
                        *(volatile uint32_t *)GPIO26_CTRL_CLR =  FORCE_LOW_MASK;  // SIOの出力設定(1)が再び有効になりピンがHighに戻る
                    }
                } else if (ad >= SD_DATA && ad <= SD_LBA2) {
                    sd_emu_write(ad, d);
                }
                mrdy_high();                          // WAIT 解除
                while (read_gpio_all() & E_MASK);     // E Low 待ち
                dat = d;
            }
        }
#if 0
        // log_state = eq_counter >= eq_countofs;
        if (ad == TIMER_REG && dat == 0x82) {
            log_state = true;
        }
        if (log_state) {
             uint32_t d = (irq ? RESET_MASK : 0) | (rw ? RW_MASK : 0) | (dat << D0_BASE) | (ad & 0xFFFF);
             trace_log[log_count++] = d;
            if (log_count >= TRACE_COUNT) {
                emulation_done = true;
                break;
            }
        }
#endif
        eq_counter++;
        if (exit_flag) {
            break;
        }
    }
    emulation_done = true;
}



//
// --- UART Task (Core 0) ---
//
void uart_task(void) {
  printf("task UART start..\n\n");
  uart_tx_ready = true;  // 初期状態は送信可能
  uart_rx_ready = false; // 初期状態は受信なし

  while (true) {
    // 送信処理: (6809 -> USB) 6809がデータを書き込んで Busy になったら実行
    if (!uart_tx_ready) {
      if (tud_cdc_connected() && tud_cdc_write_available() > 0) {
        putchar(uart_tx_data);
        uart_tx_ready = true; // 送信完了（readyに戻す）
      }
    }
    // 受信処理(USB->6809) RX Readyがfalseの場合のみ入力をチェック
    if (!uart_rx_ready) {
        // printf("*");
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (c == 0x1C) { // Ctrl-\で終了
            break;
            }
            uart_rx_data = (uint8_t)c;
            uart_rx_ready = true;
            // printf("[%02X]", uart_rx_data);
        }
    }
    if (emulation_done) {
      break;
    }
    sleep_us(100); // 100us待機（CPU負荷を下げるため）
  }
  gpio_set_dir(RESET_PIN, GPIO_OUT);
  gpio_put(RESET_PIN, 0);
  printf("RESET-ON\n");
  sleep_ms(0);
  exit_flag = true;
  printf("exit_flag-ON\n");
}


//
// main()
//
int main() {
    uint32_t target_khz = TARGET_SYS_CLK_KHZ;
//    vreg_set_voltage(VREG_VOLTAGE_1_30);
//    vreg_set_voltage(VREG_VOLTAGE_1_25);
    sleep_ms(5);  // wait for voltage to stabilize
    bool clock_ok = set_sys_clock_khz(target_khz, true);

    stdio_init_all();

    // A0-A15: GP0-15 inputs, PULL_DOWN
    for (int i = 0; i < 16; i++) {
        int pin = A0_15_BASE + i;
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
    }

    // D0-D7: GP16-23 inputs, specific pulls to default 0x90 NOP when floating
    const uint8_t d_pins[8] = {
        D0_BASE, D0_BASE + 1, D0_BASE + 2, D0_BASE + 3,
        D0_BASE + 4, D0_BASE + 5, D0_BASE + 6, D0_BASE + 7};
    const bool d_pull_up[8] 
 //       = {false, true, false, false, true, false, false, false};  // 0x12 NOP
//        = {true, true, true, true, true, true, true, true}; // 0xFF
        = {false, false, false, false, false, false, false, false}; // 0x00
    for (int i = 0; i < 8; i++) {
        gpio_init(d_pins[i]);
        gpio_set_dir(d_pins[i], GPIO_IN);
        if (d_pull_up[i]) {
            gpio_pull_up(d_pins[i]);
        } else {
            gpio_pull_down(d_pins[i]);
        }
    }

    // GP24: E input, no pull
    gpio_init(E_PIN);
    gpio_set_dir(E_PIN, GPIO_IN);

    // GP25: R\W# input, no pull
    gpio_init(RW_PIN);
    gpio_set_dir(RW_PIN, GPIO_IN);
  
    // GP26: IRQ output
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_OUT);
    gpio_put(IRQ_PIN, 1);       // IRQ# H=OFF
    gpio_set_function(IRQ_PIN, GPIO_FUNC_SIO); 

    // PWMの設定 (20ms周期のDREQ発生源)
    // GPIO26はスライス5のチャンネルA 
    irq_slice_num = pwm_gpio_to_slice_num(IRQ_PIN);        // GPIO26 -> slice 5

    // 計算: 288MHz / 250 = 1.152MHz
    //    1.152MHz / 23040 (ラップ値) = 50Hz (ちょうど20ms周期)
    pwm_set_clkdiv(irq_slice_num, 250.0f); // 分周比 250
    pwm_set_wrap(irq_slice_num, 23039);    // ラップ値 23039 (計23040カウント) 
    pwm_set_irq_enabled(irq_slice_num, true);
    
   // DMAの設定 (PWMトリガーでレジスタを直接叩く)
    volatile uint32_t *gpio_ctrl_set =
        (volatile uint32_t *)((uintptr_t)&io_bank0_hw->io[IRQ_PIN].ctrl + REG_ALIAS_SET_BITS);
    // OUTOVERフィールド(bits 9:8)を 0x2 (LOW) にするための値
    static const uint32_t force_low_mask = (2u << 8); 
    int chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false); // 固定値(mask)を読み続ける
    channel_config_set_write_increment(&c, false); // 固定レジスタに書き続ける
    
    // PWMスライス5のラップ(20ms毎)をトリガーに設定 [11, 12]
    channel_config_set_dreq(&c, DREQ_PWM_WRAP0 + PWM_SLICE);  // PWMラップでトリガー

    dma_channel_configure(
        chan,
        &c,
        gpio_ctrl_set,                        // 書き込み先（アトミックSETエイリアス）
        &force_low_mask,                      // 読み込み元：Low強制マスク
        0xFFFFFFFF,                           // 転送回数 (最大)
        true                                  // 開始
    );

    // GP27: MRDY output, initial 1 (High: no wait)
    gpio_init(MRDY_PIN);
    gpio_set_dir(MRDY_PIN, GPIO_OUT);
    gpio_put(MRDY_PIN, 1);
 
    sleep_ms(3000);  // Wait for user to connect logic analyzer
    printf("EMUZ80_RP2040_PHEMU6809(multicomp09-compatible I/O) - 2.01\n");
    printf(" $FFD0-$FFD1  ACIA 6850 (status/data)\n");
    printf(" $FFD8-$FFDC  virtual SD (256B, image %u bytes)\n", (unsigned)SD_IMG_SIZE);
    printf(" $FFDD        TIMER (bit1 enable, bit7 IRQ clear/pending)\n");

    // GP28: RESET output, initial 1 (ON)
    gpio_init(RESET_PIN);
    gpio_set_dir(RESET_PIN, GPIO_OUT);
    gpio_put(RESET_PIN, 0);
    printf("RESET-ON\n");
    init_clk();
    enum vreg_voltage vrege = vreg_get_voltage();
    float vreg_volt = 1.1f;
    if (vrege == VREG_VOLTAGE_1_15) {
        vreg_volt = 1.15f;
    } else if (vrege == VREG_VOLTAGE_1_20) {
        vreg_volt = 1.20f;
    } else if (vrege == VREG_VOLTAGE_1_25) {
        vreg_volt = 1.25f;
    } else if (vrege == VREG_VOLTAGE_1_30) {
        vreg_volt = 1.30f;
    }

    printf("RP2040 %0.1fMHz, %0.2fV\n", target_khz / 1000.0f, vreg_volt);
    printf("CLK-ON: MC6809 %0.3fMHz(%0.3fMHz)\n", MC6809_CLK_HZ / 4000000.0f, MC6809_CLK_HZ / 1000000.0f);
    sleep_ms(500);

    memset(memory, 0x00, sizeof(memory));  // Clear memory 0x00

    // Load binaries into memory
    // テスト用プログラム（どれか1つを選択して有効化）
//    memcpy(memory + 0x0100, mc6809_test1, sizeof(mc6809_test1));     // 0x0080h をインクリメントするループ
//    memcpy(memory + 0x0100, mc6809_test3, sizeof(mc6809_test3));     // ACIA($FFD0-D1) 'A' output
//    memcpy(memory + 0x0100, mc6809_test3_1, sizeof(mc6809_test3_1));     // ACIA($FFD0-D1) Hello World output

    /* NitrOS-9 direct boot (same as nitros9-runtime make-boot-bins.py):
     *   track34.bin → $2600 (4608 bytes, magic "OS")
     *   bootvecs    → $FFF2 (SWI3..NMI stubs + RESET=$2602) */
    memcpy(memory + TRACK34_LOAD_ADDR, track34, TRACK34_SIZE);
    printf("track34 loaded to $%04X (size=%u, magic=%c%c)\n",
           TRACK34_LOAD_ADDR, (unsigned)TRACK34_SIZE,
           memory[TRACK34_LOAD_ADDR], memory[TRACK34_LOAD_ADDR + 1]);

    static const uint8_t bootvecs[BOOTVECS_SIZE] = {
        0x01, 0x00,  /* $FFF2 SWI3 */
        0x01, 0x03,  /* $FFF4 SWI2 */
        0x01, 0x0F,  /* $FFF6 FIRQ */
        0x01, 0x0C,  /* $FFF8 IRQ  */
        0x01, 0x06,  /* $FFFA SWI  */
        0x01, 0x09,  /* $FFFC NMI  */
        0x26, 0x02,  /* $FFFE RESET → REL */
    };
    memcpy(memory + BOOTVECS_ADDR, bootvecs, BOOTVECS_SIZE);
    printf("bootvecs loaded to $%04X (RESET=$%02X%02X)\n",
           BOOTVECS_ADDR, memory[0xFFFE], memory[0xFFFF]);

    sd_emu_init();
    printf("sd_img ready (%u bytes @ LBA0, 256B blocks)\n", (unsigned)SD_IMG_SIZE);

    // printf("0xFFFF=%02X\n", memory[0xFFFF]);
    // Wait for user keypress (hit-any-key)
    printf("hit-any-key\n");
    int ch;
    while ((ch = getchar_timeout_us(100 * 1000)) == PICO_ERROR_TIMEOUT) {
        tight_loop_contents();
    }
    printf("Start..\n");
    add_alarm_in_us(100000, reset_release_callback, NULL, false); // 100ms後にリセットをOFF

    emulation_done = false;
    // Launch optimized assembly variant on core1
    multicore_launch_core1(busemu);
    printf("timer_enabled:%d\n", timer_enabled);
    uart_task();  // Run UART task on core0 (main core)
    
    printf("0xFFFF=%02X\n", memory[0xFFFF]);
    printf("0x6000=%02X %02X\n", memory[0x6000], memory[0x6001]);
    printf("PWM-OFF\n");
    pwm_set_enabled(clk_slice_num, false);
    printf("END\n");

#if 0
    for (int i = 0; i < TRACE_COUNT; i++) {
        uint32_t gpio = trace_log[i];
        uint16_t ad = (uint16_t)((gpio & A0_15_MASK) >> A0_15_BASE);
        uint8_t dat = (uint8_t)((gpio & D0_MASK) >> D0_BASE);
        bool rw = gpio & RW_MASK;
        bool irq = gpio & IRQ_MASK;
        // bool res = gpio & RESET_MASK;
        printf("%06d: A:%04X D:%02X RW:%d I:%d\n",
               i, ad, dat, rw ? 1 : 0, irq ? 1 : 0);
       sleep_ms(1);
        
    }
#endif
    // Idle loop
    while (true) {
        tight_loop_contents();
        sleep_ms(1);
    }
    return 0;
}

