#include <MK64F12.h>

#define MASK(x) (1UL << (x))

// ============================================================================
// 1. FUNCTION PROTOTYPES (Modular Structure by Hardware Component)
// ============================================================================
void Init_GPIO(void);
void Init_ADC0(void);
void Init_Interrupts(void);
void Init_SysTick(void);

uint8_t Read_ADC0_Pot(void);
void Check_Potentiometer_Delay(void);
void Check_Switches_ISR(void);
void Trigger_Intrusion(void);

void Control_Laser(uint8_t state);
void Set_Buzzer(uint8_t on);
void ShiftOut_Byte(uint8_t data);
void Refresh_7Segment_ISR(void);
void Update_Display_Buffer(void);
void Update_LEDs(void);
void Delay_Us_Tone(uint32_t us);
void Play_Tone(uint32_t half_period_us, uint32_t duration_ms, uint8_t stop_if_safe);
void Play_Melody(uint8_t melody_id);

// ============================================================================
// 2. HARDWARE LOOKUP TABLES & SECURITY CONFIGURATION
// ============================================================================
// 7-Segment Common Anode (Active Low: 0 = ON, 1 = OFF, Bit 7 = Decimal Point)
const uint8_t SEG_NUM[10] = {
    0xC0, 0xF9, 0xA4, 0xB0, 0x99, // 0, 1, 2, 3, 4
    0x92, 0x82, 0xF8, 0x80, 0x90  // 5, 6, 7, 8, 9
};

#define CHAR_BLANK 0xFF
#define CHAR_S     0x92  // 'S'
#define CHAR_A     0x88  // 'A'
#define CHAR_F     0x8E  // 'F'
#define CHAR_E     0x86  // 'E'
#define CHAR_R_LOW 0xAF  // 'r'
#define CHAR_O     0xC0  // 'O'
#define CHAR_B_LOW 0x83  // 'b'
#define CHAR_P     0x8C  // 'P'
#define CHAR_DASH  0xBF  // '-'
#define CHAR_C     0xC6  // 'C'
#define CHAR_D_LOW 0xA1  // 'd'

const uint8_t DIGIT_POS[4] = {0x01, 0x02, 0x04, 0x08};

// 4-Key Secret Passcode to disarm: S1 -> S3 -> S2 -> S1
const uint8_t SECRET_SEQ[4] = {1, 3, 2, 1};

volatile uint32_t ms_ticks = 0;
volatile uint32_t last_btn_tick = 0;
volatile uint32_t last_ldr_tick = 0;
volatile uint32_t rearm_until_tick = 1500; // 1.5s startup stabilization

// Switch hold counters (3000 ms for long-press actions)
volatile uint16_t s1_hold_ms = 0;
volatile uint16_t s2_hold_ms = 0;
volatile uint16_t s3_hold_ms = 0;
volatile uint8_t  s3_long_handled = 0;

// Feature toggles & counters
volatile uint8_t laser_enabled    = 1; // S1 3s hold: 1 = Solid Laser ON, 0 = OFF
volatile uint8_t ldr_enabled      = 1; // S2 3s hold: 1 = LDR Active, 0 = Disabled
volatile uint8_t intrusion_count  = 0; // S3 short press: Total breach counter (0..99)
volatile uint8_t entry_delay_sec  = 6; // Default 6s countdown (adjustable 0..9s via VR-A0)

// System States: 0 = SAFE, 1 = ENTRY_COUNTDOWN, 2 = ROB_ALARM, 3 = SYSTEM_OFF
volatile uint8_t system_state = 0;
volatile uint8_t countdown_tenths = 0;
volatile uint8_t seq_index = 0;
volatile uint8_t play_sound_req = 0; // 1 = Key OK, 2 = SAFE melody, 3 = Wrong key

// Temporary 7-Segment status messages
volatile uint32_t err_display_until = 0;
volatile uint32_t temp_msg_until = 0;
volatile uint8_t  temp_msg_type = 0; // 1 = C-XX, 4 = d-XX

