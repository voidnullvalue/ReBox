#ifndef HR54_PRESENTATION_POLICY_H
#define HR54_PRESENTATION_POLICY_H

/* HDMI-validated foreground depth on the stock HR54 configuration. Stock
 * surfaces were observed at 110 and 1000; depth 0 was physically invisible.
 * Applies only to our own surfaces. See docs/NATIVE_UI_FINDINGS.md.
 */
#define HR54_NATIVE_FOREGROUND_DEPTH 2000
#define HR54_PRESENTATION_STRINGIFY_(value) #value
#define HR54_PRESENTATION_STRINGIFY(value) HR54_PRESENTATION_STRINGIFY_(value)
#define HR54_NATIVE_FOREGROUND_DEPTH_ARG HR54_PRESENTATION_STRINGIFY(HR54_NATIVE_FOREGROUND_DEPTH)

#endif
