#pragma once 
#include "../ClockManager/Clock.h"
#include "../ConsoleManager/Console.h"


int ClockUI = 0;

static void init_ui(){
    ClockUI = clock_create(1000, "ClockUI");
}

static void show_fps_con(){
    double get_fps = clock_get_fps(ClockUI);
    char str[32];
    snprintf(str, sizeof(str), "%.1f FPS", get_fps);
    con_println_color(COL_RED, str);
}