uint8_t display_buf[4] = {CHAR_S, CHAR_A, CHAR_F, CHAR_E};

// ============================================================================
// 3. INTERRUPT SERVICE ROUTINES (ISR) & TIMING TASKS
// ============================================================================
void Trigger_Intrusion(void) {
    if (system_state != 0) return;

    seq_index = 0;
    err_display_until = 0;
    temp_msg_until = 0;
    if (intrusion_count < 99) intrusion_count++;

    if (entry_delay_sec > 0) {
        countdown_tenths = entry_delay_sec * 10;
        system_state = 1; // Start Entry Delay Countdown (6.0s default)
    } else {
        system_state = 2; // Immediate ROB Alarm
    }
}

void Check_Switches_ISR(void) {
    // 1) Hold S1 (PTB3) for 3000 ms in SAFE mode -> Toggle Solid Laser ON/OFF silently
    if (system_state == 0 && (PTB->PDIR & MASK(3)) == 0) {
        if (s1_hold_ms < 3000) {
            s1_hold_ms++;
            if (s1_hold_ms == 3000) {
                laser_enabled = !laser_enabled;
                Control_Laser(laser_enabled);
                if (laser_enabled) {
                    rearm_until_tick = ms_ticks + 500; // 0.5s grace period when turned back ON
                    PORTC->ISFR = MASK(2);             // Clear pending LDR flag
                }
            }
        }
    } else {
        s1_hold_ms = 0;
    }

    // 2) Hold S2 (PTB10) for 3000 ms in SAFE mode -> Toggle LDR Sensor ON/OFF silently
    if (system_state == 0 && (PTB->PDIR & MASK(10)) == 0) {
        if (s2_hold_ms < 3000) {
            s2_hold_ms++;
            if (s2_hold_ms == 3000) {
                ldr_enabled = !ldr_enabled;
                if (ldr_enabled) {
                    rearm_until_tick = ms_ticks + 500; // 0.5s grace period when re-enabled
                    PORTC->ISFR = MASK(2);             // Clear pending LDR flag
                }
            }
        }
    } else {
        s2_hold_ms = 0;
    }

    // 3) S3 (PTB11):
    //    - Hold 3000 ms -> Toggle Master OFF (everything off, 7-seg shows "OFF ") / SAFE
    //    - Short press in SAFE -> Show Intrusion Count ("C-XX")
    if ((system_state == 0 || system_state == 3) && (PTB->PDIR & MASK(11)) == 0) {
        if (s3_hold_ms < 3000) {
            s3_hold_ms++;
            if (s3_hold_ms == 3000) {
                s3_long_handled = 1;
                if (system_state == 0) {
                    // Enter Master OFF state: cut Laser, Buzzer, and all LEDs
                    system_state = 3;
                    temp_msg_until = 0;
                    Control_Laser(0);
                    Set_Buzzer(0);
                    PTD->PSOR = MASK(0) | MASK(1) | MASK(2) | MASK(3);
                } else {
                    // Wake up from OFF back to SAFE state
                    system_state = 0;
                    rearm_until_tick = ms_ticks + 1500; // 1.5s stabilization grace period
                    PORTC->ISFR = MASK(2);
                    Control_Laser(laser_enabled);
                }
            }
        }
    } else {
        if (s3_hold_ms >= 50 && s3_hold_ms < 3000 && !s3_long_handled && system_state == 0) {
            temp_msg_type = 1; // Show "C-XX" intrusion count
            temp_msg_until = ms_ticks + 2000;
        }
        s3_hold_ms = 0;
        s3_long_handled = 0;
    }
}

