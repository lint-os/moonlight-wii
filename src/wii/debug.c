#ifdef DEBUG
#include <ogc/system.h>
#include <ogc/usbgecko.h>
#include <sys/iosupport.h>
#include <reent.h>
#include <stdio.h>
#include <unistd.h>

static int gecko_chn = -1;

// Every printf/fprintf lands here once we point devoptab_list[STD_OUT/ERR] at
// dotab_gecko. Emit the bytes to the USB Gecko, expanding \n -> \r\n so the
// host terminal breaks the line (a bare \n is not always rendered). We use a
// .write_r devoptab (like libogc's own "soc"/"uart" tabs) rather than
// CON_EnableGecko's dotab_stdout, which only sets the non-reentrant .write and
// is not reliably dispatched. usb_sendbuffer_safe() blocks until the gecko FIFO
// drains, so no line (or its newline) is dropped when the host reads slower than
// we print. A per-call stack buffer keeps concurrent printfs from the various
// threads from clobbering each other.
static ssize_t gecko_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
  (void) r;
  (void) fd;
  if (gecko_chn < 0)
    return (ssize_t) len;

  size_t off = 0;
  while (off < len) {
    char buf[256];
    size_t pos = 0;
    while (off < len && pos + 2 < sizeof(buf)) {
      char c = ptr[off++];
      if (c == '\n') {
        buf[pos++] = '\r';
        buf[pos++] = '\n';
      } else {
        buf[pos++] = c;
      }
    }
    if (pos)
      usb_sendbuffer_safe(gecko_chn, buf, pos);
  }
  return (ssize_t) len;
}

static const devoptab_t dotab_gecko = {
  .name = "gecko",
  .write_r = gecko_write,
};

void Debug_Init(void)
{
  // Dolphin OSReport (EXI ch0) — the logger whenever no gecko is present.
  SYS_STDIO_Report(true);

  // Find a USB Gecko on either EXI channel (slot A = 0, slot B = 1). Probe a
  // few times: gecko enumeration can lag behind boot.
  for (int i = 0; i < 20; i++) {
    if (usb_isgeckoalive(0)) { gecko_chn = 0; break; }
    if (usb_isgeckoalive(1)) { gecko_chn = 1; break; }
    usleep(10000);
  }

  if (gecko_chn >= 0) {
    devoptab_list[STD_OUT] = &dotab_gecko;
    devoptab_list[STD_ERR] = &dotab_gecko;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("[debug] USB gecko active on EXI channel %d\n", gecko_chn);
  } else {
    printf("[debug] no USB gecko found; OSReport only\n");
  }
}
#endif
