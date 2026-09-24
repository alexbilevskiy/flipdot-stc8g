#include "fw_hal.h"

/**
 * Flip-dot quad firmware — STC8G1K08 (TSSOP20), replaces the stock loader.
 *
 * Reads the four TM1824 duty-state outputs and drives the dot coils through
 * four AT8837 H-bridges with pulse-then-coast semantics: confirmed state
 * changes produce a burst of one or more short coil pulses with all changed
 * bridges driven in parallel (one pulse slot serves any number of channels;
 * serialized per-channel pulses queue up and miss flips at fast frame
 * rates), then the bridges brake, coast and sleep. The burst count (N) is 1
 * by default; marginal dots that a single pulse cannot flip reliably are
 * hammered with repeated pulses until they catch. The dots are magnetically
 * bistable, so steady current is ~zero in any image — the stock firmware
 * instead holds coils energized continuously.
 *
 * Pin map (traced, identical on all 16 quads of a module):
 *   TM1824 duty outputs (open-drain, internal 4k pull-ups enabled):
 *     R = P3.6, G = P3.7, B = P5.4, W = P5.5
 *   AT8837 IN1/IN2 (push-pull), bridge k: IN1 = P1.(2k-2), IN2 = P1.(2k-1):
 *     bridge1 P1.0/P1.1, bridge2 P1.2/P1.3, bridge3 P1.4/P1.5, bridge4 P1.6/P1.7
 *   AT8837 nSLEEP (active high): bridge1 P3.2, bridge2 P3.3,
 *     bridge3 P3.4, bridge4 P3.5
 *   P3.0/P3.1 = factory ISP header RX/TX — used by the tuning console below
 *   (ISP programming still works: the ROM bootloader owns the pins at boot).
 *
 * Runs on the power-on default IRC (~24 MHz); no SYS_SetClock / IRC re-trim
 * (the FwLib trim constants are per-chip; 24 MHz -> 115200 baud error ~0.2%).
 *
 * Tuning console (115200 8N1, line-based, the ISP header):
 *   S        status (pulse, brake, burst, gap, polarity, auto mode, dot states)
 *   P<n>     coil pulse width in ms (default 100, tune down for the minimum
 *            reliable flip)
 *   B<n>     brake time in us (coil shorted after the pulse)
 *   N<n>     burst count: sub-pulses per transition, 1 = single pulse
 *            (default 1; raise for dots a single pulse cannot flip)
 *   G<n>     coast gap between burst sub-pulses in ms
 *   X        toggle bridge polarity
 *   T        test cycle: pulse all dots to black, then to white (prints
 *            phase markers, 1 s pause between)
 *   F / R    raw probe: one pulse, IN1-high / IN2-high direction; optional
 *            bridge number 1..4 ("R2" = bridge 2 only, plain "R" = all four)
 *   D        dump one duty window: low counts per channel + classification
 *   M        toggle auto mode (duty-edge pulsing). Turn OFF for bench tests
 *            with no ESP frames — otherwise the state machine "corrects"
 *            dots back to the classified duty ~16 ms after every manual
 *            pulse and garbles the observations
 *   V        toggle verbose edge logging ("e0->B" on every state-machine pulse)
 *   ?        help
 */

/* ---- tunables (defaults; P/B/X change them at runtime) ---- */
#define PULSE_MS_DEF  100   /* coil pulse length, ms (10 ms flips work but are
                               marginal; 50 ms was the long-time default; an
                               A/B of two modules at 100 ms vs 50 ms showed no
                               difference in flip behavior, so 100 ms stays as
                               the default) */
#define PULSE_MS_MAX  200   /* console P<n> clamp */
#define BRAKE_US_DEF  200   /* coil shorted decay after the pulse, us */
#define BURST_N_DEF   1     /* sub-pulses per transition (1 = single pulse);
                               marginal dots that one pulse cannot flip get
                               hammered until they catch */