void SysTick_Handler(void) {
    ms_ticks++;

    // 1) Multiplex 4-digit 7-Segment Display at 250 Hz
    Refresh_7Segment_ISR();

    // 2) Monitor 3-second hold on S1, S2, S3
    Check_Switches_ISR();

    // 3) Entry Delay Countdown (State 1): Decrement every 100 ms (0.1s)
    if (system_state == 1 && (ms_ticks % 100 == 0)) {
        if (countdown_tenths > 0) {
            countdown_tenths--;
        } else {
            system_state = 2; // Time expired -> Escalate to full ROB Alarm
        }
    }

    // 4) LED Animations during Countdown (1) and ROB Alarm (2)
    if (system_state == 1 && (ms_ticks % 200 == 0)) {
        static uint8_t warn_toggle = 0;
        warn_toggle = !warn_toggle;
        if (warn_toggle) PTD->PCOR = MASK(0) | MASK(1) | MASK(2) | MASK(3);
        else             PTD->PSOR = MASK(0) | MASK(1) | MASK(2) | MASK(3);
    } else if (system_state == 2 && (ms_ticks % 120 == 0)) {
        static uint8_t siren_toggle = 0;
        siren_toggle = !siren_toggle;
        if (siren_toggle) {
            PTD->PCOR = MASK(1) | MASK(3); // D1, D2 ON (Active Low)
            PTD->PSOR = MASK(2) | MASK(0); // D3, D4 OFF
        } else {
            PTD->PSOR = MASK(1) | MASK(3); // D1, D2 OFF
            PTD->PCOR = MASK(2) | MASK(0); // D3, D4 ON
        }
    }
}

// External GPIO Interrupt for Switches S1 (PTB3), S2 (PTB10), S3 (PTB11)
void PORTB_IRQHandler(void) {
    uint32_t flags = PORTB->ISFR;
    PORTB->ISFR = flags;

    if ((ms_ticks - last_btn_tick) < 180) return;
    last_btn_tick = ms_ticks;

    // Accept secret passcode during Entry Countdown (1) or ROB Alarm (2)
    if (system_state == 1 || system_state == 2) {
        uint8_t pressed_btn = 0;
        if (flags & MASK(3))       pressed_btn = 1; // S1
        else if (flags & MASK(10)) pressed_btn = 2; // S2
        else if (flags & MASK(11)) pressed_btn = 3; // S3

        if (pressed_btn != 0) {
            if (pressed_btn == SECRET_SEQ[seq_index]) {
                seq_index++;
                err_display_until = 0;
                if (seq_index >= 4) {
                    // Passcode complete -> Disarm immediately to SAFE
                    system_state = 0;
                    seq_index = 0;
                    rearm_until_tick = ms_ticks + 3000; // 3-second grace period
                    Set_Buzzer(0);
                    PORTC->ISFR = MASK(2);
                    play_sound_req = 2;                 // Play SAFE Victory Melody
                } else {
                    play_sound_req = 1;                 // Short key-accepted beep
                }
            } else {
                // Wrong button -> Show "Err " and reset sequence
                seq_index = 0;
                err_display_until = ms_ticks + 600;
                play_sound_req = 3;
            }
        }
    }
}

// External GPIO Interrupt for LDR Light Sensor on Header ~6 (PTC2)
void PORTC_IRQHandler(void) {
    if (PORTC->ISFR & MASK(2)) {
        PORTC->ISFR = MASK(2);

        if (system_state != 0) return;
        if (!laser_enabled || !ldr_enabled) return;
        if (ms_ticks < rearm_until_tick) return;
        if ((ms_ticks - last_ldr_tick) < 200) return;
        last_ldr_tick = ms_ticks;

        Trigger_Intrusion();
    }
}

