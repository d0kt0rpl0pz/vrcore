/*
 * entry.cpp -- main() for every app target (vrcore_add_app adds it). SDL_main.h renames
 * main to SDL_main where the platform needs it (Android's activity calls SDL_main in
 * libmain.so).
 */
#include <SDL3/SDL_main.h>

#include "app.h"

int main(int argc, char **argv)
{
    return vrcore::run(vrcore::front_info(), argc, argv);
}
