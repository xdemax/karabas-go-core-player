// A small append-only log on the SD card so a play session can be analysed later
// (the user copies PLRLOG.TXT back). Writes are buffered: the SD must never stall the
// audio FIFO, so a line just goes into RAM and the buffer is flushed rarely.
#pragma once
#include <stdint.h>

int  plog_open(const char *path);        // 0 ok; creates/overwrites the file
void plog_close(void);                   // flush and close
int  plog_is_open(void);
void plog_str(const char *s);
void plog_num(uint32_t v);
void plog_snum(int32_t v);
void plog_flush(void);                   // push the buffer to the card (safe to call often)