// ============================================================================
// 4. HARDWARE INITIALIZATION FUNCTIONS
// ============================================================================
void Init_GPIO(void) {
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTB_MASK |
                  SIM_SCGC5_PORTC_MASK | SIM_SCGC5_PORTD_MASK;

    // 1) Shield LEDs D1-D4 (PTD1, PTD3, PTD2, PTD0) -> Active Low
    PORTD->PCR[0] = PORT_PCR_MUX(1);
    PORTD->PCR[1] = PORT_PCR_MUX(1);
    PORTD->PCR[2] = PORT_PCR_MUX(1);
    PORTD->PCR[3] = PORT_PCR_MUX(1);
    PTD->PDDR |= MASK(0) | MASK(1) | MASK(2) | MASK(3);
    PTD->PSOR |= MASK(0) | MASK(1) | MASK(2) | MASK(3);

    // 2) Shield Buzzer (PTA1) -> High-Z Input (OFF) to prevent 5V->3.3V hum
    PORTA->PCR[1] = PORT_PCR_MUX(1);
    PTA->PDDR &= ~MASK(1);

    // 3) Laser Diode Module KY-008 on Header ~5 (PTA2) -> Output (Continuous ON)
    PORTA->PCR[2] = PORT_PCR_MUX(1);
    PTA->PDDR |= MASK(2);
    PTA->PSOR |= MASK(2);

    // 4) 74HC595 Shift Register pins (SDI=PTC12, SFTCLK=PTC3, LCHCLK=PTB23)
    PORTC->PCR[12] = PORT_PCR_MUX(1);
    PORTC->PCR[3]  = PORT_PCR_MUX(1);
    PORTB->PCR[23] = PORT_PCR_MUX(1);
    PTC->PDDR |= MASK(12) | MASK(3);
    PTB->PDDR |= MASK(23);
}

void Init_ADC0(void) {
    // Enable ADC0 clock and configure PTB2 (Shield Potentiometer VR-A0) as Analog
    SIM->SCGC6 |= SIM_SCGC6_ADC0_MASK;
    PORTB->PCR[2] = PORT_PCR_MUX(0);

    // 8-bit conversion mode, Bus clock / 4
    ADC0->CFG1 = ADC_CFG1_MODE(0) | ADC_CFG1_ADIV(2);
    ADC0->SC2  = 0;
}

void Init_Interrupts(void) {
    __disable_irq();

    // 1) Switches S1 (PTB3), S2 (PTB10), S3 (PTB11) -> Falling-Edge Interrupt + Pull-up
    PORTB->PCR[3]  = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);
    PORTB->PCR[10] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);
    PORTB->PCR[11] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);
    PTB->PDDR &= ~(MASK(3) | MASK(10) | MASK(11));
    PORTB->ISFR = MASK(3) | MASK(10) | MASK(11);
    NVIC_EnableIRQ(PORTB_IRQn);

    // 2) LDR Sensor on Header ~6 (PTC2) -> Rising-Edge Interrupt + Pull-up
    PORTC->PCR[2] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0x9);
    PTC->PDDR &= ~MASK(2);
    PORTC->ISFR = MASK(2);
    NVIC_EnableIRQ(PORTC_IRQn);

    __enable_irq();
}

void Init_SysTick(void) {
    SysTick->LOAD = 20480 - 1; // 1 ms tick at 20.48 MHz
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk   |
                    SysTick_CTRL_ENABLE_Msk;
}

// ============================================================================
// 5. MODULE DRIVER FUNCTIONS
// ============================================================================
uint8_t Read_ADC0_Pot(void) {
    ADC0->SC1[0] = 12; // Channel 12 = PTB2 (VR-A0)
    while ((ADC0->SC1[0] & ADC_SC1_COCO_MASK) == 0);
    return (uint8_t)ADC0->R[0];
}

void Check_Potentiometer_Delay(void) {
    static uint8_t initialized = 0;
    static uint8_t last_raw = 0;

    uint8_t raw = Read_ADC0_Pot();
    uint8_t new_delay = (uint8_t)(((uint16_t)raw * 10) / 256);
    if (new_delay > 9) new_delay = 9;

    if (!initialized) {
        initialized = 1;
        last_raw = raw;
        entry_delay_sec = 6; // Keep default 6-second countdown until VR-A0 is turned
        return;
    }

    // Update delay only when user actively turns potentiometer VR-A0
    int16_t diff = (int16_t)raw - (int16_t)last_raw;
    if (diff > 15 || diff < -15) {
        last_raw = raw;
        if (new_delay != entry_delay_sec) {
            entry_delay_sec = new_delay;
            if (system_state == 0) {
                temp_msg_type = 4; // Display "d-00" .. "d-09"
                temp_msg_until = ms_ticks + 1500;
            }
        }
    }
}

