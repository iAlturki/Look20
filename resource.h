#ifndef LOOK20_RESOURCE_H
#define LOOK20_RESOURCE_H

/* Resource ids */
#define IDI_APPICON                 1

/* Tray callback message (must be in WM_APP range) */
#define WM_TRAYICON                 (WM_APP + 1)

/* Menu command ids */
#define IDM_HEADER                  100   /* disabled status line */
#define IDM_PAUSE                   101
#define IDM_BREAK_NOW               102
#define IDM_RESET                   103
#define IDM_SOUND                   104
#define IDM_AUTOSTART               105
#define IDM_PAUSE_IDLE              106
#define IDM_SKIP_FULLSCREEN         107
#define IDM_ABOUT                   108
#define IDM_EXIT                    109
#define IDM_ALL_MONITORS            110

/* Work interval radio group (id = base + minutes) */
#define IDM_WORK_BASE               1000
/* Break length radio group (id = base + seconds) */
#define IDM_BREAK_BASE              2000
/* Snooze length radio group (id = base + minutes) */
#define IDM_SNOOZE_BASE             3000
/* Position radio group (id = base + POS_* enum value) */
#define IDM_POS_BASE                4000

#endif /* LOOK20_RESOURCE_H */