#define BURST_N_MAX   10    /* console N<n> clamp */
#define BURST_GAP_DEF 20    /* coast gap between burst sub-pulses, ms */
#define BURST_GAP_MAX 200   /* console G<n> clamp */
#define WAKE_US       40    /* nSLEEP high -> bridge active (spec max 35 us) */
#define WINDOW_TICKS  8000  /* sampling window = 8000 Timer0 ticks = 4 ms =
                               4 PWM periods (Timer0 at SYSclk/12 = 2 MHz on
                               the ~24 MHz IRC). Hardware-timed: the window
                               duration does NOT depend on the speed of the
                               sampling loop (a counted loop compiled with
                               --model-large ran ~50 ms here and phase-locked
                               the state machine to the 100 ms frame rate) */
#define DEBOUNCE_WIN  2     /* consecutive windows confirming a new state */

/* duty classification from the low-share of one window */
#define TH_BLACK_NUM  3     /* lows >= 3/4 of window -> black */
#define TH_BLACK_DEN  4
#define TH_WHITE_NUM  1     /* lows <= 1/4 of window -> white */
#define TH_WHITE_DEN  4

#define NSLEEP_MASK(i)    ((uint8_t)(1 << ((i) + 2)))       /* P3.2..P3.5 */
#define BRIDGE_IN_MASK(i) ((uint8_t)(3 << ((i) * 2)))       /* P1.(2i)/P1.(2i+1) */

static uint8_t pulse_ms = PULSE_MS_DEF;
static uint16_t brake_us = BRAKE_US_DEF;
static uint8_t burst_n = BURST_N_DEF;
static uint16_t burst_gap_ms = BURST_GAP_DEF;
static uint8_t pol_flip = 1;  /* confirmed on the bench: IN2-high (R) = black, IN1-high (F) = white */
static uint8_t auto_mode = 1; /* duty-edge pulsing on/off (M) */
static uint8_t verbose = 1;   /* edge pulse logging (V) */

static uint8_t state[4];      /* 0 = white, 1 = black */
static uint8_t cand_cnt[4];

static void bridge_coast(uint8_t i) { P1 &= (uint8_t)~BRIDGE_IN_MASK(i); }
static void bridge_brake(uint8_t i) { P1 |= BRIDGE_IN_MASK(i); }

static void bridge_pulse_dir(uint8_t i, uint8_t to_black)
{
    uint8_t m = BRIDGE_IN_MASK(i);
    /* forward (IN1 high / IN2 low) = black pulse unless polarity is flipped */
    uint8_t v = (uint8_t)(((to_black ^ pol_flip) ? 0x01 : 0x02) << ((i) * 2));
    P1 = (uint8_t)((P1 & ~m) | v);
}

static void pulse_mask(uint8_t black_mask, uint8_t white_mask)
{
    uint8_t i, n, mask = (uint8_t)(black_mask | white_mask);

    if (!mask)
        return;
    for (i = 0; i < 4; i++)
        if (mask & (uint8_t)(1u << i))
            P3 |= NSLEEP_MASK(i);         /* wake all changed bridges */
    SYS_DelayUs(WAKE_US);
    for (n = 0; n < burst_n; n++)
    {
        for (i = 0; i < 4; i++)
            if (mask & (uint8_t)(1u << i))
                bridge_pulse_dir(i, (uint8_t)((black_mask >> i) & 1u));
        SYS_Delay(pulse_ms);
        for (i = 0; i < 4; i++)
            if (mask & (uint8_t)(1u << i))
                bridge_brake(i);          /* short the coils: controlled decay */
        SYS_DelayUs(brake_us);
        for (i = 0; i < 4; i++)
            if (mask & (uint8_t)(1u << i))
                bridge_coast(i);          /* Hi-Z; dots hold while the disc settles */
        if (n + 1 < burst_n && burst_gap_ms)
            SYS_Delay(burst_gap_ms);      /* settle between sub-pulses */
    }
    for (i = 0; i < 4; i++)
        if (mask & (uint8_t)(1u << i))
            P3 &= (uint8_t)~NSLEEP_MASK(i);   /* sleep */
}

/* duty sampling window, timer-gated: counts lows per channel and the total
 * samples taken in exactly WINDOW_TICKS; classification is ratio-based, so
 * it is immune to the loop's actual sample rate. The counters live in __data
 * (direct addressing) — in XRAM every increment went through the generic
 * pointer routines and made the loop an order of magnitude slower. */