void Control_Laser(uint8_t state) {
    if (state) PTA->PSOR = MASK(2); // Solid Laser ON
    else       PTA->PCOR = MASK(2); // Laser OFF
}

void Set_Buzzer(uint8_t on) {
    if (on) {
        PTA->PCOR = MASK(1);   // Drive LOW (Active Low) to turn Buzzer ON
        PTA->PDDR |= MASK(1);
    } else {
        PTA->PDDR &= ~MASK(1); // High-Z Input: completely cuts off 5V PNP transistor
    }
}

void ShiftOut_Byte(uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        if (data & (1 << i)) PTC->PSOR = MASK(12);
        else                 PTC->PCOR = MASK(12);
        PTC->PSOR = MASK(3);
        PTC->PCOR = MASK(3);
    }
}

void Refresh_7Segment_ISR(void) {
    static uint8_t digit_idx = 0;
    PTB->PCOR = MASK(23);
    ShiftOut_Byte(display_buf[digit_idx]);
    ShiftOut_Byte(DIGIT_POS[digit_idx]);
    PTB->PSOR = MASK(23);
    digit_idx = (digit_idx + 1) % 4;
}

void Update_Display_Buffer(void) {
    if (system_state == 3) {
        // Master OFF State: 7-Segment displays "OFF " only
        display_buf[0] = CHAR_O;
        display_buf[1] = CHAR_F;
        display_buf[2] = CHAR_F;
        display_buf[3] = CHAR_BLANK;
    } else if (system_state == 0) {
        // Show temporary status message if active (C-XX, d-XX)
        if (ms_ticks < temp_msg_until) {
            if (temp_msg_type == 1) {
                display_buf[0] = CHAR_C;
                display_buf[1] = CHAR_DASH;
                display_buf[2] = SEG_NUM[intrusion_count / 10];
                display_buf[3] = SEG_NUM[intrusion_count % 10];
            } else if (temp_msg_type == 4) {
                display_buf[0] = CHAR_D_LOW;
                display_buf[1] = CHAR_DASH;
                display_buf[2] = SEG_NUM[0];
                display_buf[3] = SEG_NUM[entry_delay_sec];
            }
        } else {
            display_buf[0] = CHAR_S;
            display_buf[1] = CHAR_A;
            display_buf[2] = CHAR_F;
            display_buf[3] = CHAR_E;
        }
    } else {
        // During Countdown (1) or ROB Alarm (2): Show Passcode Progress or Error
        if (ms_ticks < err_display_until) {
            display_buf[0] = CHAR_E;
            display_buf[1] = CHAR_R_LOW;
            display_buf[2] = CHAR_R_LOW;
            display_buf[3] = CHAR_BLANK;
        } else if (seq_index > 0) {
            display_buf[0] = CHAR_P;
            display_buf[1] = CHAR_DASH;
            display_buf[2] = SEG_NUM[seq_index];
            display_buf[3] = CHAR_BLANK;
        } else if (system_state == 1) {
            // Entry Delay Countdown "06.0" -> "00.0"
            display_buf[0] = SEG_NUM[0];
            display_buf[1] = SEG_NUM[countdown_tenths / 10] & 0x7F;
            display_buf[2] = SEG_NUM[countdown_tenths % 10];
            display_buf[3] = CHAR_BLANK;
        } else {
            // Full ROB Alarm -> Blink "rOb "
            if ((ms_ticks / 250) % 2 == 0) {
                display_buf[0] = CHAR_R_LOW;
                display_buf[1] = CHAR_O;
                display_buf[2] = CHAR_B_LOW;
                display_buf[3] = CHAR_BLANK;
            } else {
                display_buf[0] = CHAR_BLANK;
                display_buf[1] = CHAR_BLANK;
                display_buf[2] = CHAR_BLANK;
                display_buf[3] = CHAR_BLANK;
            }
        }
    }
}

