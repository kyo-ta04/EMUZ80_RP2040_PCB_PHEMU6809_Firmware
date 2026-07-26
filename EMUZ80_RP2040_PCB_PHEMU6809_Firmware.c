/*
 * EMUZ80_RP2040_PCB_PHEMU6809_Firmware
 *
 * @tendai22plus さんの EMUZ80_RP2040_PCB と
 * @comoneko さん作の PHEMU6809 を使って、
 * Z80 DIP40 に MC6809P のピンを組み替えて動作させるための
 * RP2040用 ファームウェアです。 
 * 
 *
 * 秋月電子通商 AE-RP2040 ボード上で動作。
 * RP2040 が 64KB RAM + MC6850 互換 ACIA ($8018-19) をエミュレート。
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
#include "hardware/structs/sio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "tusb.h" // TinyUSB

#include "basic9.h"

// === Easy Overclock Configuration ===
// Change this one value to switch clock speed (under 300MHz recommended)
// Common good values: 200000, 250000, 266667, 280000
#define TARGET_SYS_CLK_KHZ 288000
// #define TARGET_SYS_CLK_KHZ 125000

#define A0_15_BASE  0
#define A0_15_MASK  (0xFFFFu << A0_15_BASE)   // GPIO0-15
#define D0_BASE     16
#define D0_MASK     (0xFFu << D0_BASE)        // GPIO16-23
#define E_PIN       24  // GP24: MC6809 E in
#define RW_PIN      25  // GP25: MC6809 R/W# in H=Read/L=Write
#define Q_PIN       26  // GP26: MC6809 Q in
#define MRDY_PIN    27  // GP27: MC6809 MRDY in L=WAIT
#define RESET_PIN   28  // GP28: RESET out L=Reset
#define CLK_PIN     29  // GP29: CLK (Clock) out PWM

#define E_MASK      (1u << E_PIN)
#define Q_MASK      (1u << Q_PIN)
#define RW_MASK     (1u << RW_PIN)
#define RDY_MASK    (1u << MRDY_PIN)
#define RESET_MASK  (1u << RESET_PIN)
#define CLK_MASK    (1u << CLK_PIN)

// Memory MAP
// $0000-$FFFF    ($0000-$1FFF RAM 8K, $C000-$FFFF ROM 16K)
// $8018-$8019  ACIA 6850

#define ACIA_STAT   0x8018
#define ACIA_DATA   0x8019


#define MEMORY_SIZE 65536

volatile bool emulation_done = false;


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

// $100 -
static uint8_t mc6809_test3[] = {
  0xb6,0x80,0x18,0x85,0x02,0x27,0xf9,0x86,0x41,0xb7,0x80,0x19,0x20,0xfe
};

// Hello World $0100-
static uint8_t mc6809_test3_1[] = {
  0x8e,0x01,0x15,0xa6,0x80,0x27,0x0c,0xf6,0x80,0x18,0xc5,0x02,0x27,0xf9,0xb7,0x80,
  0x19,0x20,0xf0,0x20,0xfe,0x0d,0x0a,0x48,0x65,0x6c,0x6c,0x6f,0x20,0x4d,0x43,0x36,
  0x38,0x30,0x39,0x20,0x57,0x6f,0x72,0x6c,0x64,0x21,0x0d,0x0a,0x00
};


static uint8_t bin_fffe[] = {
    0x01, 0x00  // 0100 START
};


static int64_t reset_release_callback(alarm_id_t id, void *user_data) {
    gpio_set_dir(RESET_PIN, GPIO_IN);
    printf("RESET-OFF\n\n\n");
    return 0;
}


// MC6809 に供給するクロック周波数（Hz）
// #define MC6809_CLK_HZ 12000000       // 12MHzHz
// #define MC6809_CLK_HZ 11000000       // 11MHzHz
// #define MC6809_CLK_HZ 10000000       // 10MHzHz
// #define MC6809_CLK_HZ 8000000       // 8MHzHz
#define MC6809_CLK_HZ 4000000       // 4MHzHz
// #define MC6809_CLK_HZ 400000     // 400,000Hz
// #define MC6809_CLK_HZ 100000     // 100,000Hz


uint slice_num;
static void init_clk(void) {
    gpio_set_function(CLK_PIN, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(CLK_PIN);
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

    pwm_set_clkdiv(slice_num, divider);
    pwm_set_wrap(slice_num, wrap);
    pwm_set_chan_level(slice_num, chan, (wrap + 1) / 2);  // 50% duty
    pwm_set_enabled(slice_num, true);

    // デバッグ用：設定後の実際の周波数を概算表示
    float achieved_hz = (float)sys_clk / ((wrap + 1) * divider);
    printf("PWM: slice=%u chan=%u wrap=%u div=%.4f target=%luHz achieved≈%.1fHz\n",
           slice_num, chan, wrap, (double)divider, (unsigned long)target_freq, (double)achieved_hz);
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


// Debug toggle for busemu test prints
#define TRACE_LOG_SIZE 1000
#define TRACE_COUNT 200
// uint32_t eq_countofs = 2490; // 100kHz
uint32_t eq_countofs = 9990; // 400kHz RESET 
// uint32_t eq_countofs = 40000; // 400kHz + debug


static uint32_t __attribute__((aligned(4))) __attribute__((section(".scratch_x"))) loop_count[10] = {0,0,0,0,0,0,0,0,0,0};
static volatile uint32_t __attribute__((aligned(4))) trace_log[TRACE_LOG_SIZE];
static volatile uint8_t __attribute__((section(".scratch_x"))) debug_val[10] = {0,0,0,0,0,0,0,0,0,0};

// 通信専用のコア0,1で共有するUART通信のためのグローバル変数
static volatile uint8_t __attribute__((section(".scratch_x"))) uart_tx_data; // 送信データバッファ(6809からPicoへ: 1バイト)
static volatile uint8_t __attribute__((section(".scratch_x"))) uart_rx_data; // 受信データバッファ(Picoから6809へ: 1バイト)
static volatile bool __attribute__((section(".scratch_x"))) uart_tx_ready; // 送信可フラグ (true=Ready, false=Busy)
static volatile bool __attribute__((section(".scratch_x"))) uart_rx_ready; // 受信完了フラグ (true=Ready, false=Empty)
static volatile bool exit_flag = false;


static void __time_critical_func(busemu)(void) {
    uint32_t eq_counter = 0;
    uint32_t log_count = 0;
#if BUSEMU_TEST_DEBUG
    printf("busemu_test: start\n");
#endif
    while (true) {
        uint32_t gpio;
        while (!(read_gpio_all() & Q_MASK)) {}  // Q Hi 待ち
        gpio = read_gpio_all();
        uint16_t ad = (uint16_t)((gpio & A0_15_MASK) >> A0_15_BASE);
        uint8_t dat = (uint8_t)((read_gpio_all() & D0_MASK) >> D0_BASE);
        bool rw = gpio & RW_MASK;       // R/W#
        bool res = gpio & RESET_MASK;   // RESET
        if (!((ad & 0xFFF0) == 0x8010)) {    // Memory access
            if (rw) {          // READ  
                dat = memory[ad];             // NOP 
                drive_databus(dat);
                while (!(read_gpio_all() & E_MASK)) {}  // E Hi 待ち
                while (read_gpio_all() & E_MASK) {}  // E Low 待ち
                release_databus();
            } else {            // WRITE
                while (!(read_gpio_all() & E_MASK)) {}  // E Hi 待ち
                while (read_gpio_all() & Q_MASK) {}     // Q Low 待ち
                dat = (uint8_t)((read_gpio_all() & D0_MASK) >> D0_BASE);
                memory[ad] = dat;
                while (read_gpio_all() & E_MASK) {}  // E Low 待ち
            }
        } else {               // I/O access
            if (rw) {          // READ  
                volatile int8_t d = 0;
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
                }
                dat = d;
                drive_databus(dat);
                while (!(read_gpio_all() & E_MASK)) {}  // E Hi 待ち
                while (read_gpio_all() & E_MASK) {}  // E Low 待ち
                release_databus();
            } else {            // WRITE
                while (!(read_gpio_all() & E_MASK)) {}  // E Hi 待ち
                while (read_gpio_all() & Q_MASK) {}     // Q Low 待ち
                dat = (uint8_t)((read_gpio_all() & D0_MASK) >> D0_BASE);
                if (ad == ACIA_DATA) {
                    uart_tx_data = dat;
                    uart_tx_ready = false;  // Set TX ready flag
                } 
                while (read_gpio_all() & E_MASK) {}  // E Low 待ち
            }
        }
#if 0
        if (eq_counter >= eq_countofs) {
             uint32_t d = (res ? RESET_MASK : 0) | (rw ? RW_MASK : 0) | (dat << D0_BASE) | (ad & 0xFFFF);
             trace_log[log_count++] = d;
            if (log_count >= TRACE_COUNT) {
                emulation_done = true;
                break;
            }

        }
        eq_counter++;
#endif
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

int main() {
    uint32_t target_khz = TARGET_SYS_CLK_KHZ;
    vreg_set_voltage(VREG_VOLTAGE_1_30);
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
  
    // GP26: Q input, no pull
    gpio_init(Q_PIN);
    gpio_set_dir(Q_PIN, GPIO_IN);

    // GP27: MRDY output, initial 1 (High: no wait)
    gpio_init(MRDY_PIN);
    gpio_set_dir(MRDY_PIN, GPIO_OUT);
    gpio_put(MRDY_PIN, 1);

    sleep_ms(3000);  // Wait for user to connect logic analyzer
    printf("EMUZ80_RP2040_PHEMU6809 - 0.01\n");
     // GP28: RESET output, initial 1 (ON)
    gpio_init(RESET_PIN);
    gpio_set_dir(RESET_PIN, GPIO_OUT);
    gpio_put(RESET_PIN, 0);
    printf("RESET-ON\n");
    init_clk();
    printf("CLK-ON: %0.3f MHz\n", MC6809_CLK_HZ / 1000000.0f);
    sleep_ms(500);

    memset(memory, 0x00, sizeof(memory));  // Clear memory 0x00
//    memory[0xFFFE] = 0x01;
//    memory[0xFFFF] = 0x00;

    // Load binaries into memory

    // テスト用プログラム（どれか1つを選択して有効化）
//    memcpy(memory + 0x0100, mc6809_test1, sizeof(mc6809_test1));     // 0x0080h をインクリメントするループ
//    memcpy(memory + 0x0100, mc6809_test3, sizeof(mc6809_test3));     // ACIA($8018-19) 'A' output
//    memcpy(memory + 0x0100, mc6809_test3_1, sizeof(mc6809_test3_1));     // ACIA($8018-19) Hello World output
    printf("BASIC9 loaded to memory (size=%d bytes)\n", ROM_SIZE);
    memcpy(memory + 0xC000, ROM_DATA, ROM_SIZE);   // BASIC9
   

    // Wait for user keypress (hit-any-key)
    printf("hit-any-key\n");
    int ch;
    while ((ch = getchar_timeout_us(100 * 1000)) == PICO_ERROR_TIMEOUT) {
        tight_loop_contents();
    }
    printf("Start..\n");

   
    add_alarm_in_us(100000, reset_release_callback, NULL, false);

    emulation_done = false;
    // Launch optimized assembly variant on core1
    multicore_launch_core1(busemu);
    uart_task();  // Run UART task on core0 (main core)
    printf("PWM-OFF\n");
    pwm_set_enabled(slice_num, false);
    printf("END\n");

#if 0
    for (int i = 0; i < TRACE_COUNT; i++) {
        uint32_t gpio = trace_log[i];
        uint16_t ad = (uint16_t)((gpio & A0_15_MASK) >> A0_15_BASE);
        uint8_t dat = (uint8_t)((gpio & D0_MASK) >> D0_BASE);
        bool rw = gpio & RW_MASK;
        bool res = gpio & RESET_MASK;
        printf("%06d: A:%04X D:%02X RW:%d R:%d\n",
               i + eq_countofs, ad, dat, rw ? 1 : 0, res ? 1 : 0);
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

