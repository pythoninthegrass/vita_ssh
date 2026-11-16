#ifndef INPUT_H
#define INPUT_H

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>

#define MAX_TEXT_LENGTH SCE_IME_DIALOG_MAX_TEXT_LENGTH

int kb(const uint16_t* title, uint16_t* initial_text, uint16_t* output_buffer, int password_mode);

void utf16_to_utf8(const uint16_t* src, char* dst, size_t dst_size);

/* The IME takes its initialText as UTF-16, so a recalled ASCII entry has to be
 * widened before it can prefill a field. dst is MAX_TEXT_LENGTH + 1 wide. */
void utf8_to_utf16(const char* src, uint16_t* dst);

#endif