static uint16_t __data sw_lows[4];
static uint16_t __data sw_total;

static void window_timer_init(void)
{
    AUXR &= (uint8_t)~0x80;   /* T0x12 = 0: SYSclk/12 = 2 MHz ticks */
    TMOD &= (uint8_t)0xF0;    /* T0 mode 0: 16-bit auto-reload, timer */
    TL0 = 0x80;               /* 65536 - 8000 -> TF0 every 4 ms */
    TH0 = 0xE0;
    TR0 = 1;
}

static void sample_window(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
        sw_lows[i] = 0;
    sw_total = 0;
    TF0 = 0;
    do
    {
        if (!(P3 & 0x40)) sw_lows[0]++;   /* P3.6 = R */
        if (!(P3 & 0x80)) sw_lows[1]++;   /* P3.7 = G */
        if (!(P5 & 0x10)) sw_lows[2]++;   /* P5.4 = B */
        if (!(P5 & 0x20)) sw_lows[3]++;   /* P5.5 = W */
        sw_total++;
    } while (!TF0);
}

/* 0 = white, 1 = black, 2 = hold previous (mid-range duty) */
static uint8_t classify(uint16_t lows, uint16_t total)
{
    if ((uint32_t)lows * TH_BLACK_DEN >= (uint32_t)total * TH_BLACK_NUM)
        return 1;
    if ((uint32_t)lows * TH_WHITE_DEN <= (uint32_t)total * TH_WHITE_NUM)
        return 0;
    return 2;
}

/* ---- tuning console ---- */

#define LINE_LEN 16
static volatile char line_buf[LINE_LEN];
static volatile uint8_t line_len = 0;
static volatile uint8_t line_ready = 0;

INTERRUPT(UART1_Routine, EXTI_VectUART1)
{
    uint8_t c;
    if (RI)
    {
        UART1_ClearRxInterrupt();
        c = SBUF;
        if (c == '\r' || c == '\n')
        {
            if (line_len > 0)
                line_ready = 1;
        }
        else if (!line_ready && line_len < LINE_LEN - 1)
        {
            line_buf[line_len++] = (char)c;
        }
    }
}

static void console_init(void)
{
    // UART1 on the ISP header pins, baud 115200, source Timer2, rx interrupt
    UART1_Config8bitUart(UART1_BaudSource_Timer2, HAL_State_ON, 115200);
    UART1_SetRxState(HAL_State_ON);
    EXTI_UART1_SetIntState(HAL_State_ON);
    EXTI_Global_SetIntState(HAL_State_ON);
}

