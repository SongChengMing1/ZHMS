#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <stdint.h>

#define LCD_DISPLAY_EVENT_CLOCK (1U << 0)
#define LCD_DISPLAY_EVENT_WIFI  (1U << 1)
#define LCD_DISPLAY_EVENT_HEART (1U << 2)
#define LCD_DISPLAY_EVENT_PAGE  (1U << 3)

void lcd_display_init(void);
void lcd_display_enable(void);
void lcd_display_post_event(uint32_t events);

#endif /* LCD_DISPLAY_H */
