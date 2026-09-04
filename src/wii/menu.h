#pragma once

// A small on-screen menu framework driven by the Wiimote D-pad + A/B.
//
//   Up/Down  move the selection
//   A        confirm
//   B        go back / cancel
//
// Text entry (IP address, integers) has no keyboard on the Wii, so each
// character is picked with Up/Down and the cursor moved with Left/Right.

// Selection list. Returns the chosen index, or -1 if cancelled (B).
int menu_select(const char* title, const char** options, int count, int sel);

// Enter a string one character at a time. `charset` is the set of characters a
// position may take (Up/Down cycles it); Left/Right moves the cursor. Copies the
// result into `out` (NUL-terminated, at most `maxlen` chars; `out` must be at
// least `maxlen + 1` bytes). Returns 1 if confirmed (A), 0 if cancelled (B).
int menu_input_string(const char* title, const char* charset,
                      char* out, int maxlen, const char* initial);

// Enter a non-negative integer (digits only). Returns the value, or -1 if
// cancelled.
int menu_input_int(const char* title, int initial);

// Yes/No prompt. Returns 1 (yes) or 0 (no), or -1 if cancelled.
int menu_select_bool(const char* title, int initial);