static void tx_dec(uint16_t v)
{
    char b[6];
    int8_t i = 0;
    if (v == 0)
    {
        UART1_TxChar('0');
        return;
    }
    while (v)
    {
        b[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i)
        UART1_TxChar(b[--i]);
}

static uint8_t parse_num(const char *s, uint8_t len, uint16_t *out)
{
    uint16_t v = 0;
    uint8_t i;
    if (len == 0)
        return 0;
    for (i = 0; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = (uint16_t)(v * 10 + (s[i] - '0'));
    }
    *out = v;
    return 1;
}

static void console_poll(void)
{
    char cmd[LINE_LEN];
    uint8_t len, i;
    uint16_t n;

    if (!line_ready)
        return;
    for (i = 0; i < line_len; i++)
        cmd[i] = line_buf[i];
    len = line_len;
    line_ready = 0;
    line_len = 0;

    switch (cmd[0])
    {
    case 'S':
        UART1_TxString("pulse_ms=");
        tx_dec(pulse_ms);
        UART1_TxString(" brake_us=");
        tx_dec(brake_us);
        UART1_TxString(" burst_n=");
        tx_dec(burst_n);
        UART1_TxString(" burst_gap_ms=");
        tx_dec(burst_gap_ms);
        UART1_TxString(" pol_flip=");
        tx_dec(pol_flip);
        UART1_TxString(" auto=");
        tx_dec(auto_mode);
        UART1_TxString(" state=");
        for (i = 0; i < 4; i++)
            UART1_TxChar(state[i] ? 'B' : 'W');
        UART1_TxString("\r\n");
        break;
    case 'P':
        if (parse_num(cmd + 1, len - 1, &n) && n >= 1 && n <= PULSE_MS_MAX)
        {
            pulse_ms = (uint8_t)n;
            UART1_TxString("pulse_ms=");
            tx_dec(pulse_ms);
            UART1_TxString("\r\n");
        }
        else
            UART1_TxString("P needs 1..200\r\n");
        break;
    case 'B':
        if (parse_num(cmd + 1, len - 1, &n) && n <= 5000)
        {
            brake_us = n;
            UART1_TxString("brake_us=");
            tx_dec(brake_us);
            UART1_TxString("\r\n");
        }
        else
            UART1_TxString("B needs 0..5000\r\n");
        break;
    case 'N':
        if (parse_num(cmd + 1, len - 1, &n) && n >= 1 && n <= BURST_N_MAX)
        {
            burst_n = n;
            UART1_TxString("burst_n=");
            tx_dec(burst_n);
            UART1_TxString("\r\n");
        }
        else
            UART1_TxString("N needs 1..10\r\n");
        break;
    case 'G':
        if (parse_num(cmd + 1, len - 1, &n) && n <= BURST_GAP_MAX)
        {
            burst_gap_ms = n;
            UART1_TxString("burst_gap_ms=");
            tx_dec(burst_gap_ms);
            UART1_TxString("\r\n");
        }
        else
            UART1_TxString("G needs 0..200\r\n");
        break;
    case 'X':
        pol_flip ^= 1;
        UART1_TxString("pol_flip=");
        tx_dec(pol_flip);
        UART1_TxString("\r\n");
        break;
    case 'T':
        UART1_TxString("T: all black\r\n");
        pulse_mask(0x0F, 0x00);
        for (i = 0; i < 4; i++)
            state[i] = 1;
        SYS_Delay(1000);
        UART1_TxString("T: all white\r\n");
        pulse_mask(0x00, 0x0F);
        for (i = 0; i < 4; i++)
            state[i] = 0;
        UART1_TxString("\r\ntest cycle done\r\n");
        break;
    case 'F': /* raw probe: one pulse, IN1-high direction */
    case 'R': /* raw probe: one pulse, IN2-high direction */
    {
        const uint8_t dir = (cmd[0] == 'F') ? 0x01 : 0x02;
        /* optional bridge number 1..4; plain F/R = all four */
        uint8_t single = 0xFF;
        if (len >= 2 && cmd[1] >= '1' && cmd[1] <= '4')
            single = cmd[1] - '1';
        for (i = 0; i < 4; i++)
        {
            if (single != 0xFF && single != i)
                continue;
            P3 |= NSLEEP_MASK(i);
        }
        SYS_DelayUs(WAKE_US);
        for (i = 0; i < 4; i++)
        {
            if (single != 0xFF && single != i)
                continue;
            {
                uint8_t m = BRIDGE_IN_MASK(i);
                P1 = (uint8_t)((P1 & ~m) | (uint8_t)(dir << ((i) * 2)));
            }
        }
        SYS_Delay(pulse_ms);
        for (i = 0; i < 4; i++)
        {
            if (single != 0xFF && single != i)
                continue;
            bridge_brake(i);
        }
        SYS_DelayUs(brake_us);
        for (i = 0; i < 4; i++)
        {
            if (single != 0xFF && single != i)
                continue;
            bridge_coast(i);
        }
        for (i = 0; i < 4; i++)
        {
            if (single != 0xFF && single != i)
                continue;
            P3 &= (uint8_t)~NSLEEP_MASK(i);
        }
        UART1_TxString(cmd[0] == 'F' ? "F (IN1-high) pulse done" : "R (IN2-high) pulse done");
        if (single != 0xFF)
        {
            UART1_TxString(" bridge ");
            UART1_TxChar(cmd[1]);
        }
        UART1_TxString("\r\n");
        break;
    }
    case 'D':
    {
        sample_window();
        UART1_TxString("lows R/G/B/W = ");
        for (i = 0; i < 4; i++)
        {
            tx_dec(sw_lows[i]);
            UART1_TxChar('/');
        }
        UART1_TxString(" of ");
        tx_dec(sw_total);
        UART1_TxString(" cls=");
        for (i = 0; i < 4; i++)
            UART1_TxChar(classify(sw_lows[i], sw_total) == 1 ? 'B' : (classify(sw_lows[i], sw_total) == 0 ? 'W' : '?'));
        UART1_TxString("\r\n");
        break;
    }
    case 'M':
    {
        auto_mode ^= 1;
        if (auto_mode)
        {
            /* re-sync the tracked states with the current duty, silently
             * (no pulses) — otherwise stale states fire false edges on the
             * first frame after re-enabling */
            uint8_t k;
            sample_window();
            for (k = 0; k < 4; k++)
            {
                state[k] = (classify(sw_lows[k], sw_total) == 1) ? 1 : 0;
                cand_cnt[k] = 0;
            }
        }
        UART1_TxString("auto=");
        tx_dec(auto_mode);
        UART1_TxString("\r\n");
        break;
    }
    case 'V':
        verbose ^= 1;
        UART1_TxString("verbose=");
        tx_dec(verbose);
        UART1_TxString("\r\n");
        break;
    default:
        UART1_TxString("S P<n> B<n> N<n> G<n> X T F[n] R[n] D M V ?\r\n");
        break;
    }
}

void main(void)
{
    uint8_t i, cls;
    uint8_t black_mask, white_mask;

    /* bridges off before anything else: IN pairs low, nSLEEP low (asleep).
     * After reset all I/O is Hi-Z anyway, so the bridges' own pull-downs
     * already keep them in sleep/coast until this init runs. */
    P1 = 0x00;
    GPIO_P1_SetMode(0xFF, GPIO_Mode_Output_PP);
    GPIO_P3_SetMode(GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5,
                    GPIO_Mode_Output_PP);
    P3 &= (uint8_t)~0x3C;

    /* duty inputs: quasi-bidirectional + internal 4k pull-up */
    GPIO_P3_SetMode(GPIO_Pin_6 | GPIO_Pin_7, GPIO_Mode_InOut_QBD);
    GPIO_P5_SetMode(GPIO_Pin_4 | GPIO_Pin_5, GPIO_Mode_InOut_QBD);
    GPIO_SetPullUp(GPIO_Port_3, GPIO_Pin_6 | GPIO_Pin_7, HAL_State_ON);
    GPIO_SetPullUp(GPIO_Port_5, GPIO_Pin_4 | GPIO_Pin_5, HAL_State_ON);

    console_init();
    window_timer_init();

    /* initial classification: taken silently, no pulses — the dots stay
     * where they are (a reflashed quad picks up the live image as is) */
    sample_window();
    for (i = 0; i < 4; i++)
    {
        state[i] = (classify(sw_lows[i], sw_total) == 1) ? 1 : 0;
        cand_cnt[i] = 0;
    }
    UART1_TxString("flipdot quad up\r\n");

    while (1)
    {
        console_poll();
        if (!auto_mode)
            continue;
        sample_window();
        black_mask = 0;
        white_mask = 0;
        for (i = 0; i < 4; i++)
        {
            cls = classify(sw_lows[i], sw_total);
            if (cls == 2 || cls == state[i])
            {
                cand_cnt[i] = 0;
            }
            else if (++cand_cnt[i] >= DEBOUNCE_WIN)
            {
                cand_cnt[i] = 0;
                if (cls)
                    black_mask |= (uint8_t)(1u << i);
                else
                    white_mask |= (uint8_t)(1u << i);
            }
        }
        if (!(black_mask | white_mask))
            continue;
        if (verbose)
        {
            for (i = 0; i < 4; i++)
            {
                if ((black_mask | white_mask) & (uint8_t)(1u << i))
                {
                    UART1_TxString("e");
                    UART1_TxChar('0' + i);
                    UART1_TxString((black_mask & (uint8_t)(1u << i)) ? "->B\r\n" : "->W\r\n");
                }
            }
        }
        for (i = 0; i < 4; i++)
        {
            if (black_mask & (uint8_t)(1u << i))
                state[i] = 1;
            else if (white_mask & (uint8_t)(1u << i))
                state[i] = 0;
        }
        pulse_mask(black_mask, white_mask);
    }
}
