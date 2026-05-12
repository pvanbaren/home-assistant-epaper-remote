#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

// Compat shim for the stale `-Wl,--wrap=log_printf` flag in
// pioarduino's framework-arduinoespressif32-libs/esp32s3/flags/ld_flags.
// Arduino-ESP32's ESP_LOGx macros expand to log_printf(fmt, ...).
// IDF 5.5.4 renamed log_printf to esp_log_printf with a new signature,
// so we can't alias the symbol. Two earlier attempts failed:
//   - vprintf:        works, but the full newlib stdio chain (vfprintf →
//                     __sprint_r → __sfvwrite_r → _fflush_r → __swrite →
//                     console_write → write → uart_write → lock_acquire)
//                     was ~14 frames deep and tripped task stack canaries
//                     under CORE_DEBUG_LEVEL=5 on battery / ui / touch.
//   - esp_rom_vprintf: tiny stack, but doesn't implement %f / %#010lx
//                     (garbled output) and spin-blocks on UART0 FIFO from
//                     ISR context, tripping the interrupt watchdog.
// vsnprintf to a stack-local buffer + a single write() syscall keeps
// stack bounded (~256 B buf + newlib format internals), supports full
// printf syntax, and routes through the VFS so output reaches CDC.
int __wrap_log_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) {
        return n;
    }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    write(STDOUT_FILENO, buf, len);
    return n;
}
