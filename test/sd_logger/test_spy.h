#ifndef TEST_SPY_H_
#define TEST_SPY_H_
#include "session_fsm.h"
#include "tia_reader.h"
typedef struct {
    uint32_t       tick;
    SessionState_t state;
    int  starts, stops, pauses, takes;
    uint8_t mask;
    bool running, paused;
    int  write_while_paused;
    TiaWindow_t win;
} LogSpy_t;
extern LogSpy_t g_spy;
#endif
