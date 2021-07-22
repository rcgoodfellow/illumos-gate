#ifndef	_UART_H
#define	_UART_H

typedef enum async_databits {
	AD_5BITS = 0x1100001,
	AD_6BITS = 0x1100002,
	AD_7BITS = 0x1100003,
	AD_8BITS = 0x1100004,
	AD_9BITS = 0x1100005
} async_databits_t;

typedef enum async_stopbits {
	AS_1BIT = 0x1200000,
	AS_15BITS = 0x1200001,
	AS_2BITS = 0x1200002
} async_stopbits_t;

typedef enum async_parity {
	AP_NONE = 0x1300000,
	AP_EVEN = 0x1300001,
	AP_ODD = 0x1300002,
	AP_MARK = 0x1300003,
	AP_SPACE = 0x1300004
} async_parity_t;

extern void putstr(const uintptr_t, const char *);
extern size_t getline(const uintptr_t, char *, size_t);

#endif	/* _UART_H */
