#ifndef RTL8139RE_VERSION_H
#define RTL8139RE_VERSION_H

#define VERSION   0
#define REVISION  1

/* Match the original rtl8139.device string layout so tools that check
 * $VER see something familiar. Suffix -RE marks this as the C
 * reconstruction, not the shipping Hyperion binary. */
#define VSTRING   "rtl8139re.device 0.1-RE (31.7.2026)"
#define VERSTAG   "\0$VER: " VSTRING

#endif