void Update_LEDs(void) {
    if (system_state == 3) {
        // Master OFF State: Turn off all Shield LEDs (D1-D4)
        PTD->PSOR = MASK(0) | MASK(1) | MASK(2) | MASK(3);
    } else if (system_state == 0) {
        // D4 (PTD0): ON in SAFE Armed mode
        PTD->PCOR = MASK(0);

        // D1 (PTD1): Live laser alignment indicator
        if (laser_enabled && ldr_enabled && ((PTC->PDIR & MASK(2)) == 0)) {
            PTD->PCOR = MASK(1); // D1 ON
        } else {
            PTD->PSOR = MASK(1); // D1 OFF
        }

        // D2 (PTD3): Laser status toggled ON/OFF by holding S1 for 3 seconds
        if (laser_enabled) PTD->PCOR = MASK(3);
        else               PTD->PSOR = MASK(3);

        // D3 (PTD2): LDR status toggled ON/OFF by holding S2 for 3 seconds
        if (ldr_enabled)   PTD->PCOR = MASK(2);
        else               PTD->PSOR = MASK(2);
    }
}

void Delay_Us_Tone(uint32_t us) {
    for (uint32_t i = 0; i < us * 3; i++) __NOP();
}

void Play_Tone(uint32_t half_period_us, uint32_t duration_ms, uint8_t stop_if_safe) {
    uint32_t start_tick = ms_ticks;
    while ((ms_ticks - start_tick) < duration_ms) {
        if (stop_if_safe && system_state != 1 && system_state != 2) break;
        Set_Buzzer(1);
        Delay_Us_Tone(half_period_us);
        Set_Buzzer(0);
        Delay_Us_Tone(half_period_us);
    }
    Set_Buzzer(0);
}

void Play_Melody(uint8_t melody_id) {
    if (melody_id == 1) {
        Play_Tone(550, 45, 0);     // Key accepted beep
    } else if (melody_id == 2) {
        // SAFE Victory Melody (C5 - E5 - G5 - C6)
        Play_Tone(1911, 110, 0);
        Play_Tone(1517, 110, 0);
        Play_Tone(1275, 110, 0);
        Play_Tone(955,  240, 0);
    } else if (melody_id == 3) {
        Play_Tone(2400, 120, 0);   // Wrong key low buzz
    } else if (melody_id == 4) {
        Play_Tone(700,  110, 1);
        Play_Tone(1100, 110, 1);
    }
    Set_Buzzer(0);
}

// ============================================================================
// 6. MAIN FUNCTION
// ============================================================================
int main(void) {
    uint32_t last_adc_tick = 0;
    uint32_t last_warn_beep_tick = 0;

    Init_GPIO();
    Init_ADC0();
    Init_Interrupts();
    Init_SysTick();

    while (1) {
        if (system_state == 3) {
            // Master OFF State: Keep Laser and Buzzer strictly OFF
            Control_Laser(0);
            Set_Buzzer(0);
        } else {
            Control_Laser(laser_enabled);

            // Sample potentiometer VR-A0 every 80 ms for Entry Delay setting
            if ((ms_ticks - last_adc_tick) >= 80) {
                last_adc_tick = ms_ticks;
                Check_Potentiometer_Delay();
            }
        }

        Update_Display_Buffer();
        Update_LEDs();

        if (play_sound_req != 0) {
            uint8_t snd = play_sound_req;
            play_sound_req = 0;
            Play_Melody(snd);
            Set_Buzzer(0);
        }

        if (system_state == 1) {
            // Entry Delay Countdown beep every 500 ms
            if ((ms_ticks - last_warn_beep_tick) >= 500) {
                last_warn_beep_tick = ms_ticks;
                Play_Tone(950, 35, 1);
            } else {
                Set_Buzzer(0);
            }
        } else if (system_state == 2) {
            Play_Melody(4); // Continuous siren
        } else {
            Set_Buzzer(0);  // 100% silent in SAFE and OFF modes
        }
    }
}