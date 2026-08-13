#pragma once 
#include "../ClockManager/Clock.h"
#include "../ConsoleManager/Console.h"



int ClockUI = 0;

static void init_ui(){
    ClockUI = clock_create(1000, "ClockUI");
}

static void show_fps(){
    float get_fps = clock_get_fps(ClockUI);
    char str[32]; // Ensure the buffer is large enough
    snprintf(str, sizeof(str), "%f", get_fps);
    con_println_color(COL_RED,str);
}