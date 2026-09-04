#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>

void Font_Init(void);

void Font_Deinit(void);

void Font_Draw(void);

void Font_Draw_TVDRC(void);

void Font_Clear(void);

void Font_Printw(uint32_t x, uint32_t y, const wchar_t* string);

void Font_Print(uint32_t x, uint32_t y, const char* string);

void Font_Printf(uint32_t x, uint32_t y, const char* msg, ...);

void Font_SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void Font_SetSize(uint32_t size);

// Inset (in pixels) applied to all OSD text so it stays inside the TV's safe
// area on displays with overscan. 0 disables it.
void Font_SetOverscan(uint32_t margin);

uint8_t* Font_Buffer(void);

uint32_t Font_BufferSize(void);

void Font_PrintChar(uint32_t x, uint32_t y, char c);
